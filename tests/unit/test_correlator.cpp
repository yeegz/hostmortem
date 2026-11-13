#include <gtest/gtest.h>

#include "analysis/correlator.hpp"
#include "core/kinds.hpp"
#include "test_util.hpp"

using namespace hm;

namespace {

class Scenario {
 public:
  Scenario& at(std::int64_t t, const std::string& k, Attrs a = {}, const std::string& subject = "",
               std::int64_t ordinal = 1) {
    auto e = test::make_event(ordinal == 1 ? "b1" : "b2", "t", static_cast<std::uint64_t>(++id_), t, k, std::move(a),
                              Severity::info, subject);
    e.id = id_;
    events_.push_back({e, ordinal});
    return *this;
  }
  Scenario& status(const std::string& collector, const std::string& state, const std::string& reason = "") {
    avail_.collectors[collector] = {state == "active" ? CollectorState::active : CollectorState::unavailable, reason};
    return *this;
  }
  Scenario& base(std::int64_t t, const std::string& k, Attrs a) {
    auto e = test::make_event("b1", "t", static_cast<std::uint64_t>(++id_), t, k, std::move(a));
    e.id = id_;
    baseline_.push_back({e, 1});
    return *this;
  }
  Diagnosis run() {
    inc_.id = 1;
    inc_.opened = {1, "b1", events_.empty() ? 0 : events_.front().event.mono_ns};
    inc_.trigger = "test";
    return diagnose(inc_, events_, baseline_, avail_, {1, "b1", 0}, {events_.back().boot_ordinal, "", events_.back().event.mono_ns});
  }

 private:
  std::int64_t id_ = 0;
  Incident inc_;
  std::vector<StoredEvent> events_;
  std::vector<StoredEvent> baseline_;
  Availability avail_;
};

const Hypothesis* find(const Diagnosis& d, const std::string& pb) {
  for (const auto& h : d.hypotheses) {
    if (h.playbook == pb) return &h;
  }
  return nullptr;
}

Attrs gw(bool ok) {
  return {{"ok", ok}, {"role", std::string("gateway")}, {"target", std::string("10.0.0.1")}, {"rtt_ms", 1.0}};
}

Scenario all_active() {
  Scenario s;
  s.status("kmsg", "active").status("netlink", "active").status("prober", "active").status("prober.icmp", "active")
      .status("procfs.psi", "active").status("procfs.conntrack", "active");
  return s;
}

}  // namespace

// Lab scenario "DNS, local path": stop the stub resolver; direct queries work.
// Pass criterion: does not blame upstream.
TEST(Correlator, LocalResolverFaultDoesNotBlameUpstream) {
  auto s = all_active();
  for (int t = 0; t < 40; t += 10) {
    s.at(t, kind::probe_dns_resolver, {{"ok", false}, {"err", std::string("Temporary failure in name resolution")}}, "resolver")
        .at(t + 1, kind::probe_dns_direct, {{"ok", true}}, "10.0.0.53")
        .at(t + 2, kind::probe_icmp, gw(true), "10.0.0.1");
  }
  auto d = s.run();
  ASSERT_NE(find(d, "local_resolver"), nullptr);
  EXPECT_EQ(find(d, "local_resolver")->verdict, Verdict::supported);
  EXPECT_EQ(find(d, "upstream_dns"), nullptr);
  EXPECT_EQ(find(d, "link_or_gateway")->verdict, Verdict::ruled_out);
  EXPECT_EQ(d.conclusion, "supported");
  EXPECT_EQ(d.hypotheses[static_cast<std::size_t>(d.leading_index)].playbook, "local_resolver");
  EXPECT_FALSE(validate_citations(d).has_value()) << *validate_citations(d);
}

// "DNS, upstream": drop UDP 53 on the path. Pass: does not blame local.
TEST(Correlator, UpstreamDnsFaultDoesNotBlameLocal) {
  auto s = all_active();
  for (int t = 0; t < 40; t += 10) {
    s.at(t, kind::probe_dns_resolver, {{"ok", false}}, "resolver")
        .at(t + 1, kind::probe_dns_direct, {{"ok", false}, {"err", std::string("timeout")}}, "10.0.0.53")
        .at(t + 2, kind::probe_icmp, gw(true), "10.0.0.1");
  }
  auto d = s.run();
  EXPECT_EQ(find(d, "upstream_dns")->verdict, Verdict::supported);
  EXPECT_EQ(find(d, "local_resolver")->verdict, Verdict::ruled_out);
  EXPECT_EQ(d.hypotheses[static_cast<std::size_t>(d.leading_index)].playbook, "upstream_dns");
}

