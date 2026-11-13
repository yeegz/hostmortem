// The playbooks. Each one encodes a documented edge failure mode (see
// docs/playbooks.md for the public incidents behind them) as:
//   for      evidence that raises the score,
//   against  evidence that rules the hypothesis out,
//   missing  telemetry that would be needed but was not collected.
// A playbook stays silent when nothing in the window concerns it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "analysis/correlator.hpp"
#include "core/kinds.hpp"

namespace hm {
namespace {

using Ev = const StoredEvent*;
using Evs = std::vector<Ev>;

constexpr std::int64_t kSec = 1'000'000'000LL;
constexpr std::int64_t kConcurrent = 15 * kSec;  // probes run every 10 s by default
constexpr std::size_t kMaxCited = 20;

Evs of_kind(const std::vector<StoredEvent>& events, const char* k) {
  Evs out;
  for (const auto& e : events) {
    if (e.event.kind == k) out.push_back(&e);
  }
  return out;
}

bool ok(Ev e) { return e->event.attr_bool("ok").value_or(true); }

Evs where(const Evs& in, bool want_ok) {
  Evs out;
  for (Ev e : in) {
    if (ok(e) == want_ok) out.push_back(e);
  }
  return out;
}

bool near(Ev a, Ev b, std::int64_t tol = kConcurrent) {
  return a->boot_ordinal == b->boot_ordinal && std::llabs(a->event.mono_ns - b->event.mono_ns) <= tol;
}

// Events in `cands` that happened close to at least one event in `anchors`.
Evs concurrent_with(const Evs& cands, const Evs& anchors) {
  Evs out;
  for (Ev c : cands) {
    for (Ev a : anchors) {
      if (near(c, a)) {
        out.push_back(c);
        break;
      }
    }
  }
  return out;
}

std::string cite(const Evs& evs, std::size_t max = 4) {
  std::ostringstream os;
  for (std::size_t i = 0; i < evs.size() && i < max; ++i) {
    if (i) os << ", ";
    os << '#' << evs[i]->event.id;
  }
  if (evs.size() > max) os << " +" << (evs.size() - max) << " more";
  return os.str();
}

void add(Hypothesis& h, const Evs& evs, EvidenceRole role, const std::string& note) {
  std::size_t n = 0;
  for (Ev e : evs) {
    if (n++ >= kMaxCited) break;
    h.evidence.push_back({e->event.id, role, note});
  }
}

void add_missing(Hypothesis& h, const std::string& note) {
  h.evidence.push_back({0, EvidenceRole::missing, note});
}

// Fixed thresholds: >= 4 supported, 1..3 possible. A decisive contradiction
// rules the hypothesis out regardless of score.
void finish(Hypothesis h, bool ruled_out, std::vector<Hypothesis>& out) {
  if (ruled_out) {
    h.verdict = Verdict::ruled_out;
  } else if (h.score >= 4) {
    h.verdict = Verdict::supported;
  } else if (h.score >= 1) {
    h.verdict = Verdict::possible;
  } else {
    return;
  }
  out.push_back(std::move(h));
}

std::string str(Ev e, const char* key) { return e->event.attr_str(key).value_or("?"); }

std::string pct(double ppm) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f %%", ppm / 10000.0);
  return buf;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

bool network_incident(const Window& w) {
  for (const auto& e : w.events) {
    const auto& k = e.event.kind;
    if ((k.rfind("probe.", 0) == 0 && !e.event.attr_bool("ok").value_or(true)) || k == kind::link_state ||
        k == kind::route_del || k == kind::kernel_link_down || k == kind::kernel_netdev_watchdog) {
      return true;
    }
  }
  return false;
}

Evs process_exits(const Window& w) {
  Evs out = of_kind(w.events, kind::process_exit);
  for (Ev e : of_kind(w.events, kind::process_restart)) out.push_back(e);
  return out;
}

// ---------------------------------------------------------------- DNS ----

void local_resolver(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs resolver = of_kind(w.events, kind::probe_dns_resolver);
  const Evs res_fail = where(resolver, false);
  if (res_fail.empty()) return;
  const Evs direct = of_kind(w.events, kind::probe_dns_direct);
  const Evs direct_ok = concurrent_with(where(direct, true), res_fail);
  const Evs direct_fail = concurrent_with(where(direct, false), res_fail);

  Hypothesis h{"local_resolver", "Local resolver path (nsswitch, stub resolver, systemd-resolved)", {}, 0, {}, {}, {}};
  h.score += res_fail.size() >= 2 ? 2 : 1;
  add(h, res_fail, EvidenceRole::for_, "getaddrinfo() through the system resolver failed");
  if (!direct_ok.empty()) {
    h.score += 3;
    add(h, direct_ok, EvidenceRole::for_, "a direct query to the upstream server succeeded at the same time");
  }
  bool ruled_out = false;
  if (!direct_fail.empty()) {
    add(h, direct_fail, EvidenceRole::against, "direct upstream queries failed too, so the fault is not only local");
    ruled_out = direct_fail.size() >= direct_ok.size();
  }
  if (direct.empty()) {
    add_missing(h, "no direct upstream DNS queries ran, so the local path cannot be separated from upstream");
    unobs.push_back({"direct upstream DNS", "no DNS servers configured or discovered for the prober"});
  }
  std::ostringstream os;
  os << "The system resolver failed " << res_fail.size() << " time(s) (" << cite(res_fail) << ")";
  if (!direct_ok.empty()) os << " while direct queries to the upstream server succeeded (" << cite(direct_ok) << ")";
  if (ruled_out) os << ", but direct queries failed at the same time (" << cite(direct_fail) << ")";
  os << '.';
  h.explanation = os.str();
  h.next_steps = {"resolvectl status (or cat /etc/resolv.conf) to see which stub is in use",
                  "check the hosts: line in /etc/nsswitch.conf",
                  "journalctl -u systemd-resolved around the incident window"};
  finish(std::move(h), ruled_out, out);
}

void upstream_dns(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>&) {
  const Evs direct = of_kind(w.events, kind::probe_dns_direct);
  const Evs direct_fail = where(direct, false);
  if (direct_fail.empty()) return;
  const Evs gw = of_kind(w.events, kind::probe_icmp);
  Evs gw_probes;
  for (Ev e : gw) {
    if (str(e, "role") == "gateway") gw_probes.push_back(e);
  }
  const Evs gw_ok = concurrent_with(where(gw_probes, true), direct_fail);
  const Evs gw_fail = concurrent_with(where(gw_probes, false), direct_fail);
  const Evs links_down = of_kind(w.events, kind::link_state);

  Hypothesis h{"upstream_dns", "Upstream DNS server or the path to it", {}, 0, {}, {}, {}};
  h.score += direct_fail.size() >= 2 ? 3 : 1;
  add(h, direct_fail, EvidenceRole::for_, "direct query to the upstream DNS server failed or timed out");
  const Evs res_fail = concurrent_with(where(of_kind(w.events, kind::probe_dns_resolver), false), direct_fail);
  if (!res_fail.empty()) {
    h.score += 1;
    add(h, res_fail, EvidenceRole::for_, "the system resolver failed at the same time, as expected if upstream is down");
  }
  if (!gw_ok.empty()) {
    h.score += 2;
    add(h, gw_ok, EvidenceRole::for_, "the default gateway answered ICMP, so the local link was up");
  }
  const bool ruled_out = !gw_fail.empty() && gw_fail.size() >= gw_ok.size();
  if (ruled_out) {
    add(h, gw_fail, EvidenceRole::against, "the gateway itself was unreachable: DNS failure is a consequence, not the cause");
  }
  if (gw_probes.empty()) {
    add_missing(h, "no gateway reachability probes, so an upstream DNS fault cannot be separated from a link fault: " +
                       w.avail.reason("prober.icmp"));
  }
  std::ostringstream os;
  os << "Direct queries to the upstream DNS server failed " << direct_fail.size() << " time(s) (" << cite(direct_fail) << ")";
  if (!gw_ok.empty()) os << " while the gateway stayed reachable (" << cite(gw_ok) << ")";
  if (ruled_out) os << ", but the gateway was unreachable too (" << cite(gw_fail) << ")";
  os << '.';
  h.explanation = os.str();
  h.next_steps = {"dig @<server> <name> from this host and from another host on the same network",
                  "check firewall rules for UDP/TCP 53 on the path (iptables -L -v -n, nft list ruleset)",
                  "compare with a second upstream server"};
  (void)links_down;
  finish(std::move(h), ruled_out, out);
}

// ------------------------------------------------------------- link ----

void link_or_gateway(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  if (!network_incident(w)) return;
  Evs down;
  for (Ev e : of_kind(w.events, kind::link_state)) {
    if (str(e, "ifname") == "lo") continue;
    const auto oper = str(e, "oper");
    if (oper == "down" || oper == "lowerlayerdown" || !e->event.attr_bool("carrier").value_or(true)) down.push_back(e);
  }
  Evs route_lost;
  for (Ev e : of_kind(w.events, kind::route_del)) {
    if (e->event.attr_bool("default").value_or(false)) route_lost.push_back(e);
  }
  const Evs klink = of_kind(w.events, kind::kernel_link_down);
  Evs gw;
  for (Ev e : of_kind(w.events, kind::probe_icmp)) {
    if (str(e, "role") == "gateway") gw.push_back(e);
  }
  const Evs gw_fail = where(gw, false);
  const Evs gw_ok = where(gw, true);

  Hypothesis h{"link_or_gateway", "Local link or default gateway lost", {}, 0, {}, {}, {}};
  if (!down.empty()) { h.score += 4; add(h, down, EvidenceRole::for_, "interface went down or lost carrier"); }
  if (!route_lost.empty()) { h.score += 2; add(h, route_lost, EvidenceRole::for_, "default route was removed"); }
  if (!klink.empty()) { h.score += 1; add(h, klink, EvidenceRole::for_, "kernel driver logged link down"); }
  if (gw_fail.size() >= 2) { h.score += 3; add(h, gw_fail, EvidenceRole::for_, "default gateway did not answer ICMP"); }
  else if (gw_fail.size() == 1) { h.score += 1; add(h, gw_fail, EvidenceRole::for_, "default gateway did not answer ICMP once"); }

  // Reachable throughout, and no link events: the local link is ruled out.
  const bool ruled_out = down.empty() && route_lost.empty() && gw_fail.empty() && gw_ok.size() >= 2;
  if (ruled_out) add(h, gw_ok, EvidenceRole::against, "the gateway answered every ICMP probe in the window");
  if (gw.empty()) {
    add_missing(h, "gateway reachability was not probed: " + w.avail.reason("prober.icmp"));
    unobs.push_back({"gateway reachability", w.avail.reason("prober.icmp")});
  }
  if (!w.avail.can_see("netlink")) {
    add_missing(h, "link and route changes were not observed: " + w.avail.reason("netlink"));
    unobs.push_back({"link and route changes", w.avail.reason("netlink")});
  }
  std::string text;
  if (ruled_out) {
    text = "The gateway answered throughout (" + cite(gw_ok) + ") and no interface or route change was seen.";
  } else {
    std::vector<std::string> parts;
    if (!down.empty()) parts.push_back("link down " + cite(down));
    if (!route_lost.empty()) parts.push_back("default route removed " + cite(route_lost));
    if (!gw_fail.empty()) parts.push_back("gateway unreachable " + cite(gw_fail));
    text = "Link evidence: ";
    for (std::size_t i = 0; i < parts.size(); ++i) text += (i ? "; " : "") + parts[i];
    if (parts.empty()) text += "none observed";
    text += '.';
  }
  h.explanation = text;
  h.next_steps = {"ip -s link show <ifname> for error and drop counters",
                  "check cabling, Wi-Fi association or modem state (mmcli -m 0) at the time",
                  "compare with the switch/AP logs for the same minute"};
  finish(std::move(h), ruled_out, out);
}

void path_degradation(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>&) {
  // Loss or latency to a target that is still partly reachable.
  std::map<std::string, Evs> by_target;
  for (Ev e : of_kind(w.events, kind::probe_icmp)) by_target[str(e, "target")].push_back(e);
  std::map<std::string, std::vector<double>> base_rtt;
  for (const auto& e : w.baseline) {
    if (e.event.kind == kind::probe_icmp && e.event.attr_bool("ok").value_or(false)) {
      base_rtt[e.event.attr_str("target").value_or("?")].push_back(e.event.attr_num("rtt_ms").value_or(0));
    }
  }
  Hypothesis h{"path_degradation", "Packet loss or latency on the network path", {}, 0, {}, {}, {}};
  std::ostringstream os;
  bool any = false;
  for (const auto& [target, evs] : by_target) {
    const Evs fail = where(evs, false);
    const Evs good = where(evs, true);
    if (!fail.empty() && !good.empty()) {
      h.score += 2;
      any = true;
      add(h, fail, EvidenceRole::for_, "intermittent ICMP loss to " + target);
      os << "ICMP to " << target << " lost " << fail.size() << " of " << evs.size() << " probes (" << cite(fail) << "). ";
    }
    std::vector<double> rtts;
    for (Ev e : good) rtts.push_back(e->event.attr_num("rtt_ms").value_or(0));
    const double base = median(base_rtt[target]);
    const double now = median(rtts);
    if (!rtts.empty() && base > 0 && now > std::max(3.0 * base, base + 50.0)) {
      h.score += 4;
      any = true;
      add(h, good, EvidenceRole::for_, "round-trip time to " + target + " far above its baseline");
      os << "Median RTT to " << target << " rose from " << std::lround(base) << " ms to " << std::lround(now)
         << " ms (" << cite(good) << "). ";
    }
  }
  if (!any) return;
  h.explanation = os.str();
  h.next_steps = {"mtr -rwc 100 <target> to find the hop where loss starts",
                  "tc -s qdisc show to check local queue drops",
                  "check Wi-Fi signal or LTE RSRP/SINR at the time"};
  finish(std::move(h), false, out);
}

// -------------------------------------------------------------- TCP ----

void retrans_storm(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs tcp = of_kind(w.events, kind::net_tcp);
  if (tcp.empty()) return;
  std::vector<double> base;
  for (const auto& e : w.baseline) {
    if (e.event.kind == kind::net_tcp && e.event.attr_int("out_segs").value_or(0) >= 50) {
      base.push_back(e.event.attr_num("retrans_ppm").value_or(0));
    }
  }
  const double base_ppm = median(base);
  const double threshold = std::max(20000.0, 3.0 * base_ppm);
  Evs high, flat;
  for (Ev e : tcp) {
    if (e->event.attr_int("out_segs").value_or(0) < 50) continue;
    (e->event.attr_num("retrans_ppm").value_or(0) > threshold ? high : flat).push_back(e);
  }
  if (high.empty() && !network_incident(w)) return;

  Hypothesis h{"retrans_storm", "TCP retransmission storm", {}, 0, {}, {}, {}};
  if (!high.empty()) {
    h.score += std::min<int>(2 + static_cast<int>(high.size()), 6);
    add(h, high, EvidenceRole::for_, "retransmitted segments above baseline");
  }
  Evs timeouts;
  for (Ev e : tcp) {
    if (e->event.attr_int("timeouts").value_or(0) > 0) timeouts.push_back(e);
  }
  if (!timeouts.empty() && !high.empty()) {
    h.score += 1;
    add(h, timeouts, EvidenceRole::for_, "TCP retransmission timeouts fired");
  }
  const bool ruled_out = high.empty() && flat.size() >= 2;
  if (ruled_out) add(h, flat, EvidenceRole::against, "retransmission ratio stayed at baseline");
  add_missing(h, "per-connection attribution unavailable: host-wide counters only (eBPF tcp_retransmit_skb arrives in v0.2)");
  if (high.empty() && flat.size() < 2) return;
  std::ostringstream os;
  if (ruled_out) {
    os << "Host-wide retransmissions stayed near baseline (" << pct(base_ppm) << ") across " << flat.size() << " samples (" << cite(flat) << ").";
  } else {
    os << high.size() << " sample(s) exceeded " << pct(threshold) << " retransmitted ("
       << cite(high) << ").";
  }
  h.explanation = os.str();
  h.next_steps = {"ss -ti to see per-connection retrans and rtt now",
                  "correlate with path loss probes; a storm without loss suggests receiver or middlebox issues"};
  unobs.push_back({"which TCP connection retransmitted", "eBPF tracepoints not available in v0.1"});
  finish(std::move(h), ruled_out, out);
}

void conntrack_pressure(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs ct = of_kind(w.events, kind::net_conntrack);
  const Evs full = of_kind(w.events, kind::kernel_conntrack_full);
  if (ct.empty() && full.empty()) {
    if (network_incident(w) && !w.avail.can_see("procfs.conntrack")) {
      unobs.push_back({"conntrack insert failures and drops", w.avail.reason("procfs.conntrack")});
    }
    return;
  }
  Evs rising, flat;
  for (Ev e : ct) {
    const auto f = e->event.attr_int("insert_failed").value_or(0) + e->event.attr_int("drop").value_or(0);
    (f > 0 ? rising : flat).push_back(e);
  }
  Hypothesis h{"conntrack_pressure", "Connection tracking table pressure or insert races", {}, 0, {}, {}, {}};
  if (!rising.empty()) { h.score += 3; add(h, rising, EvidenceRole::for_, "conntrack insert_failed or drop counters rose"); }
  if (!full.empty()) { h.score += 3; add(h, full, EvidenceRole::for_, "kernel logged a full conntrack table"); }
  const bool ruled_out = rising.empty() && full.empty() && flat.size() >= 2;
  if (ruled_out) add(h, flat, EvidenceRole::against, "conntrack counters stayed flat");
  if (!ruled_out && h.score == 0) return;
  h.explanation = ruled_out ? "Conntrack insert_failed and drop counters were flat across the window (" + cite(flat) + ")."
                            : "Conntrack counters rose (" + cite(rising) + ")" +
                                  (full.empty() ? std::string() : ", table full logged (" + cite(full) + ")") + '.';
  h.next_steps = {"conntrack -S for per-CPU insert_failed and drop", "check nf_conntrack_max against nf_conntrack_count",
                  "for DNS: the 2018 conntrack race affects parallel A/AAAA queries; try options single-request-reopen"};
  finish(std::move(h), ruled_out, out);
}

void nic_stall(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs wd = of_kind(w.events, kind::kernel_netdev_watchdog);
  if (wd.empty()) {
    if (network_incident(w) && !w.avail.can_see("kmsg")) {
      unobs.push_back({"NIC driver stalls (NETDEV WATCHDOG)", w.avail.reason("kmsg")});
    }
    if (network_incident(w) && w.avail.can_see("kmsg")) {
      Hypothesis h{"nic_stall", "NIC or driver transmit stall", Verdict::ruled_out, 0,
                   "The kernel log was readable and contained no transmit-timeout or NETDEV WATCHDOG message.", {}, {}};
      add_missing(h, "a ruled-out NIC stall rests on the absence of kernel messages, not on a positive event");
      out.push_back(std::move(h));
    }
    return;
  }
  Hypothesis h{"nic_stall", "NIC or driver transmit stall", {}, 5, {}, {}, {}};
  add(h, wd, EvidenceRole::for_, "kernel reported a transmit queue timeout");
  h.explanation = "The kernel reported a transmit timeout on " + str(wd.front(), "ifname") + " (" + cite(wd) + ").";
  h.next_steps = {"ethtool -S <ifname> and ethtool -i <ifname> for driver/firmware versions",
                  "check for a recent kernel or driver update; compare with the previous boot's kernel"};
  finish(std::move(h), false, out);
}

// --------------------------------------------------------- processes ----

void oom_kill(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs oom = of_kind(w.events, kind::kernel_oom);
  const Evs cg = of_kind(w.events, kind::cgroup_oom_kill);
  const Evs exits = process_exits(w);
  if (oom.empty() && cg.empty() && exits.empty()) return;

  Hypothesis h{"oom_kill", "Killed by the out-of-memory killer", {}, 0, {}, {}, {}};
  if (!oom.empty()) { h.score += 4; add(h, oom, EvidenceRole::for_, "kernel OOM killer selected a victim"); }
  if (!cg.empty()) { h.score += 4; add(h, cg, EvidenceRole::for_, "cgroup memory.events oom_kill counter rose"); }
  Evs psi;
  for (const auto& e : w.events) {
    if (e.event.kind == kind::sys_psi && e.event.attr_num("mem_full10").value_or(0) >= 10) psi.push_back(&e);
  }
  if (!psi.empty() && (!oom.empty() || !cg.empty())) {
    h.score += 1;
    add(h, psi, EvidenceRole::for_, "memory pressure stalls (PSI full) before the kill");
  }
  const bool kmsg_visible = w.avail.can_see("kmsg");
  const bool ruled_out = oom.empty() && cg.empty() && kmsg_visible;
  if (ruled_out) {
    add(h, exits, EvidenceRole::against, "the process exited but the kernel log holds no OOM-killer record");
  }
  if (!kmsg_visible) {
    add_missing(h, "kernel log not readable, so an OOM kill can be neither confirmed nor ruled out: " + w.avail.reason("kmsg"));
    unobs.push_back({"OOM killer activity", w.avail.reason("kmsg")});
    if (h.score == 0) h.score = 1;  // cannot rule out: keep it on the table as possible
  }
  std::ostringstream os;
  if (ruled_out) {
    os << "Not an OOM kill: the kernel log was readable and holds no OOM-killer record for the exits ("
       << cite(exits) << ").";
  } else if (!oom.empty() || !cg.empty()) {
    os << "The OOM killer acted";
    if (!oom.empty()) os << " (kernel: " << str(oom.front(), "comm") << " pid " << oom.front()->event.attr_int("pid").value_or(0) << ", " << cite(oom) << ")";
    if (!cg.empty()) os << " (cgroup " << cite(cg) << ")";
    os << '.';
  } else {
    os << "Cannot tell whether the exits (" << cite(exits) << ") were OOM kills: kernel log unavailable.";
  }
  h.explanation = os.str();
  h.next_steps = {"check MemoryMax/MemoryHigh for the unit (systemctl show -p MemoryMax <unit>)",
                  "compare the process RSS trend before the kill"};
  finish(std::move(h), ruled_out, out);
}

void restart_loop(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>&) {
  std::map<std::string, Evs> by_name;
  for (Ev e : of_kind(w.events, kind::process_restart)) by_name[str(e, "name")].push_back(e);
  for (const auto& [name, evs] : by_name) {
    if (evs.size() < 3) continue;
    Hypothesis h{"restart_loop", "Restart loop: " + name, {}, 0, {}, {}, {}};
    h.score = 4 + std::min<int>(static_cast<int>(evs.size()) - 3, 3);
    add(h, evs, EvidenceRole::for_, "process restarted");
    add_missing(h, "exit status and signal are not observable without eBPF sched_process_exit (v0.2) or journald");
    h.explanation = name + " restarted " + std::to_string(evs.size()) + " times in the window (" + cite(evs) + ").";
    h.next_steps = {"journalctl -u <unit> -n 50 for the exit reason",
                    "systemctl show -p NRestarts,ExecMainStatus <unit>"};
    finish(std::move(h), false, out);
  }
}

void unexplained_exit(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs exits = of_kind(w.events, kind::process_exit);
  if (exits.empty()) return;
  if (!of_kind(w.events, kind::kernel_oom).empty() || !of_kind(w.events, kind::cgroup_oom_kill).empty()) return;
  Hypothesis h{"process_exit", "Process exited: crash, external kill or clean stop", {}, 2, {}, {}, {}};
  add(h, exits, EvidenceRole::for_, "watched process disappeared");
  add_missing(h, "exit code and signal unavailable (eBPF sched_process_exit arrives in v0.2)");
  unobs.push_back({"exit code / signal of exited processes", "needs eBPF sched_process_exit (v0.2) or journald"});
  h.explanation = "Watched process " + str(exits.front(), "name") + " exited (" + cite(exits) +
                  "). Without the exit signal, a crash, an external SIGKILL and a clean stop look the same.";
  h.next_steps = {"journalctl -u <unit> or the service's own log for the last lines before exit",
                  "check for core dumps: coredumpctl list"};
  finish(std::move(h), false, out);
}

// ------------------------------------------------------------- clock ----

void clock_step(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>&) {
  const Evs steps = of_kind(w.events, kind::clock_step);
  Evs unsynced;
  for (Ev e : of_kind(w.events, kind::clock_state)) {
    if (!e->event.attr_bool("synced").value_or(true)) unsynced.push_back(e);
  }
  if (steps.empty() && unsynced.empty()) return;
  Hypothesis h{"clock_step", "Wall clock stepped or unsynchronised", {}, 0, {}, {}, {}};
  if (!steps.empty()) { h.score += 4; add(h, steps, EvidenceRole::for_, "CLOCK_REALTIME jumped relative to CLOCK_BOOTTIME"); }
  if (!unsynced.empty()) { h.score += 1; add(h, unsynced, EvidenceRole::for_, "kernel reported the clock unsynchronised"); }
  std::ostringstream os;
  if (!steps.empty()) os << "The wall clock stepped by " << steps.front()->event.attr_int("step_ms").value_or(0) / 1000 << " s (" << cite(steps) << "). ";
  os << "TLS certificate validity and token expiry checks fail when wall time is wrong; ordering here uses CLOCK_BOOTTIME and is unaffected.";
  h.explanation = os.str();
  h.next_steps = {"timedatectl timesync-status / chronyc tracking", "an RTC or fake-hwclock avoids starting in the past after power loss"};
  finish(std::move(h), false, out);
}

// -------------------------------------------------------- resources ----

void resource_pressure(const Window& w, std::vector<Hypothesis>& out, std::vector<Unobservable>& unobs) {
  const Evs psi = of_kind(w.events, kind::sys_psi);
  if (psi.empty()) {
    if (!w.avail.can_see("procfs.psi")) unobs.push_back({"CPU/memory/IO pressure stalls (PSI)", w.avail.reason("procfs.psi")});
    return;
  }
  Evs cpu, mem, io, calm;
  for (Ev e : psi) {
    bool hot = false;
    if (e->event.attr_num("cpu_some10").value_or(0) >= 50) { cpu.push_back(e); hot = true; }
    if (e->event.attr_num("mem_some10").value_or(0) >= 20) { mem.push_back(e); hot = true; }
    if (e->event.attr_num("io_some10").value_or(0) >= 50) { io.push_back(e); hot = true; }
    if (!hot) calm.push_back(e);
  }
  Hypothesis h{"resource_pressure", "CPU, memory or I/O starvation", {}, 0, {}, {}, {}};
  if (cpu.size() >= 2) { h.score += 4; add(h, cpu, EvidenceRole::for_, "tasks stalled waiting for CPU (PSI some avg10 >= 50 %)"); }
  else if (!cpu.empty()) { h.score += 2; add(h, cpu, EvidenceRole::for_, "CPU pressure spike"); }
  if (!mem.empty()) { h.score += 3; add(h, mem, EvidenceRole::for_, "tasks stalled on memory reclaim (PSI some avg10 >= 20 %)"); }
  if (!io.empty()) { h.score += 2; add(h, io, EvidenceRole::for_, "tasks stalled on I/O (PSI some avg10 >= 50 %)"); }
  const bool ruled_out = h.score == 0 && calm.size() >= 2;
  if (ruled_out) add(h, calm, EvidenceRole::against, "pressure stall information stayed low");
  std::ostringstream os;
  if (ruled_out) os << "CPU, memory and I/O pressure stayed low across " << calm.size() << " samples (" << cite(calm) << ").";
  else {
    if (!cpu.empty()) os << "CPU pressure high in " << cpu.size() << " sample(s) (" << cite(cpu) << "). ";
    if (!mem.empty()) os << "Memory pressure in " << mem.size() << " sample(s) (" << cite(mem) << "). ";
    if (!io.empty()) os << "I/O pressure in " << io.size() << " sample(s) (" << cite(io) << "). ";
  }
  h.explanation = os.str();
  h.next_steps = {"top -o %CPU / ps aux --sort=-rss at the time of the next occurrence",
                  "cat /proc/pressure/{cpu,memory,io} and per-cgroup cpu.pressure"};
  finish(std::move(h), ruled_out, out);
}

}  // namespace

const std::vector<std::pair<std::string, Playbook>>& playbooks() {
  static const std::vector<std::pair<std::string, Playbook>> all = {
      {"local_resolver", &local_resolver},
      {"upstream_dns", &upstream_dns},
      {"link_or_gateway", &link_or_gateway},
      {"path_degradation", &path_degradation},
      {"retrans_storm", &retrans_storm},
      {"conntrack_pressure", &conntrack_pressure},
      {"nic_stall", &nic_stall},
      {"oom_kill", &oom_kill},
      {"restart_loop", &restart_loop},
      {"process_exit", &unexplained_exit},
      {"clock_step", &clock_step},
      {"resource_pressure", &resource_pressure},
  };
  return all;
}

}  // namespace hm
