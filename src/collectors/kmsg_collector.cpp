#include "collectors/kmsg_collector.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>

#include "collectors/kmsg_parse.hpp"
#include "core/unique_fd.hpp"

namespace hm {

void KmsgCollector::run(std::stop_token stop) {
  UniqueFd fd(::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (!fd.valid()) {
    const int err = errno;
    std::string why = "open " + path_ + ": " + errno_name(err);
    if (err == EPERM || err == EACCES) why += " (needs CAP_SYSLOG, or kernel.dmesg_restrict=0)";  // CAP_SYS_ADMIN also passes the kernel check
    em_.status("kmsg", "unavailable", why);
    return;
  }
  em_.status("kmsg", "active");
  std::array<char, 8192> buf;
  std::uint64_t expect_seq = 0;
  while (!stop.stop_requested()) {
    const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
    if (n < 0) {
      if (errno == EAGAIN) {
        pollfd p{fd.get(), POLLIN, 0};
        ::poll(&p, 1, 500);
        continue;
      }
      if (errno == EPIPE) {
        // Records were overwritten in the ring before we read them. The next
        // read resumes at the oldest available record; the seq gap is logged below.
        continue;
      }
      if (errno == EINTR) continue;
      em_.status("kmsg", "degraded", "read: " + errno_name(errno));
      return;
    }
    auto rec = kmsg::parse_record(std::string_view(buf.data(), static_cast<std::size_t>(n)));
    if (!rec) continue;
    if (expect_seq != 0 && rec->seq > expect_seq) {
      em_.emit(kind::pipeline_drop, Severity::warning, "kmsg",
               {{"origin", std::string("kmsg")}, {"reason", std::string("ring overwritten")},
                {"count", static_cast<std::int64_t>(rec->seq - expect_seq)}});
    }
    expect_seq = rec->seq + 1;
    auto c = kmsg::classify(*rec);
    if (!c) continue;
    // printk timestamps are CLOCK_MONOTONIC-based: equal to CLOCK_BOOTTIME
    // unless the machine has suspended. Wall time is back-computed.
    const ClockReading now = read_clocks();
    ClockReading at = now;
    at.boottime_ns = static_cast<std::int64_t>(rec->usec) * 1000;
    at.realtime_ns = now.realtime_ns - (now.boottime_ns - at.boottime_ns);
    c->attrs["ts_source"] = std::string("printk");
    rec_.push(rec_.make_at(c->kind, c->severity, c->subject, std::move(c->attrs), at, rec->seq));
  }
}

}  // namespace hm
