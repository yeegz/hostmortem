// Collector framework.
//
// Each collector owns one std::jthread. It talks to the rest of the agent
// only through its Emitter (which stamps clocks and per-source sequence
// numbers and pushes into the bounded queue). Collectors never touch the
// store, never throw across the thread boundary, and report their own health
// as collector.status events (FR6) rather than failing silently.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

#include "core/clock.hpp"
#include "core/kinds.hpp"
#include "pipeline/event_queue.hpp"

namespace hm {

class Emitter {
 public:
  Emitter(EventQueue& q, std::string source, std::string boot_id, std::uint64_t last_seq)
      : q_(q), source_(std::move(source)), boot_id_(std::move(boot_id)), seq_(last_seq) {}

  // Builds a stamped event without queueing it (the writer uses this for
  // its own pipeline.drop records, which must not depend on queue space).
  Event make(std::string kind, Severity sev, std::string subject, Attrs attrs) {
    const ClockReading c = read_clocks();
    return make_at(std::move(kind), sev, std::move(subject), std::move(attrs), c, ++seq_);
  }

  Event make_at(std::string kind, Severity sev, std::string subject, Attrs attrs, const ClockReading& c,
                std::uint64_t seq) const {
    Event e;
    e.boot_id = boot_id_;
    e.source = source_;
    e.seq = seq;
    e.mono_ns = c.boottime_ns;
    e.wall_ns = c.realtime_ns;
    e.wall_quality = c.quality;
    e.kind = std::move(kind);
    e.severity = sev;
    e.subject = std::move(subject);
    e.attrs = std::move(attrs);
    return e;
  }

  EventQueue::Push emit(std::string kind, Severity sev, std::string subject = {}, Attrs attrs = {}) {
    return q_.push(make(std::move(kind), sev, std::move(subject), std::move(attrs)));
  }

  EventQueue::Push push(Event&& e) { return q_.push(std::move(e)); }

  // FR6: state is "active", "degraded" or "unavailable", always with a reason
  // unless active.
  void status(const std::string& collector, std::string_view state, std::string reason = {}) {
    Attrs a{{"state", std::string(state)}};
    if (!reason.empty()) a["reason"] = std::move(reason);
    emit(kind::collector_status, state == "active" ? Severity::info : Severity::notice, collector, std::move(a));
  }

  const std::string& source() const { return source_; }
  const std::string& boot_id() const { return boot_id_; }

 private:
  EventQueue& q_;
  std::string source_;
  std::string boot_id_;
  std::atomic<std::uint64_t> seq_;
};

class Collector {
 public:
  virtual ~Collector() = default;
  virtual std::string_view name() const = 0;
  // Runs until stop is requested. Must return within ~1 s of a stop request.
  virtual void run(std::stop_token stop) = 0;
};

// Interruptible sleep. Returns false if a stop was requested.
inline bool sleep_for(std::stop_token stop, std::chrono::milliseconds d) {
  std::mutex m;
  std::condition_variable_any cv;
  std::unique_lock lock(m);
  return !cv.wait_for(lock, stop, d, [] { return false; }) && !stop.stop_requested();
}

std::string errno_name(int err);

}  // namespace hm
