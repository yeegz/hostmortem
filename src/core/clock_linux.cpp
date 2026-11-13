#include <sys/timex.h>
#include <sys/utsname.h>
#include <time.h>

#include <cerrno>
#include <cstring>
#include <fstream>

#include "core/clock.hpp"

namespace hm {

namespace {
std::int64_t to_ns(const timespec& ts) noexcept {
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}
}  // namespace

ClockReading read_clocks() noexcept {
  ClockReading r;
  timespec ts{};
  if (::clock_gettime(CLOCK_BOOTTIME, &ts) == 0) r.boottime_ns = to_ns(ts);
  if (::clock_gettime(CLOCK_REALTIME, &ts) == 0) r.realtime_ns = to_ns(ts);
  // adjtimex with modes = 0 is a read-only query: no CAP_SYS_TIME needed.
  timex tx{};
  const int state = ::adjtimex(&tx);
  if (state == -1) {
    r.quality = WallQuality::unknown;
  } else if (state == TIME_ERROR || (tx.status & STA_UNSYNC) != 0) {
    r.quality = WallQuality::unsynced;
    r.max_error_us = tx.maxerror;
  } else {
    r.quality = WallQuality::synced;
    r.max_error_us = tx.maxerror;
  }
  return r;
}

Result<std::string> read_boot_id() {
  std::ifstream in("/proc/sys/kernel/random/boot_id");
  std::string id;
  if (!in || !std::getline(in, id) || id.size() < 32) {
    return make_error("boot_id_unavailable", "cannot read /proc/sys/kernel/random/boot_id");
  }
  return id;
}

std::string kernel_release() {
  utsname u{};
  if (::uname(&u) != 0) return "unknown";
  return u.release;
}

}  // namespace hm
