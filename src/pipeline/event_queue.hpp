// Bounded multi-producer / single-consumer event queue.
//
// Design (ADR-0003):
//  * Two lanes. Severity >= warning goes to the "high" lane, which has its own
//    capacity, so a flood of low-value samples can never push out the kernel
//    OOM line or the probe failure that an incident depends on.
//  * Never blocks a producer. When a lane is full the incoming event is
//    dropped and counted per (source, lane). The writer turns those counts
//    into pipeline.drop events, so loss is itself recorded evidence (FR5).
//  * At >= 80% of a lane, push() reports pressure so samplers can back off.
//  * mutex + condition_variable. A lock-free ring is a research experiment
//    only if profiling shows this lock is contended (it is not at the
//    20k events/s target: one lock per event is ~tens of ns uncontended).
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/event.hpp"

namespace hm {

class EventQueue {
 public:
  enum class Push { accepted, pressure, dropped, closed };

  struct DropCount {
    std::string source;
    bool high_lane = false;
    std::uint64_t count = 0;
  };

  struct Stats {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::size_t depth = 0;
    std::size_t high_water = 0;
  };

  EventQueue(std::size_t low_capacity, std::size_t high_capacity)
      : low_cap_(low_capacity), high_cap_(high_capacity) {}

  EventQueue(const EventQueue&) = delete;
  EventQueue& operator=(const EventQueue&) = delete;

  Push push(Event&& e) {
    const bool high = is_high_priority(e);
    Push result = Push::accepted;
    {
      std::lock_guard lock(mu_);
      if (closed_) return Push::closed;
      auto& lane = high ? high_ : low_;
      const std::size_t cap = high ? high_cap_ : low_cap_;
      if (lane.size() >= cap) {
        ++drops_[{e.source, high}];
        ++stats_.dropped;
        return Push::dropped;
      }
      lane.push_back(std::move(e));
      ++stats_.accepted;
      const std::size_t depth = low_.size() + high_.size();
      if (depth > stats_.high_water) stats_.high_water = depth;
      if (lane.size() * 5 >= cap * 4) result = Push::pressure;
    }
    // Notify after unlocking so the woken consumer does not immediately block
    // on the mutex we still hold. The predicate wait below makes this safe
    // against lost wake-ups: state is changed under the lock before notify.
    cv_.notify_one();
    return result;
  }

  // Moves up to `max` events into `out`, high lane first. Blocks until at least
  // one event is available, `deadline` passes, or the queue is closed.
  // Returns false only when the queue is closed AND fully drained.
  bool pop_batch(std::vector<Event>& out, std::size_t max,
                 std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mu_);
    cv_.wait_until(lock, deadline, [&] { return closed_ || !high_.empty() || !low_.empty(); });
    std::size_t n = 0;
    while (n < max && !high_.empty()) {
      out.push_back(std::move(high_.front()));
      high_.pop_front();
      ++n;
    }
    while (n < max && !low_.empty()) {
      out.push_back(std::move(low_.front()));
      low_.pop_front();
      ++n;
    }
    return !(closed_ && high_.empty() && low_.empty() && n == 0);
  }

  // After close(), push() rejects and pop_batch() drains what remains.
  void close() {
    {
      std::lock_guard lock(mu_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  std::vector<DropCount> take_drops() {
    std::lock_guard lock(mu_);
    std::vector<DropCount> out;
    out.reserve(drops_.size());
    for (auto& [key, count] : drops_) out.push_back({key.first, key.second, count});
    drops_.clear();
    return out;
  }

  Stats stats() const {
    std::lock_guard lock(mu_);
    Stats s = stats_;
    s.depth = low_.size() + high_.size();
    return s;
  }

 private:
  const std::size_t low_cap_;
  const std::size_t high_cap_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Event> low_;
  std::deque<Event> high_;
  bool closed_ = false;
  std::map<std::pair<std::string, bool>, std::uint64_t> drops_;
  Stats stats_;
};

}  // namespace hm