// "Gateway disconnect": link down. Pass: does not blame DNS.
TEST(Correlator, LinkLossDoesNotBlameDns) {
  auto s = all_active();
  s.at(0, kind::link_state, {{"ifname", std::string("eth0")}, {"oper", std::string("down")}, {"carrier", false}}, "eth0");
  for (int t = 1; t < 40; t += 10) {
    s.at(t, kind::probe_dns_resolver, {{"ok", false}}, "resolver")
        .at(t + 1, kind::probe_dns_direct, {{"ok", false}}, "10.0.0.53")
        .at(t + 2, kind::probe_icmp, gw(false), "10.0.0.1");
  }
  auto d = s.run();
  EXPECT_EQ(find(d, "link_or_gateway")->verdict, Verdict::supported);
  EXPECT_EQ(find(d, "upstream_dns")->verdict, Verdict::ruled_out);
  EXPECT_EQ(find(d, "local_resolver")->verdict, Verdict::ruled_out);
  EXPECT_EQ(d.conclusion, "supported");
  EXPECT_EQ(d.hypotheses[static_cast<std::size_t>(d.leading_index)].playbook, "link_or_gateway");
}

// "Process termination" (kill -9): OOM ruled out, cause unknown.
TEST(Correlator, KillNineRulesOutOomAndSaysUnknown) {
  auto s = all_active();
  s.at(0, kind::process_exit, {{"name", std::string("app")}, {"pid", std::int64_t{42}}}, "app");
  auto d = s.run();
  EXPECT_EQ(find(d, "oom_kill")->verdict, Verdict::ruled_out);
  EXPECT_EQ(find(d, "process_exit")->verdict, Verdict::possible);
  EXPECT_EQ(d.conclusion, "unknown");
}

// "Missing data": without the kernel log OOM cannot be ruled out, and the
// diagnosis must say what it could not see. Pass: no confident verdict.
TEST(Correlator, MissingKernelLogMeansNoConfidentVerdict) {
  Scenario s;
  s.status("kmsg", "unavailable", "open /dev/kmsg: EPERM (needs CAP_SYSLOG)");
  s.at(0, kind::process_exit, {{"name", std::string("app")}, {"pid", std::int64_t{42}}}, "app");
  auto d = s.run();
  ASSERT_NE(find(d, "oom_kill"), nullptr);
  EXPECT_EQ(find(d, "oom_kill")->verdict, Verdict::possible);
  EXPECT_EQ(d.conclusion, "unknown");
  bool listed = false;
  for (const auto& u : d.unobservable) listed |= u.what == "OOM killer activity";
  EXPECT_TRUE(listed);
}

TEST(Correlator, OomKillSupported) {
  auto s = all_active();
  s.at(0, kind::sys_psi, {{"mem_full10", 35.0}, {"mem_some10", 60.0}})
      .at(1, kind::kernel_oom, {{"pid", std::int64_t{42}}, {"comm", std::string("app")}}, "app")
      .at(2, kind::process_exit, {{"name", std::string("app")}, {"pid", std::int64_t{42}}}, "app");
  auto d = s.run();
  EXPECT_EQ(find(d, "oom_kill")->verdict, Verdict::supported);
  EXPECT_EQ(find(d, "process_exit"), nullptr) << "an explained exit is not 'unexplained'";
}

TEST(Correlator, RestartLoopSupported) {
  auto s = all_active();
  for (int t = 0; t < 40; t += 10) s.at(t, kind::process_restart, {{"name", std::string("svc")}}, "svc");
  auto d = s.run();
  EXPECT_EQ(find(d, "restart_loop")->verdict, Verdict::supported);
}

TEST(Correlator, ClockStepSupported) {
  auto s = all_active();
  s.at(0, kind::clock_step, {{"step_ms", std::int64_t{86'400'000}}})
      .at(5, kind::probe_tcp, {{"ok", false}, {"err", std::string("ECONNREFUSED")}}, "10.0.0.9:443");
  auto d = s.run();
  EXPECT_EQ(find(d, "clock_step")->verdict, Verdict::supported);
}

TEST(Correlator, RetransStormAgainstBaseline) {
  auto s = all_active();
  for (int t = -300; t < 0; t += 5) s.base(t, kind::net_tcp, {{"out_segs", std::int64_t{2000}}, {"retrans_ppm", 500.0}});
  for (int t = 0; t < 30; t += 5) {
    s.at(t, kind::net_tcp, {{"out_segs", std::int64_t{2000}}, {"retrans_ppm", 60000.0}, {"timeouts", std::int64_t{3}}});
  }
  auto d = s.run();
  const auto* h = find(d, "retrans_storm");
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->verdict, Verdict::supported);
  bool has_missing = false;
  for (const auto& e : h->evidence) has_missing |= e.role == EvidenceRole::missing;
  EXPECT_TRUE(has_missing) << "must admit per-flow attribution is unavailable";
}

