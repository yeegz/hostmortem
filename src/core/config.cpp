#include "core/config.hpp"

#include <charconv>
#include <fstream>
#include <sstream>

namespace hm {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::vector<std::string> split_list(std::string_view v) {
  std::vector<std::string> out;
  while (!v.empty()) {
    const auto comma = v.find(',');
    auto item = trim(v.substr(0, comma));
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    v.remove_prefix(comma + 1);
  }
  return out;
}

Result<std::int64_t> parse_int(std::string_view s) {
  std::int64_t v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size()) return make_error("bad_int", std::string(s));
  return v;
}

Error at(int line, const std::string& msg) {
  return make_error("bad_config", "line " + std::to_string(line) + ": " + msg);
}

}  // namespace

Result<std::chrono::milliseconds> parse_duration(std::string_view s) {
  s = trim(s);
  std::int64_t mult = 1000;  // bare numbers are seconds
  if (s.ends_with("ms")) { mult = 1; s.remove_suffix(2); }
  else if (s.ends_with('s')) { mult = 1000; s.remove_suffix(1); }
  else if (s.ends_with('m')) { mult = 60'000; s.remove_suffix(1); }
  else if (s.ends_with('h')) { mult = 3'600'000; s.remove_suffix(1); }
  auto n = parse_int(s);
  if (!n || *n < 0 || *n > 7LL * 24 * 3600 * 1000) return make_error("bad_duration", std::string(s));
  return std::chrono::milliseconds(*n * mult);
}

Result<Config> parse_config(std::string_view text) {
  Config c;
  std::string section;
  int lineno = 0;
  while (!text.empty()) {
    ++lineno;
    const auto nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']') return at(lineno, "unterminated section");
      section = std::string(trim(line.substr(1, line.size() - 2)));
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) return at(lineno, "expected key = value");
    const std::string key = section + "." + std::string(trim(line.substr(0, eq)));
    const std::string_view val = trim(line.substr(eq + 1));

    auto dur = [&](std::chrono::milliseconds& out) -> Status {
      auto d = parse_duration(val);
      if (!d) return at(lineno, key + ": bad duration");
      out = *d;
      return ok_status();
    };
    auto num = [&](std::int64_t lo, std::int64_t hi) -> Result<std::int64_t> {
      auto n = parse_int(val);
      if (!n || *n < lo || *n > hi) return at(lineno, key + ": out of range");
      return n;
    };

    Status st = ok_status();
    if (key == "store.path") c.db_path = std::string(val);
    else if (key == "sampler.interval") st = dur(c.sample_interval);
    else if (key == "probe.interval") st = dur(c.probe_interval);
    else if (key == "probe.timeout") st = dur(c.probe_timeout);
    else if (key == "probe.dns_names") c.dns_names = split_list(val);
    else if (key == "probe.dns_servers") c.dns_servers = split_list(val);
    else if (key == "probe.resolv_conf") c.resolv_conf = std::string(val);
    else if (key == "probe.icmp_targets") c.icmp_targets = split_list(val);
    else if (key == "probe.tcp_targets") {
      c.tcp_targets.clear();
      for (const auto& t : split_list(val)) {
        const auto colon = t.rfind(':');
        if (colon == std::string::npos) return at(lineno, "tcp target needs host:port");
        auto port = parse_int(std::string_view(t).substr(colon + 1));
        if (!port || *port < 1 || *port > 65535) return at(lineno, "bad tcp port");
        std::string host = t.substr(0, colon);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
        if (host.empty()) return at(lineno, "empty tcp target host");
        c.tcp_targets.push_back({host, static_cast<std::uint16_t>(*port)});
      }
    }
    else if (key == "watch.processes") c.watch_processes = split_list(val);
    else if (key == "store.commit_interval") st = dur(c.commit_interval);
    else if (key == "store.commit_batch") {
      auto n = num(1, 100000);
      if (!n) return n.error();
      c.commit_batch = static_cast<std::size_t>(*n);
    }
    else if (key == "store.retention_days") {
      auto n = num(1, 3650);
      if (!n) return n.error();
      c.retention_days = static_cast<int>(*n);
    }
    else if (key == "store.max_mb") {
      auto n = num(8, 1 << 20);
      if (!n) return n.error();
      c.max_store_mb = *n;
    }
    else if (key == "privacy.clear_names") c.clear_names = split_list(val);
    else if (key == "detector.probe_fail_streak") {
      auto n = num(1, 100);
      if (!n) return n.error();
      c.probe_fail_streak = static_cast<int>(*n);
    }
    else if (key == "detector.incident_quiet") st = dur(c.incident_quiet);
    else return at(lineno, "unknown key " + key);
    if (!st) return st.error();
  }
  if (c.probe_timeout >= c.probe_interval) {
    return make_error("bad_config", "probe.timeout must be shorter than probe.interval");
  }
  return c;
}

Result<Config> load_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) return make_error("config_unreadable", "cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  if (text.size() > 1 << 20) return make_error("bad_config", "config larger than 1 MiB");
  return parse_config(text);
}

}  // namespace hm
