#include <gtest/gtest.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <cstring>
#include <vector>

#include "collectors/netlink_parse.hpp"

using namespace hm;

namespace {
// Builds an RTM_NEWLINK message the way the kernel lays it out.
std::vector<std::uint32_t> link_msg(int index, const char* name, unsigned flags, unsigned char oper) {
  std::vector<char> b(NLMSG_SPACE(sizeof(ifinfomsg)) + RTA_SPACE(IFNAMSIZ) + RTA_SPACE(1), 0);
  auto* nh = reinterpret_cast<nlmsghdr*>(b.data());
  nh->nlmsg_type = RTM_NEWLINK;
  auto* ifi = reinterpret_cast<ifinfomsg*>(NLMSG_DATA(nh));
  ifi->ifi_index = index;
  ifi->ifi_flags = flags;
  auto* rta = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(ifi) + NLMSG_ALIGN(sizeof(ifinfomsg)));
  rta->rta_type = IFLA_IFNAME;
  rta->rta_len = static_cast<unsigned short>(RTA_LENGTH(std::strlen(name) + 1));
  std::memcpy(RTA_DATA(rta), name, std::strlen(name) + 1);
  auto* rta2 = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(rta) + RTA_ALIGN(rta->rta_len));
  rta2->rta_type = IFLA_OPERSTATE;
  rta2->rta_len = static_cast<unsigned short>(RTA_LENGTH(1));
  *static_cast<unsigned char*>(RTA_DATA(rta2)) = oper;
  nh->nlmsg_len = static_cast<std::uint32_t>(reinterpret_cast<char*>(rta2) + RTA_ALIGN(rta2->rta_len) - b.data());
  std::vector<std::uint32_t> aligned((nh->nlmsg_len + 3) / 4);
  std::memcpy(aligned.data(), b.data(), nh->nlmsg_len);
  return aligned;
}
}  // namespace

TEST(NetlinkParse, DecodesLinkMessage) {
  auto buf = link_msg(2, "eth0", IFF_UP, IF_OPER_DOWN);
  bool bad = true;
  auto msgs = nl::parse(buf.data(), buf.size() * 4, &bad);
  EXPECT_FALSE(bad);
  ASSERT_EQ(msgs.size(), 1u);
  const auto& l = std::get<nl::LinkMsg>(msgs[0]);
  EXPECT_EQ(l.ifindex, 2);
  EXPECT_EQ(l.ifname, "eth0");
  EXPECT_TRUE(l.admin_up);
  EXPECT_FALSE(l.lower_up);
  EXPECT_EQ(l.oper, "down");
}

TEST(NetlinkParse, TruncatedBufferIsMalformedNotACrash) {
  auto buf = link_msg(2, "eth0", IFF_UP | IFF_LOWER_UP, IF_OPER_UP);
  bool bad = false;
  auto msgs = nl::parse(buf.data(), 10, &bad);
  EXPECT_TRUE(msgs.empty());
  EXPECT_TRUE(bad);
  // Lie about the length inside the header.
  reinterpret_cast<nlmsghdr*>(buf.data())->nlmsg_len = 4000;
  msgs = nl::parse(buf.data(), buf.size() * 4, &bad);
  EXPECT_TRUE(msgs.empty());
}
