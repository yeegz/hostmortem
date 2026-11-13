// /dev/kmsg record parsing and classification of the kernel messages the
// playbooks care about.
// Record format (Documentation/ABI/testing/dev-kmsg):
//   "<prefix>,<seq>,<usec>,<flags>[,...];<message>\n[ KEY=value continuation lines ]"
// prefix = facility * 8 + level.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/event.hpp"

namespace hm::kmsg {

struct Record {
  int level = 6;
  int facility = 0;
  std::uint64_t seq = 0;
  std::uint64_t usec = 0;  // CLOCK_BOOTTIME-ish timestamp from the kernel (monotonic since boot)
  std::string message;
};

std::optional<Record> parse_record(std::string_view raw);

struct Classified {
  std::string kind;
  std::string subject;
  Severity severity = Severity::warning;
  Attrs attrs;
};

// Returns the event to emit for this record, or nullopt to ignore it.
// Kernel error-level and worse lines are kept as kernel.log; the specific
// patterns become typed events.
std::optional<Classified> classify(const Record& r);

}  // namespace hm::kmsg
