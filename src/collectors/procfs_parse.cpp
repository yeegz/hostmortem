#include "collectors/procfs_parse.hpp"

#include <arpa/inet.h>

#include <charconv>
#include <cstdio>

namespace hm::procfs {
namespace {

std::vector<std::string_view> lines(std::string_view text) {
  std::vector<std::string_view> out;
  while (!text.empty()) {
    const auto nl = text.find('\n');
    out.push_back(text.substr(0, nl));
    if (nl == std::string_view::npos) break;
    text.remove_prefix(nl + 1);
  }
  return out;
}

std::vector<std::string_view> fields(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
  return out;
}

template <typename T>
std::optional<T> num(std::string_view s, int base = 10) {
  T v{};
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
  if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<double> parse_double_kv(std::string_view field, std::string_view key) {
  // "avg10=1.23"
  if (field.substr(0, key.size()) != key || field.size() <= key.size() || field[key.size()] != '=') return std::nullopt;
  const std::string val(field.substr(key.size() + 1));
  char* end = nullptr;
  const double d = std::strtod(val.c_str(), &end);
  if (end == val.c_str() || *end != '\0') return std::nullopt;
  return d;
}

}  // namespace

std::optional<CpuTimes> parse_stat_cpu(std::string_view text) {
  for (auto line : lines(text)) {
    auto f = fields(line);
    if (f.empty() || f[0] != "cpu") continue;
    if (f.size() < 5) return std::nullopt;
    CpuTimes c;
    std::uint64_t* slots[] = {&c.user, &c.nice, &c.system, &c.idle, &c.iowait, &c.irq, &c.softirq, &c.steal};
    for (std::size_t i = 0; i < 8 && i + 1 < f.size(); ++i) {
      auto v = num<std::uint64_t>(f[i + 1]);
      if (!v) return std::nullopt;
      *slots[i] = *v;
    }
    return c;
  }
  return std::nullopt;
}

std::optional<MemInfo> parse_meminfo(std::string_view text) {
  MemInfo m;
  for (auto line : lines(text)) {
    auto f = fields(line);
    if (f.size() < 2) continue;
    auto v = num<std::int64_t>(f[1]);
    if (!v) continue;
    if (f[0] == "MemTotal:") m.total_kb = *v;
    else if (f[0] == "MemAvailable:") m.available_kb = *v;
    else if (f[0] == "SwapTotal:") m.swap_total_kb = *v;
    else if (f[0] == "SwapFree:") m.swap_free_kb = *v;
  }
  if (m.total_kb < 0 || m.available_kb < 0) return std::nullopt;
  return m;
}

std::optional<Psi> parse_psi(std::string_view text) {
  Psi p;
  bool some = false;
  for (auto line : lines(text)) {
    auto f = fields(line);
    if (f.size() < 2) continue;
    auto v = parse_double_kv(f[1], "avg10");
    if (!v) return std::nullopt;
    if (f[0] == "some") { p.some_avg10 = *v; some = true; }
    else if (f[0] == "full") p.full_avg10 = *v;
  }
  if (!some) return std::nullopt;
  return p;
}

Table parse_snmp(std::string_view text) {
  Table t;
  auto ls = lines(text);
  for (std::size_t i = 0; i + 1 < ls.size(); i += 2) {
    auto head = fields(ls[i]);
    auto vals = fields(ls[i + 1]);
    if (head.empty() || vals.empty() || head[0] != vals[0] || head.size() != vals.size()) continue;
    std::string proto(head[0]);
    if (!proto.empty() && proto.back() == ':') proto.pop_back();
    auto& row = t[proto];
    for (std::size_t k = 1; k < head.size(); ++k) {
      if (auto v = num<std::int64_t>(vals[k])) row[std::string(head[k])] = *v;
    }
  }
  return t;
}

std::optional<std::map<std::string, std::uint64_t>> parse_conntrack_stat(std::string_view text) {
  auto ls = lines(text);
  if (ls.empty()) return std::nullopt;
  auto head = fields(ls[0]);
  if (head.empty()) return std::nullopt;
  std::map<std::string, std::uint64_t> sum;
  for (auto h : head) sum[std::string(h)] = 0;
  bool any = false;
  for (std::size_t i = 1; i < ls.size(); ++i) {
    auto vals = fields(ls[i]);
    if (vals.empty()) continue;
    if (vals.size() != head.size()) return std::nullopt;
    for (std::size_t k = 0; k < vals.size(); ++k) {
      auto v = num<std::uint64_t>(vals[k], 16);
      if (!v) return std::nullopt;
      sum[std::string(head[k])] += *v;
    }
    any = true;
  }
  if (!any) return std::nullopt;
  return sum;
}

std::optional<DefaultRoute> parse_default_route(std::string_view text) {
  auto ls = lines(text);
  for (std::size_t i = 1; i < ls.size(); ++i) {
    auto f = fields(ls[i]);
    if (f.size() < 8) continue;
    if (f[1] != "00000000") continue;  // destination 0.0.0.0
    auto mask = num<std::uint32_t>(f[7], 16);
    if (!mask || *mask != 0) continue;
    auto gw = num<std::uint32_t>(f[2], 16);
    if (!gw) continue;
    // /proc/net/route prints the address in host byte order of a
    // network-order value; on little-endian hosts that is reversed text.
    in_addr a{};
    a.s_addr = *gw;
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &a, buf, sizeof buf)) continue;
    return DefaultRoute{std::string(f[0]), buf};
  }
  return std::nullopt;
}

std::vector<std::string> parse_resolv_conf(std::string_view text) {
  std::vector<std::string> out;
  for (auto line : lines(text)) {
    auto f = fields(line);
    if (f.size() < 2 || f[0] != "nameserver") continue;
    const std::string addr(f[1]);
    in6_addr a6{};
    in_addr a4{};
    if (inet_pton(AF_INET, addr.c_str(), &a4) == 1 || inet_pton(AF_INET6, addr.c_str(), &a6) == 1) out.push_back(addr);
  }
  return out;
}

}  // namespace hm::procfs
