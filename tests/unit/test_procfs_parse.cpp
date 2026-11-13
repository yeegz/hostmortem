#include <gtest/gtest.h>

#include "collectors/procfs_parse.hpp"

using namespace hm::procfs;

TEST(Procfs, StatCpu) {
  auto c = parse_stat_cpu("cpu  100 5 50 1000 20 1 2 3 0 0\ncpu0 1 2 3 4\nintr 1\n");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->user, 100u);
  EXPECT_EQ(c->steal, 3u);
  EXPECT_EQ(c->total(), 1181u);
  EXPECT_EQ(c->idle_all(), 1020u);
  EXPECT_FALSE(parse_stat_cpu("cpu0 1 2 3 4\n").has_value());
  EXPECT_FALSE(parse_stat_cpu("cpu  x y z w\n").has_value());
}

TEST(Procfs, Meminfo) {
  auto m = parse_meminfo("MemTotal:  8000000 kB\nMemFree: 100 kB\nMemAvailable:   4000000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->total_kb, 8000000);
  EXPECT_EQ(m->available_kb, 4000000);
  EXPECT_FALSE(parse_meminfo("MemTotal: 5 kB\n").has_value()) << "MemAvailable required (kernel >= 3.14)";
}

TEST(Procfs, Psi) {
  auto p = parse_psi("some avg10=12.50 avg60=3.00 avg300=1.00 total=123\nfull avg10=4.25 avg60=0.00 avg300=0.00 total=5\n");
  ASSERT_TRUE(p.has_value());
  EXPECT_DOUBLE_EQ(p->some_avg10, 12.5);
  EXPECT_DOUBLE_EQ(*p->full_avg10, 4.25);
  auto cpu_old = parse_psi("some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
  ASSERT_TRUE(cpu_old.has_value());
  EXPECT_FALSE(cpu_old->full_avg10.has_value());
  EXPECT_FALSE(parse_psi("garbage\n").has_value());
}

TEST(Procfs, SnmpTables) {
  auto t = parse_snmp(
      "Ip: Forwarding DefaultTTL\nIp: 1 64\n"
      "Tcp: RtoAlgorithm RtoMin ActiveOpens OutSegs RetransSegs\nTcp: 1 200 10 5000 25\n"
      "Udp: InDatagrams InErrors\nUdp: 7 1\n");
  EXPECT_EQ(t["Tcp"]["OutSegs"], 5000);
  EXPECT_EQ(t["Tcp"]["RetransSegs"], 25);
  EXPECT_EQ(t["Udp"]["InErrors"], 1);
  auto mismatched = parse_snmp("Tcp: A B\nUdp: 1 2\n");
  EXPECT_TRUE(mismatched.empty());
}

TEST(Procfs, ConntrackStatSumsCpusInHex) {
  auto s = parse_conntrack_stat(
      "entries  clashres found new invalid ignore delete delete_list insert insert_failed drop early_drop\n"
      "00000010 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 0000000a 00000001 00000000\n"
      "00000010 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000005 00000000 00000000\n");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ((*s)["insert_failed"], 15u);
  EXPECT_EQ((*s)["drop"], 1u);
  EXPECT_FALSE(parse_conntrack_stat("a b\n1 2 3\n").has_value());
}

TEST(Procfs, DefaultRoute) {
  // 0101A8C0 is 192.168.1.1 printed in little-endian host order.
  auto r = parse_default_route(
      "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
      "eth0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
      "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->ifname, "eth0");
  EXPECT_EQ(r->gateway, "192.168.1.1");
  EXPECT_FALSE(parse_default_route("Iface\tDestination\n").has_value());
}

TEST(Procfs, ResolvConf) {
  auto v = parse_resolv_conf("# x\nnameserver 127.0.0.53\nnameserver 2001:db8::1\nnameserver bogus\nsearch lan\n");
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[0], "127.0.0.53");
  EXPECT_EQ(v[1], "2001:db8::1");
}
