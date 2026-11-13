#include <gtest/gtest.h>

#include "collectors/kmsg_parse.hpp"
#include "core/kinds.hpp"

using namespace hm;

TEST(Kmsg, ParsesRecord) {
  auto r = kmsg::parse_record("3,1234,5678901,-;Out of memory: Killed process 4242 (stress-ng) total-vm:1kB\n SUBSYSTEM=x\n");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->level, 3);
  EXPECT_EQ(r->seq, 1234u);
  EXPECT_EQ(r->usec, 5678901u);
  EXPECT_EQ(r->message, "Out of memory: Killed process 4242 (stress-ng) total-vm:1kB");
  EXPECT_FALSE(kmsg::parse_record("no semicolon").has_value());
  EXPECT_FALSE(kmsg::parse_record("x,1,2;msg").has_value());
  EXPECT_FALSE(kmsg::parse_record("1,2;msg").has_value());
}

TEST(Kmsg, ClassifiesOom) {
  kmsg::Record r{3, 0, 1, 1, "Out of memory: Killed process 4242 (stress-ng) total-vm:1kB"};
  auto c = kmsg::classify(r);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->kind, kind::kernel_oom);
  EXPECT_EQ(std::get<std::int64_t>(c->attrs["pid"]), 4242);
  EXPECT_EQ(std::get<std::string>(c->attrs["comm"]), "stress-ng");
  kmsg::Record memcg{3, 0, 1, 1, "Memory cgroup out of memory: Killed process 77 (app) total-vm:1kB"};
  auto c2 = kmsg::classify(memcg);
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(c2->kind, kind::kernel_oom);
  EXPECT_TRUE(std::get<bool>(c2->attrs["memcg"]));
}

TEST(Kmsg, ClassifiesNetdevWatchdogBothFormats) {
  kmsg::Record newer{4, 0, 1, 1, "r8169 0000:02:00.0 enp2s0: NETDEV WATCHDOG: CPU: 3: transmit queue 0 timed out 5120 ms"};
  auto a = kmsg::classify(newer);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->kind, kind::kernel_netdev_watchdog);
  EXPECT_EQ(a->subject, "enp2s0");
  kmsg::Record older{4, 0, 1, 1, "NETDEV WATCHDOG: eth0 (r8169): transmit queue 0 timed out"};
  auto b = kmsg::classify(older);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->subject, "eth0");
}

TEST(Kmsg, ClassifiesLinkDownAndConntrack) {
  auto l = kmsg::classify({6, 0, 1, 1, "e1000e 0000:00:1f.6 eno1: NIC Link is Down"});
  // "NIC Link is Down" is not ": Link is Down": ignored at info level.
  EXPECT_FALSE(l.has_value());
  auto l2 = kmsg::classify({6, 0, 1, 1, "bcmgenet fd580000.ethernet eth0: Link is Down"});
  ASSERT_TRUE(l2.has_value());
  EXPECT_EQ(l2->subject, "eth0");
  auto ct = kmsg::classify({4, 0, 1, 1, "nf_conntrack: nf_conntrack: table full, dropping packet"});
  ASSERT_TRUE(ct.has_value());
  EXPECT_EQ(ct->kind, kind::kernel_conntrack_full);
}

TEST(Kmsg, KeepsErrorsDropsInfo) {
  EXPECT_TRUE(kmsg::classify({3, 0, 1, 1, "something failed"}).has_value());
  EXPECT_FALSE(kmsg::classify({6, 0, 1, 1, "usb 1-1: new device"}).has_value());
}
