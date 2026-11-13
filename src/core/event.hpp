// The Event value type: one observed fact, immutable once emitted.
//
// FR1: every event carries boot_id, a per-source sequence number,
// CLOCK_BOOTTIME (mono_ns, used for ordering inside a boot) and
// CLOCK_REALTIME (wall_ns, display only) plus a wall-clock quality flag.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace hm {

inline constexpr int kSchemaVersion = 1;

enum class Severity : int { debug = 0, info = 1, notice = 2, warning = 3, error = 4, critical = 5 };

// How far CLOCK_REALTIME can be trusted at the moment of capture.
enum class WallQuality : int {
  unknown = 0,  // could not query the kernel (adjtimex failed)
  unsynced = 1, // kernel reports STA_UNSYNC: wall time may be years off
  synced = 2,   // NTP/PTP discipline active
  stepped = 3,  // a step was observed near this sample
};

using AttrValue = std::variant<std::int64_t, double, bool, std::string>;
using Attrs = std::map<std::string, AttrValue, std::less<>>;

struct Event {
  std::int64_t id = 0;  // store rowid; 0 until persisted
  std::string boot_id;
  std::string source;
  std::uint64_t seq = 0;
  std::int64_t mono_ns = 0;
  std::int64_t wall_ns = 0;
  WallQuality wall_quality = WallQuality::unknown;
  std::string kind;
  Severity severity = Severity::info;
  std::string subject;
  Attrs attrs;

  // Typed attribute accessors. Return nullopt on absence or type mismatch;
  // ints widen to double for numeric reads.
  std::optional<std::int64_t> attr_int(std::string_view key) const;
  std::optional<double> attr_num(std::string_view key) const;
  std::optional<bool> attr_bool(std::string_view key) const;
  std::optional<std::string> attr_str(std::string_view key) const;
};

std::string_view to_string(Severity s) noexcept;
std::optional<Severity> severity_from_string(std::string_view s) noexcept;
std::string_view to_string(WallQuality q) noexcept;

// High-priority events get a reserved lane in the pipeline queue so a flood
// of samples cannot starve the evidence an incident depends on.
inline bool is_high_priority(const Event& e) noexcept { return e.severity >= Severity::warning; }

}  // namespace hm
