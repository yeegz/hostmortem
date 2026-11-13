// Parsers for procfs text. Pure functions over strings: unit-tested with
// captured fixtures and fuzzed, because a kernel upgrade can change a format
// and the agent must degrade, not crash.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hm::procfs {

struct CpuTimes {
  std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  std::uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
  std::uint64_t idle_all() const { return idle + iowait; }
};
std::optional<CpuTimes> parse_stat_cpu(std::string_view proc_stat);

struct MemInfo {
  std::int64_t total_kb = -1, available_kb = -1, swap_total_kb = -1, swap_free_kb = -1;
};
std::optional<MemInfo> parse_meminfo(std::string_view text);

struct Psi {
  double some_avg10 = 0;
  std::optional<double> full_avg10;  // /proc/pressure/cpu has no "full" line before 5.13
};
std::optional<Psi> parse_psi(std::string_view text);

// /proc/net/snmp and /proc/net/netstat: alternating header/value lines,
// "Tcp: RtoAlgorithm RtoMin ..." then "Tcp: 1 200 ...".
using Table = std::map<std::string, std::map<std::string, std::int64_t>>;
Table parse_snmp(std::string_view text);

// /proc/net/stat/nf_conntrack: header line then one hex row per CPU; summed.
std::optional<std::map<std::string, std::uint64_t>> parse_conntrack_stat(std::string_view text);

struct DefaultRoute {
  std::string ifname;
  std::string gateway;  // dotted quad
};
// /proc/net/route (IPv4 only; IPv6 default routes come from netlink).
std::optional<DefaultRoute> parse_default_route(std::string_view text);

// nameserver lines from resolv.conf (IPv4/IPv6 literals; others skipped).
std::vector<std::string> parse_resolv_conf(std::string_view text);

}  // namespace hm::procfs
