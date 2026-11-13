// Agent configuration: a small INI dialect ("[section]" and "key = value",
// '#' comments, comma lists). Chosen over TOML/YAML to avoid a parser
// dependency for ~15 keys; the parser is fuzzed because the file may be
// edited by hand.
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace hm {

struct TcpTarget {
  std::string host;
  std::uint16_t port = 0;
};

struct Config {
  std::string db_path = "/var/lib/hostmortem/events.db";

  // [sampler]
  std::chrono::milliseconds sample_interval{5000};

  // [probe]
  std::chrono::milliseconds probe_interval{10000};
  std::chrono::milliseconds probe_timeout{2000};
  std::vector<std::string> dns_names{"example.com"};
  std::vector<std::string> dns_servers;  // empty = read resolv.conf
  std::string resolv_conf = "/etc/resolv.conf";
  std::vector<TcpTarget> tcp_targets;
  std::vector<std::string> icmp_targets{"gateway"};  // "gateway" = default route

  // [watch]
  std::vector<std::string> watch_processes;

  // [store]
  std::chrono::milliseconds commit_interval{5000};
  std::size_t commit_batch = 500;
  int retention_days = 14;
  std::int64_t max_store_mb = 256;

  // [privacy]
  std::vector<std::string> clear_names;  // DNS names stored without hashing

  // [detector]
  int probe_fail_streak = 3;
  std::chrono::milliseconds incident_quiet{120000};
};

Result<Config> parse_config(std::string_view text);
Result<Config> load_config(const std::string& path);
Result<std::chrono::milliseconds> parse_duration(std::string_view s);

}  // namespace hm
