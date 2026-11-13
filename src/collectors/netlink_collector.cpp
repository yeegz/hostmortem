#include "collectors/netlink_collector.hpp"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstring>

#include "core/log.hpp"

namespace hm {

namespace {
constexpr std::size_t kBuf = 64 * 1024;
}

bool NetlinkCollector::open_socket() {
  UniqueFd fd(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
  if (!fd.valid()) {
    em_.status("netlink", "unavailable", "socket(AF_NETLINK): " + errno_name(errno));
    return false;
  }
  int rcvbuf = 1 << 20;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  sockaddr_nl sa{};
  sa.nl_family = AF_NETLINK;
  sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    em_.status("netlink", "unavailable", "bind(RTMGRP_*): " + errno_name(errno));
    return false;
  }
  fd_ = std::move(fd);
  return true;
}

bool NetlinkCollector::request_dump(int type) {
  struct {
    nlmsghdr nh;
    rtgenmsg g;
  } req{};
  req.nh.nlmsg_len = sizeof req;
  req.nh.nlmsg_type = static_cast<std::uint16_t>(type);
  req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.nh.nlmsg_seq = ++seq_;
  req.g.rtgen_family = AF_UNSPEC;
  return ::send(fd_.get(), &req, sizeof req, 0) == static_cast<ssize_t>(sizeof req);
}

// Reads multipart (NLM_F_MULTI) dump replies until NLMSG_DONE.
bool NetlinkCollector::drain_dump(std::stop_token& stop) {
  alignas(nlmsghdr) std::array<char, kBuf> buf;
  for (int i = 0; i < 1000 && !stop.stop_requested(); ++i) {
    pollfd p{fd_.get(), POLLIN, 0};
    if (::poll(&p, 1, 2000) <= 0) return false;
    const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
    if (n <= 0) return false;
    bool bad = false;
    auto msgs = nl::parse(buf.data(), static_cast<std::size_t>(n), &bad);
    for (const auto& m : msgs) {
      if (std::holds_alternative<nl::Done>(m)) return true;
      handle(m, true);
    }
    if (bad) return false;
  }
  return false;
}

void NetlinkCollector::handle(const nl::Message& msg, bool initial) {
  if (const auto* l = std::get_if<nl::LinkMsg>(&msg)) {
    auto& prev = links_[l->ifindex];
    const bool changed = prev.ifname.empty() || prev.oper != l->oper || prev.up != l->admin_up ||
                         prev.carrier != l->lower_up || l->removed;
    const std::string ifname = l->ifname.empty() ? prev.ifname : l->ifname;
    if (changed && !ifname.empty()) {
      const bool bad = !initial && ifname != "lo" &&
                       (l->removed || l->oper == "down" || l->oper == "lowerlayerdown" || !l->lower_up);
      em_.emit(kind::link_state, bad ? Severity::warning : Severity::info, ifname,
               {{"ifname", ifname}, {"ifindex", static_cast<std::int64_t>(l->ifindex)},
                {"oper", l->removed ? std::string("removed") : l->oper},
                {"up", l->admin_up}, {"carrier", l->lower_up}, {"initial", initial}});
    }
    prev = {ifname, l->oper, l->admin_up, l->lower_up};
    if (l->removed) links_.erase(l->ifindex);
  } else if (const auto* a = std::get_if<nl::AddrMsg>(&msg)) {
    if (initial) return;
    auto it = links_.find(a->ifindex);
    const std::string ifname = it == links_.end() ? std::to_string(a->ifindex) : it->second.ifname;
    em_.emit(a->removed ? kind::addr_del : kind::addr_add, a->removed ? Severity::notice : Severity::info, ifname,
             {{"ifname", ifname}, {"family", std::string(a->family == AF_INET6 ? "ipv6" : "ipv4")},
              {"address", a->address}, {"prefixlen", static_cast<std::int64_t>(a->prefixlen)}});
  } else if (const auto* r = std::get_if<nl::RouteMsg>(&msg)) {
    if (!r->is_default() || r->table != RT_TABLE_MAIN) return;  // only default routes matter here
    auto it = links_.find(r->oif);
    const std::string ifname = it == links_.end() ? std::to_string(r->oif) : it->second.ifname;
    em_.emit(r->removed ? kind::route_del : kind::route_add, r->removed ? Severity::warning : Severity::info,
             "default", {{"default", true}, {"family", std::string(r->family == AF_INET6 ? "ipv6" : "ipv4")},
                         {"gateway", r->gateway}, {"ifname", ifname}, {"initial", initial}});
  }
}

void NetlinkCollector::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    if (!fd_.valid() && !open_socket()) {
      if (!sleep_for(stop, std::chrono::seconds(30))) return;
      continue;
    }
    bool ok = request_dump(RTM_GETLINK) && drain_dump(stop) && request_dump(RTM_GETROUTE) && drain_dump(stop);
    if (!ok) {
      em_.status("netlink", "degraded", "initial dump failed: " + errno_name(errno));
    } else {
      em_.status("netlink", "active");
    }
    alignas(nlmsghdr) std::array<char, kBuf> buf;
    bool resync = false;
    while (!stop.stop_requested() && !resync) {
      pollfd p{fd_.get(), POLLIN, 0};
      const int pr = ::poll(&p, 1, 500);
      if (pr < 0 && errno != EINTR) break;
      if (pr <= 0) continue;
      const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), MSG_DONTWAIT);
      if (n < 0) {
        if (errno == ENOBUFS) {
          // The kernel overran our socket buffer: notifications were lost.
          em_.emit(kind::pipeline_drop, Severity::warning, "netlink",
                   {{"origin", std::string("netlink")}, {"reason", std::string("ENOBUFS")}, {"count", std::int64_t{-1}}});
          resync = true;
        } else if (errno != EAGAIN && errno != EINTR) {
          em_.status("netlink", "degraded", "recv: " + errno_name(errno));
          fd_.reset();
          break;
        }
        continue;
      }
      bool bad = false;
      for (const auto& m : nl::parse(buf.data(), static_cast<std::size_t>(n), &bad)) handle(m, false);
      if (bad) log::warn("netlink", "malformed netlink message ignored");
    }
  }
}

}  // namespace hm
