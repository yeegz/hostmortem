// rtnetlink message parsing. Takes a raw recv() buffer, walks nlmsghdr
// records with bounds checks (NLMSG_OK) and decodes the link, address and
// route messages we track. Fuzzed: the buffer is kernel-produced, but a
// parser bug here would crash a privileged daemon.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hm::nl {

struct LinkMsg {
  bool removed = false;
  int ifindex = 0;
  std::string ifname;
  bool admin_up = false;
  bool lower_up = false;          // IFF_LOWER_UP: carrier
  std::string oper = "unknown";   // IF_OPER_* as text
};

struct AddrMsg {
  bool removed = false;
  int ifindex = 0;
  int family = 0;
  int prefixlen = 0;
  std::string address;
};

struct RouteMsg {
  bool removed = false;
  int family = 0;
  int dst_len = 0;
  std::string dst;
  std::string gateway;
  int oif = 0;
  int table = 0;
  bool is_default() const { return dst_len == 0; }
};

struct ErrorMsg {
  int error = 0;  // negative errno, 0 = ACK
};

struct Done {};

using Message = std::variant<LinkMsg, AddrMsg, RouteMsg, ErrorMsg, Done>;

// Parses every complete message in the buffer. Malformed records stop the
// walk; what was decoded before is returned and `malformed` is set.
std::vector<Message> parse(const void* buf, std::size_t len, bool* malformed = nullptr);

const char* oper_state_name(int state) noexcept;

}  // namespace hm::nl
