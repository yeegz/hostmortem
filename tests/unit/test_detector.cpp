#include <gtest/gtest.h>

#include "analysis/detector.hpp"
#include "core/kinds.hpp"
#include "test_util.hpp"

using namespace hm;
using Type = Detector::Action::Type;

namespace {
std::int64_t g_id = 0;
StoredEvent se(std::int64_t ordinal, const std::string& boot, std::int64_t mono_s, const std::string& k, Attrs a = {},
               const std::string& subject = "") {
  auto e = test::make_event(boot, "t", static_cast<std::uint64_t>(++g_id), mono_s, k, std::move(a), Severity::info, subject);
  e.id = g_id;
  return {e, ordinal};
}
}  // namespace

TEST(Detector, ProbeStreakOpensAtFirstFailure) {
  Detector d(DetectorConfig{});
  EXPECT_TRUE(d.observe(se(1, "b", 10, kind::probe_dns_resolver, {{"ok", false}}, "resolver")).empty());
  const auto first_id = g_id;
  EXPECT_TRUE(d.observe(se(1, "b", 20, kind::probe_dns_resolver, {{"ok", false}}, "resolver")).empty());
  auto acts = d.observe(se(1, "b", 30, kind::probe_dns_resolver, {{"ok", false}}, "resolver"));
  ASSERT_EQ(acts.size(), 1u);
  EXPECT_EQ(acts[0].type, Type::open);
  EXPECT_EQ(acts[0].incident.trigger, "probe_fail_streak");
  EXPECT_EQ(acts[0].incident.opened.mono_ns, 10'000'000'000LL);
  EXPECT_EQ(acts[0].incident.trigger_event_id, first_id);
}

TEST(Detector, SuccessResetsStreak) {
  Detector d(DetectorConfig{});
  (void)d.observe(se(1, "b", 10, kind::probe_icmp, {{"ok", false}}, "gw"));
  (void)d.observe(se(1, "b", 20, kind::probe_icmp, {{"ok", false}}, "gw"));
  (void)d.observe(se(1, "b", 30, kind::probe_icmp, {{"ok", true}}, "gw"));
  EXPECT_TRUE(d.observe(se(1, "b", 40, kind::probe_icmp, {{"ok", false}}, "gw")).empty());
}

TEST(Detector, ClosesAfterQuietPeriodAndExtendsOnSymptoms) {
  DetectorConfig cfg;
  cfg.quiet = std::chrono::seconds(60);
  Detector d(cfg);
  auto open = d.observe(se(1, "b", 100, kind::link_state, {{"ifname", std::string("eth0")}, {"oper", std::string("down")}}));
  ASSERT_EQ(open.size(), 1u);
  d.set_open_id(7);
  EXPECT_TRUE(d.observe(se(1, "b", 140, kind::process_exit, {{"name", std::string("x")}})).empty()) << "extends";
  EXPECT_TRUE(d.observe(se(1, "b", 190, kind::sys_cpu)).empty()) << "50 s after last symptom: still open";
  auto close = d.observe(se(1, "b", 201, kind::sys_cpu));
  ASSERT_EQ(close.size(), 1u);
  EXPECT_EQ(close[0].type, Type::close);
  EXPECT_EQ(close[0].incident.id, 7);
}

TEST(Detector, IncidentSurvivesReboot) {
  DetectorConfig cfg;
  cfg.quiet = std::chrono::seconds(60);
  Detector d(cfg);
  (void)d.observe(se(1, "b1", 1000, kind::kernel_oom, {{"pid", std::int64_t{5}}}));
  d.set_open_id(3);
  // New boot: mono restarts near zero. Must not close (nor compare mono across boots).
  EXPECT_TRUE(d.observe(se(2, "b2", 5, kind::agent_start)).empty());
  EXPECT_TRUE(d.observe(se(2, "b2", 50, kind::sys_cpu)).empty());
  auto close = d.observe(se(2, "b2", 70, kind::sys_cpu));
  ASSERT_EQ(close.size(), 1u);
  EXPECT_EQ(close[0].incident.closed.boot_ordinal, 2);
}

TEST(Detector, RestartBurstIsALoop) {
  Detector d(DetectorConfig{});
  auto r = [&](int t) { return d.symptom(se(1, "b", t, kind::process_restart, {{"name", std::string("svc")}})); };
  EXPECT_EQ(r(10), "process_restart");
  EXPECT_EQ(r(20), "process_restart");
  EXPECT_EQ(r(30), "restart_loop");
  EXPECT_EQ(r(1000), "process_restart") << "outside the 5-minute window the burst resets";
}

TEST(Detector, RetransExcursionAgainstLearnedBaseline) {
  Detector d(DetectorConfig{});
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(d.symptom(se(1, "b", i, kind::net_tcp, {{"out_segs", std::int64_t{1000}}, {"retrans_ppm", 1000.0}})));
  }
  EXPECT_FALSE(d.symptom(se(1, "b", 11, kind::net_tcp, {{"out_segs", std::int64_t{1000}}, {"retrans_ppm", 15000.0}})))
      << "below the 2 % floor";
  EXPECT_EQ(d.symptom(se(1, "b", 12, kind::net_tcp, {{"out_segs", std::int64_t{1000}}, {"retrans_ppm", 50000.0}})),
            "retrans_excursion");
  EXPECT_FALSE(d.symptom(se(1, "b", 13, kind::net_tcp, {{"out_segs", std::int64_t{3}}, {"retrans_ppm", 900000.0}})))
      << "too few segments to judge";
}

TEST(Detector, LoopbackLinkIgnored) {
  Detector d(DetectorConfig{});
  EXPECT_FALSE(d.symptom(se(1, "b", 1, kind::link_state, {{"ifname", std::string("lo")}, {"oper", std::string("down")}})));
}