TEST(Correlator, PathLatencyAgainstBaseline) {
  auto s = all_active();
  for (int t = -300; t < 0; t += 10) {
    s.base(t, kind::probe_icmp, {{"ok", true}, {"role", std::string("target")}, {"target", std::string("1.1.1.1")}, {"rtt_ms", 10.0}});
  }
  for (int t = 0; t < 60; t += 10) {
    s.at(t, kind::probe_icmp, {{"ok", true}, {"role", std::string("target")}, {"target", std::string("1.1.1.1")}, {"rtt_ms", 320.0}}, "1.1.1.1");
  }
  s.at(61, kind::probe_tcp, {{"ok", false}, {"err", std::string("timeout")}}, "1.1.1.1:443");
  auto d = s.run();
  ASSERT_NE(find(d, "path_degradation"), nullptr);
  EXPECT_EQ(find(d, "path_degradation")->verdict, Verdict::supported);
}

TEST(Correlator, EquallySupportedExplanationsMeanUnknown) {
  auto s = all_active();
  s.at(0, kind::clock_step, {{"step_ms", std::int64_t{5000}}})
      .at(1, kind::kernel_netdev_watchdog, {{"ifname", std::string("eth0")}}, "eth0")
      .at(2, kind::probe_tcp, {{"ok", false}}, "x");
  auto d = s.run();
  EXPECT_EQ(find(d, "clock_step")->score, 4);
  EXPECT_EQ(find(d, "nic_stall")->score, 5);
  EXPECT_EQ(d.conclusion, "unknown") << d.conclusion_reason;
}

TEST(Correlator, SpansBoots) {
  auto s = all_active();
  s.at(100, kind::probe_dns_resolver, {{"ok", false}}, "resolver")
      .at(5, kind::agent_start, {}, "hmd", 2)
      .at(10, kind::probe_dns_resolver, {{"ok", false}}, "resolver", 2);
  auto d = s.run();
  EXPECT_TRUE(d.spans_boots);
}

TEST(Correlator, AvailabilityLatestWinsAndFallsBackToParent) {
  std::vector<StoredEvent> st;
  auto e1 = test::make_event("b", "kmsg", 1, 1, kind::collector_status, {{"state", std::string("active")}}, Severity::info, "kmsg");
  auto e2 = test::make_event("b", "kmsg", 2, 2, kind::collector_status,
                             {{"state", std::string("degraded")}, {"reason", std::string("read: EIO")}}, Severity::info, "kmsg");
  auto e3 = test::make_event("b", "p", 1, 1, kind::collector_status, {{"state", std::string("active")}}, Severity::info, "prober");
  st.push_back({e1, 1});
  st.push_back({e2, 1});
  st.push_back({e3, 1});
  auto a = availability_from(st);
  EXPECT_EQ(a.state("kmsg"), CollectorState::degraded);
  EXPECT_EQ(a.state("prober.icmp"), CollectorState::active);
  EXPECT_EQ(a.state("netlink"), CollectorState::unknown);
  EXPECT_NE(a.reason("kmsg").find("EIO"), std::string::npos);
}

// Real lab finding: 50 % netem loss scored only "possible". Interleaved loss
// (replies between failures) is now supported...
TEST(Correlator, InterleavedLossIsSupportedPathDegradation) {
  auto s = all_active();
  const bool pattern[] = {true, false, true, false, false, true, false, true};
  int t = 0;
  for (bool ok : pattern) s.at(t += 5, kind::probe_icmp, gw(ok), "10.0.0.1");
  auto d = s.run();
  ASSERT_NE(find(d, "path_degradation"), nullptr);
  EXPECT_EQ(find(d, "path_degradation")->verdict, Verdict::supported);
}

// ...but a solid outage surrounded by healthy probes is not "loss".
TEST(Correlator, OutageBlockIsNotPathLoss) {
  auto s = all_active();
  int t = 0;
  for (int i = 0; i < 4; ++i) s.at(t += 5, kind::probe_icmp, gw(true), "10.0.0.1");
  for (int i = 0; i < 5; ++i) s.at(t += 5, kind::probe_icmp, gw(false), "10.0.0.1");
  for (int i = 0; i < 4; ++i) s.at(t += 5, kind::probe_icmp, gw(true), "10.0.0.1");
  auto d = s.run();
  EXPECT_EQ(find(d, "path_degradation"), nullptr);
  EXPECT_EQ(find(d, "link_or_gateway")->verdict, Verdict::possible) << "gateway unreachable without link events";
}
