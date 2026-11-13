#pragma once
#include <optional>

#include "collectors/collector.hpp"
#include "collectors/procfs_parse.hpp"
#include "core/config.hpp"

namespace hm {

// Samples CPU, memory, PSI, TCP/UDP counters, conntrack stats and clock
// state every `interval`. Counters are emitted as deltas so the store holds
// rates, not ever-growing totals.
class ProcfsSampler final : public Collector {
 public:
  ProcfsSampler(Emitter& em, std::chrono::milliseconds interval, std::string proc_root = "/proc")
      : em_(em), interval_(interval), root_(std::move(proc_root)) {}
  std::string_view name() const override { return "procfs"; }
  void run(std::stop_token stop) override;
  void sample_once();  // exposed for tests

 private:
  Emitter& em_;
  std::chrono::milliseconds interval_;
  std::string root_;
  bool reported_ = false;
  bool psi_ok_ = false;
  bool ct_ok_ = false;
  std::optional<procfs::CpuTimes> last_cpu_;
  std::optional<procfs::Table> last_snmp_;
  std::optional<procfs::Table> last_netstat_;
  std::optional<std::map<std::string, std::uint64_t>> last_ct_;
  std::optional<ClockReading> last_clock_;
  WallQuality last_quality_ = WallQuality::unknown;
  bool backoff_ = false;
};

}  // namespace hm
