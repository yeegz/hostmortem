#include "collectors/procfs_sampler.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.hpp"

namespace hm {

std::string errno_name(int err) {
  switch (err) {
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case ETIMEDOUT: return "ETIMEDOUT";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EHOSTUNREACH: return "EHOSTUNREACH";
    case ENETUNREACH: return "ENETUNREACH";
    case ECONNRESET: return "ECONNRESET";
    case EAGAIN: return "EAGAIN";
    case ENOBUFS: return "ENOBUFS";
    default: return "errno_" + std::to_string(err);
  }
}

namespace {

std::optional<std::string> slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::int64_t delta(const procfs::Table& now, const procfs::Table& prev, const char* proto, const char* key) {
  auto a = now.find(proto);
  auto b = prev.find(proto);
  if (a == now.end() || b == prev.end()) return 0;
  auto x = a->second.find(key);
  auto y = b->second.find(key);
  if (x == a->second.end() || y == b->second.end()) return 0;
  const std::int64_t d = x->second - y->second;
  return d < 0 ? 0 : d;  // counter reset (e.g. netns recreated): treat as 0, not negative
}

}  // namespace

void ProcfsSampler::sample_once() {
  const ClockReading clk = read_clocks();

  // Clock: a step is CLOCK_REALTIME moving differently from CLOCK_BOOTTIME.
  if (last_clock_) {
    const std::int64_t d_real = clk.realtime_ns - last_clock_->realtime_ns;
    const std::int64_t d_boot = clk.boottime_ns - last_clock_->boottime_ns;
    const std::int64_t step = d_real - d_boot;
    if (step > 1'000'000'000LL || step < -1'000'000'000LL) {
      em_.emit(kind::clock_step, Severity::warning, "realtime", {{"step_ms", step / 1'000'000}});
    }
  }
  if (!last_clock_ || clk.quality != last_quality_) {
    em_.emit(kind::clock_state, clk.quality == WallQuality::unsynced ? Severity::notice : Severity::info, "realtime",
             {{"synced", clk.quality == WallQuality::synced},
              {"quality", std::string(to_string(clk.quality))},
              {"max_error_us", clk.max_error_us}});
    last_quality_ = clk.quality;
  }
  last_clock_ = clk;

  EventQueue::Push worst = EventQueue::Push::accepted;
  auto note = [&](EventQueue::Push p) {
    if (p == EventQueue::Push::pressure || p == EventQueue::Push::dropped) worst = p;
  };

  if (auto t = slurp(root_ + "/stat")) {
    if (auto cpu = procfs::parse_stat_cpu(*t)) {
      if (last_cpu_ && cpu->total() > last_cpu_->total()) {
        const double dt = static_cast<double>(cpu->total() - last_cpu_->total());
        const double di = static_cast<double>(cpu->idle_all() - last_cpu_->idle_all());
        note(em_.emit(kind::sys_cpu, Severity::debug, "cpu", {{"busy_pct", 100.0 * (1.0 - di / dt)}}));
      }
      last_cpu_ = cpu;
    }
  }
  if (auto t = slurp(root_ + "/meminfo")) {
    if (auto m = procfs::parse_meminfo(*t)) {
      note(em_.emit(kind::sys_mem, Severity::debug, "memory",
                    {{"mem_total_kb", m->total_kb}, {"mem_available_kb", m->available_kb},
                     {"swap_total_kb", m->swap_total_kb}, {"swap_free_kb", m->swap_free_kb}}));
    }
  }

  auto cpu_psi = slurp(root_ + "/pressure/cpu");
  auto mem_psi = slurp(root_ + "/pressure/memory");
  auto io_psi = slurp(root_ + "/pressure/io");
  const bool psi_now = cpu_psi && mem_psi && io_psi;
  if (psi_now) {
    auto c = procfs::parse_psi(*cpu_psi);
    auto m = procfs::parse_psi(*mem_psi);
    auto i = procfs::parse_psi(*io_psi);
    if (c && m && i) {
      note(em_.emit(kind::sys_psi, Severity::debug, "psi",
                    {{"cpu_some10", c->some_avg10}, {"mem_some10", m->some_avg10},
                     {"mem_full10", m->full_avg10.value_or(0.0)}, {"io_some10", i->some_avg10}}));
    }
  }

  if (auto t = slurp(root_ + "/net/snmp")) {
    auto snmp = procfs::parse_snmp(*t);
    auto netstat_text = slurp(root_ + "/net/netstat");
    procfs::Table netstat = netstat_text ? procfs::parse_snmp(*netstat_text) : procfs::Table{};
    if (last_snmp_) {
      const auto out = delta(snmp, *last_snmp_, "Tcp", "OutSegs");
      const auto retr = delta(snmp, *last_snmp_, "Tcp", "RetransSegs");
      const double ppm = out > 0 ? 1e6 * static_cast<double>(retr) / static_cast<double>(out) : 0.0;
      const auto timeouts = last_netstat_ ? delta(netstat, *last_netstat_, "TcpExt", "TCPTimeouts") : 0;
      note(em_.emit(kind::net_tcp, Severity::debug, "tcp",
                    {{"out_segs", out}, {"retrans_segs", retr}, {"retrans_ppm", ppm},
                     {"attempt_fails", delta(snmp, *last_snmp_, "Tcp", "AttemptFails")},
                     {"estab_resets", delta(snmp, *last_snmp_, "Tcp", "EstabResets")},
                     {"timeouts", timeouts}}));
      note(em_.emit(kind::net_udp, Severity::debug, "udp",
                    {{"in_errors", delta(snmp, *last_snmp_, "Udp", "InErrors")},
                     {"rcvbuf_errors", delta(snmp, *last_snmp_, "Udp", "RcvbufErrors")}}));
    }
    last_snmp_ = std::move(snmp);
    last_netstat_ = std::move(netstat);
  }

  auto ct_text = slurp(root_ + "/net/stat/nf_conntrack");
  std::optional<std::map<std::string, std::uint64_t>> ct;
  if (ct_text) ct = procfs::parse_conntrack_stat(*ct_text);
  if (ct && last_ct_) {
    auto d = [&](const char* k) -> std::int64_t {
      auto a = ct->find(k);
      auto b = last_ct_->find(k);
      if (a == ct->end() || b == last_ct_->end() || a->second < b->second) return 0;
      return static_cast<std::int64_t>(a->second - b->second);
    };
    const auto failed = d("insert_failed");
    const auto drop = d("drop");
    note(em_.emit(kind::net_conntrack, failed + drop > 0 ? Severity::warning : Severity::debug, "conntrack",
                  {{"insert_failed", failed}, {"drop", drop}, {"early_drop", d("early_drop")}}));
  }
  last_ct_ = ct;

  if (!reported_ || psi_now != psi_ok_ || ct.has_value() != ct_ok_) {
    em_.status("procfs", "active");
    em_.status("procfs.psi", psi_now ? "active" : "unavailable",
               psi_now ? "" : "no /proc/pressure: kernel without CONFIG_PSI or booted with psi=0");
    em_.status("procfs.conntrack", ct ? "active" : "unavailable",
               ct ? "" : "no /proc/net/stat/nf_conntrack: nf_conntrack not loaded in this network namespace");
    reported_ = true;
    psi_ok_ = psi_now;
    ct_ok_ = ct.has_value();
  }

  if (worst != EventQueue::Push::accepted && !backoff_) {
    log::warn("procfs", "queue pressure: halving sample rate");
    backoff_ = true;
  } else if (worst == EventQueue::Push::accepted && backoff_) {
    backoff_ = false;
  }
}

void ProcfsSampler::run(std::stop_token stop) {
  do {
    sample_once();
  } while (sleep_for(stop, backoff_ ? interval_ * 2 : interval_));
}

}  // namespace hm
