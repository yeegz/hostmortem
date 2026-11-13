#include "collectors/kmsg_parse.hpp"

#include <cctype>
#include <charconv>

#include "core/kinds.hpp"

namespace hm::kmsg {
namespace {

template <typename T>
std::optional<T> num(std::string_view s) {
  T v{};
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// The token immediately before `marker` (e.g. "eth0" in "r8169 0000:01:00.0 eth0: Link is Down").
std::string token_before(std::string_view msg, std::size_t marker_pos) {
  std::size_t end = marker_pos;
  std::size_t start = end;
  while (start > 0 && msg[start - 1] != ' ') --start;
  return std::string(msg.substr(start, end - start));
}

bool plausible_ifname(std::string_view s) {
  if (s.empty() || s.size() > 15) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '@')) return false;
  }
  return true;
}

}  // namespace

std::optional<Record> parse_record(std::string_view raw) {
  const auto semi = raw.find(';');
  if (semi == std::string_view::npos) return std::nullopt;
  std::string_view header = raw.substr(0, semi);
  std::string_view msg = raw.substr(semi + 1);
  if (const auto nl = msg.find('\n'); nl != std::string_view::npos) msg = msg.substr(0, nl);

  std::string_view parts[4];
  std::size_t n = 0;
  while (n < 4) {
    const auto comma = header.find(',');
    parts[n++] = header.substr(0, comma);
    if (comma == std::string_view::npos) break;
    header.remove_prefix(comma + 1);
  }
  if (n < 3) return std::nullopt;
  auto prefix = num<int>(parts[0]);
  auto seq = num<std::uint64_t>(parts[1]);
  auto usec = num<std::uint64_t>(parts[2]);
  if (!prefix || !seq || !usec || *prefix < 0) return std::nullopt;
  Record r;
  r.level = *prefix & 7;
  r.facility = *prefix >> 3;
  r.seq = *seq;
  r.usec = *usec;
  r.message = std::string(msg.substr(0, 1024));
  return r;
}

std::optional<Classified> classify(const Record& r) {
  const std::string_view m = r.message;

  // OOM killer: "Out of memory: Killed process 1234 (name) ..." and the
  // memcg variant "Memory cgroup out of memory: Killed process 1234 (name)".
  const auto killed = m.find("Killed process ");
  if (killed != std::string_view::npos &&
      (m.find("Out of memory") != std::string_view::npos || m.find("out of memory") != std::string_view::npos)) {
    const auto pos = killed;
    std::string_view rest = m.substr(pos + 15);
    const auto sp = rest.find(' ');
    auto pid = num<std::int64_t>(rest.substr(0, sp));
    std::string comm;
    if (const auto lp = rest.find('('); lp != std::string_view::npos) {
      if (const auto rp = rest.find(')', lp); rp != std::string_view::npos) comm = std::string(rest.substr(lp + 1, rp - lp - 1));
    }
    Classified c{kind::kernel_oom, comm, Severity::error, {}};
    c.attrs["pid"] = pid.value_or(0);
    c.attrs["comm"] = comm;
    c.attrs["memcg"] = m.find("Memory cgroup") != std::string_view::npos;
    return c;
  }
  if (auto pos = m.find("NETDEV WATCHDOG"); pos != std::string_view::npos) {
    std::string ifname;
    // Newer kernels: "<drv> <bus> eth0: NETDEV WATCHDOG: CPU: 1: transmit queue 0 timed out"
    if (pos >= 2 && m.substr(pos - 2, 2) == ": ") ifname = token_before(m, pos - 2);
    // Older kernels: "NETDEV WATCHDOG: eth0 (r8169): transmit queue 0 timed out"
    if (!plausible_ifname(ifname)) {
      // Bounds-checked: a message may end right after the marker (found by
      // fuzz_kmsg; substr past the end throws std::out_of_range).
      constexpr std::string_view kMarker = "NETDEV WATCHDOG:";
      std::string_view rest = pos + kMarker.size() <= m.size() ? m.substr(pos + kMarker.size()) : std::string_view{};
      while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
      ifname = std::string(rest.substr(0, rest.find_first_of(" :")));
    }
    if (!plausible_ifname(ifname) || ifname == "CPU") ifname = "unknown";
    Classified c{kind::kernel_netdev_watchdog, ifname, Severity::error, {}};
    c.attrs["ifname"] = ifname;
    return c;
  }
  if (m.find("nf_conntrack: table full") != std::string_view::npos) {
    return Classified{kind::kernel_conntrack_full, "nf_conntrack", Severity::error, {}};
  }
  if (auto pos = m.find(": Link is Down"); pos != std::string_view::npos) {
    std::string ifname = token_before(m, pos);
    if (!plausible_ifname(ifname)) ifname = "unknown";
    Classified c{kind::kernel_link_down, ifname, Severity::warning, {}};
    c.attrs["ifname"] = ifname;
    return c;
  }
  if (r.level <= 3) {  // KERN_ERR and worse
    Classified c{kind::kernel_log, {}, r.level <= 2 ? Severity::critical : Severity::error, {}};
    c.attrs["level"] = static_cast<std::int64_t>(r.level);
    c.attrs["msg"] = r.message;
    return c;
  }
  return std::nullopt;
}

}  // namespace hm::kmsg
