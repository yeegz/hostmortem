#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>

#include "../unit/test_util.hpp"
#include "collectors/netlink_collector.hpp"
#include "collectors/procfs_sampler.hpp"
#include "collectors/process_watch.hpp"

using namespace hm;
namespace fs = std::filesystem;

namespace {
void write(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream(path) << text;
}

std::vector<Event> drain(EventQueue& q) {
  std::vector<Event> out;
  q.close();
  while (q.pop_batch(out, 1000, std::chrono::steady_clock::now())) {
  }
  return out;
}

std::vector<Event> of(const std::vector<Event>& evs, const char* k) {
  std::vector<Event> out;
  for (const auto& e : evs) {
    if (e.kind == k) out.push_back(e);
  }
  return out;
}
}  // namespace

TEST(ProcfsSampler, EmitsRatesAndReportsMissingFeatures) {
  test::TempDir d;
  const std::string root = d.file("proc");
  write(root + "/stat", "cpu  100 0 100 800 0 0 0 0\n");
  write(root + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 500 kB\n");
  write(root + "/net/snmp", "Tcp: OutSegs RetransSegs AttemptFails EstabResets\nTcp: 1000 10 0 0\nUdp: InErrors RcvbufErrors\nUdp: 0 0\n");
  write(root + "/net/netstat", "TcpExt: TCPTimeouts\nTcpExt: 5\n");
  EventQueue q(1000, 1000);
  Emitter em(q, "procfs", "boot", 0);
  ProcfsSampler s(em, std::chrono::milliseconds(1000), root);
  s.sample_once();
  write(root + "/stat", "cpu  150 0 150 900 0 0 0 0\n");
  write(root + "/net/snmp", "Tcp: OutSegs RetransSegs AttemptFails EstabResets\nTcp: 3000 110 2 1\nUdp: InErrors RcvbufErrors\nUdp: 4 0\n");
  write(root + "/net/netstat", "TcpExt: TCPTimeouts\nTcpExt: 8\n");
  s.sample_once();
  auto evs = drain(q);

  auto cpu = of(evs, kind::sys_cpu);
  ASSERT_EQ(cpu.size(), 1u);
  EXPECT_NEAR(*cpu[0].attr_num("busy_pct"), 50.0, 0.01);
  auto tcp = of(evs, kind::net_tcp);
  ASSERT_EQ(tcp.size(), 1u);
  EXPECT_EQ(tcp[0].attr_int("out_segs"), 2000);
  EXPECT_EQ(tcp[0].attr_int("retrans_segs"), 100);
  EXPECT_NEAR(*tcp[0].attr_num("retrans_ppm"), 50000.0, 0.1);
  EXPECT_EQ(tcp[0].attr_int("timeouts"), 3);

  bool psi_unavailable = false, ct_unavailable = false;
  for (const auto& e : of(evs, kind::collector_status)) {
    if (e.subject == "procfs.psi") psi_unavailable = e.attr_str("state") == "unavailable" && e.attr_str("reason").has_value();
    if (e.subject == "procfs.conntrack") ct_unavailable = e.attr_str("state") == "unavailable";
  }
  EXPECT_TRUE(psi_unavailable) << "FR6: a missing source is reported with a reason";
  EXPECT_TRUE(ct_unavailable);
}

TEST(ProcfsSampler, CounterResetIsNotNegative) {
  test::TempDir d;
  const std::string root = d.file("proc");
  write(root + "/net/snmp", "Tcp: OutSegs RetransSegs\nTcp: 5000 500\n");
  EventQueue q(1000, 1000);
  Emitter em(q, "procfs", "boot", 0);
  ProcfsSampler s(em, std::chrono::milliseconds(1000), root);
  s.sample_once();
  write(root + "/net/snmp", "Tcp: OutSegs RetransSegs\nTcp: 10 1\n");
  s.sample_once();
  auto tcp = of(drain(q), kind::net_tcp);
  ASSERT_EQ(tcp.size(), 1u);
  EXPECT_EQ(tcp[0].attr_int("out_segs"), 0);
}

TEST(ProcessWatch, DetectsExitAndRestart) {
  test::TempDir d;
  const std::string proc = d.file("proc");
  write(proc + "/100/comm", "app\n");
  write(proc + "/200/comm", "other\n");
  EventQueue q(1000, 1000);
  Emitter em(q, "process", "boot", 0);
  ProcessWatch w(em, {"app"}, proc, d.file("cg"));
  w.scan_once();
  fs::remove_all(proc + "/100");
  w.scan_once();
  write(proc + "/101/comm", "app\n");
  w.scan_once();
  auto evs = drain(q);
  ASSERT_EQ(of(evs, kind::process_exit).size(), 1u);
  EXPECT_EQ(of(evs, kind::process_exit)[0].attr_int("pid"), 100);
  auto restarts = of(evs, kind::process_restart);
  ASSERT_EQ(restarts.size(), 1u);
  EXPECT_EQ(restarts[0].attr_int("old_pid"), 100);
  EXPECT_EQ(restarts[0].attr_int("new_pid"), 101);
}

TEST(ProcessWatch, CgroupOomKillCounter) {
  test::TempDir d;
  const std::string proc = d.file("proc");
  const std::string cg = d.file("cg");
  write(proc + "/100/comm", "app\n");
  write(proc + "/100/cgroup", "0::/app.slice\n");
  write(cg + "/app.slice/memory.events", "low 0\nhigh 0\nmax 3\noom 1\noom_kill 0\n");
  EventQueue q(1000, 1000);
  Emitter em(q, "process", "boot", 0);
  ProcessWatch w(em, {"app"}, proc, cg);
  w.scan_once();
  fs::remove_all(proc + "/100");
  write(cg + "/app.slice/memory.events", "low 0\nhigh 0\nmax 9\noom 2\noom_kill 1\n");
  w.scan_once();
  auto ooms = of(drain(q), kind::cgroup_oom_kill);
  ASSERT_EQ(ooms.size(), 1u);
  EXPECT_EQ(ooms[0].attr_int("delta"), 1);
}

// Real kernel: dump links over rtnetlink in this network namespace. Every
// Linux netns has at least "lo", so this runs anywhere without privileges.
TEST(NetlinkCollector, InitialDumpSeesLoopback) {
  EventQueue q(1000, 1000);
  Emitter em(q, "netlink", "boot", 0);
  NetlinkCollector c(em);
  std::jthread t([&](std::stop_token st) { c.run(st); });
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  t.request_stop();
  t.join();
  auto evs = drain(q);
  bool lo = false, active = false;
  for (const auto& e : evs) {
    lo |= e.kind == kind::link_state && e.subject == "lo";
    active |= e.kind == kind::collector_status && e.attr_str("state") == "active";
  }
  EXPECT_TRUE(lo);
  EXPECT_TRUE(active);
}
