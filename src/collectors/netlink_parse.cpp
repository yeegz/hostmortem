#include "collectors/netlink_parse.hpp"

#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <cstring>

namespace hm::nl {
namespace {

std::string addr_to_string(int family, const void* data, std::size_t len) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (family == AF_INET && len >= 4) {
    if (inet_ntop(AF_INET, data, buf, sizeof buf)) return buf;
  } else if (family == AF_INET6 && len >= 16) {
    if (inet_ntop(AF_INET6, data, buf, sizeof buf)) return buf;
  }
  return {};
}

// Iterate rtattrs within [attr, attr+len) with RTA_OK bounds checks.
template <typename F>
void for_each_attr(const rtattr* attr, int len, F&& f) {
  // RTA_* macros use C-style casts and unsigned arithmetic; isolate them here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wcast-align"
  for (; RTA_OK(attr, len); attr = RTA_NEXT(attr, len)) {
    f(attr->rta_type, RTA_DATA(attr), static_cast<std::size_t>(RTA_PAYLOAD(attr)));
  }
#pragma GCC diagnostic pop
}

}  // namespace

const char* oper_state_name(int state) noexcept {
  switch (state) {
    case IF_OPER_UNKNOWN: return "unknown";
    case IF_OPER_NOTPRESENT: return "notpresent";
    case IF_OPER_DOWN: return "down";
    case IF_OPER_LOWERLAYERDOWN: return "lowerlayerdown";
    case IF_OPER_TESTING: return "testing";
    case IF_OPER_DORMANT: return "dormant";
    case IF_OPER_UP: return "up";
    default: return "unknown";
  }
}

std::vector<Message> parse(const void* buf, std::size_t len, bool* malformed) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wsign-compare"
  std::vector<Message> out;
  if (malformed) *malformed = false;
  if (len > 1 << 24) {
    if (malformed) *malformed = true;
    return out;
  }
  int remaining = static_cast<int>(len);
  const auto* nh = static_cast<const nlmsghdr*>(buf);
  // The kernel aligns messages to 4 bytes; a misaligned caller buffer is a bug.
  if (reinterpret_cast<std::uintptr_t>(buf) % alignof(nlmsghdr) != 0) {
    if (malformed) *malformed = true;
    return out;
  }
  for (; NLMSG_OK(nh, remaining); nh = NLMSG_NEXT(nh, remaining)) {
    const int payload = static_cast<int>(nh->nlmsg_len) - static_cast<int>(NLMSG_HDRLEN);
    switch (nh->nlmsg_type) {
      case NLMSG_DONE:
        out.emplace_back(Done{});
        return out;
      case NLMSG_ERROR: {
        if (payload < static_cast<int>(sizeof(nlmsgerr))) { if (malformed) *malformed = true; return out; }
        const auto* err = static_cast<const nlmsgerr*>(NLMSG_DATA(nh));
        out.emplace_back(ErrorMsg{err->error});
        break;
      }
      case RTM_NEWLINK:
      case RTM_DELLINK: {
        if (payload < static_cast<int>(sizeof(ifinfomsg))) { if (malformed) *malformed = true; return out; }
        const auto* ifi = static_cast<const ifinfomsg*>(NLMSG_DATA(nh));
        LinkMsg m;
        m.removed = nh->nlmsg_type == RTM_DELLINK;
        m.ifindex = ifi->ifi_index;
        m.admin_up = (ifi->ifi_flags & IFF_UP) != 0;
        m.lower_up = (ifi->ifi_flags & IFF_LOWER_UP) != 0;
        for_each_attr(IFLA_RTA(ifi), payload - static_cast<int>(NLMSG_ALIGN(sizeof(ifinfomsg))),
                      [&](unsigned short type, const void* data, std::size_t n) {
                        if (type == IFLA_IFNAME && n > 0) {
                          m.ifname.assign(static_cast<const char*>(data), strnlen(static_cast<const char*>(data), n));
                        } else if (type == IFLA_OPERSTATE && n >= 1) {
                          m.oper = oper_state_name(*static_cast<const unsigned char*>(data));
                        }
                      });
        out.emplace_back(std::move(m));
        break;
      }
      case RTM_NEWADDR:
      case RTM_DELADDR: {
        if (payload < static_cast<int>(sizeof(ifaddrmsg))) { if (malformed) *malformed = true; return out; }
        const auto* ifa = static_cast<const ifaddrmsg*>(NLMSG_DATA(nh));
        AddrMsg m;
        m.removed = nh->nlmsg_type == RTM_DELADDR;
        m.ifindex = static_cast<int>(ifa->ifa_index);
        m.family = ifa->ifa_family;
        m.prefixlen = ifa->ifa_prefixlen;
        for_each_attr(IFA_RTA(ifa), payload - static_cast<int>(NLMSG_ALIGN(sizeof(ifaddrmsg))),
                      [&](unsigned short type, const void* data, std::size_t n) {
                        if (type == IFA_ADDRESS || (type == IFA_LOCAL && m.address.empty())) {
                          m.address = addr_to_string(m.family, data, n);
                        }
                      });
        out.emplace_back(std::move(m));
        break;
      }
      case RTM_NEWROUTE:
      case RTM_DELROUTE: {
        if (payload < static_cast<int>(sizeof(rtmsg))) { if (malformed) *malformed = true; return out; }
        const auto* rt = static_cast<const rtmsg*>(NLMSG_DATA(nh));
        RouteMsg m;
        m.removed = nh->nlmsg_type == RTM_DELROUTE;
        m.family = rt->rtm_family;
        m.dst_len = rt->rtm_dst_len;
        m.table = rt->rtm_table;
        for_each_attr(RTM_RTA(rt), payload - static_cast<int>(NLMSG_ALIGN(sizeof(rtmsg))),
                      [&](unsigned short type, const void* data, std::size_t n) {
                        if (type == RTA_DST) m.dst = addr_to_string(m.family, data, n);
                        else if (type == RTA_GATEWAY) m.gateway = addr_to_string(m.family, data, n);
                        else if (type == RTA_OIF && n >= 4) {
                          std::int32_t v;
                          std::memcpy(&v, data, 4);
                          m.oif = v;
                        } else if (type == RTA_TABLE && n >= 4) {
                          std::uint32_t v;
                          std::memcpy(&v, data, 4);
                          m.table = static_cast<int>(v);
                        }
                      });
        out.emplace_back(std::move(m));
        break;
      }
      default:
        break;  // other message types are ignored, not errors
    }
  }
  if (remaining != 0 && malformed) *malformed = true;
  return out;
#pragma GCC diagnostic pop
}

}  // namespace hm::nl
