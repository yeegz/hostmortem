// Clock and boot identity. Linux implementation in clock_linux.cpp.
#pragma once
#include <cstdint>
#include <string>

#include "core/event.hpp"
#include "core/result.hpp"

namespace hm {

struct ClockReading {
  std::int64_t boottime_ns = 0;  // CLOCK_BOOTTIME: monotonic, counts suspend
  std::int64_t realtime_ns = 0;  // CLOCK_REALTIME: wall clock, may step
  WallQuality quality = WallQuality::unknown;
  std::int64_t max_error_us = -1;  // from adjtimex, -1 if unknown
};

ClockReading read_clocks() noexcept;

// /proc/sys/kernel/random/boot_id: a UUID the kernel regenerates every boot.
Result<std::string> read_boot_id();

// uname release, recorded per boot for the compatibility matrix.
std::string kernel_release();

}  // namespace hm
