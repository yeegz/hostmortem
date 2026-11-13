#pragma once
#include <chrono>
#include <cstdint>

#include "analysis/detector.hpp"
#include "collectors/collector.hpp"
#include "pipeline/event_queue.hpp"
#include "store/store.hpp"

namespace hm {

struct WriterConfig {
  std::size_t commit_batch = 500;
  std::chrono::milliseconds commit_interval{5000};
  std::chrono::milliseconds retention_every{60000};
  int retention_days = 14;
  std::int64_t max_bytes = 256LL << 20;
  std::size_t max_pending = 20000;  // bound on events held while the store refuses writes
  // Every commit is fsynced (synchronous=FULL), so under load batches grow
  // instead of committing every 500 events: at most one commit per
  // min_commit_spacing, each covering everything queued. 200 ms is far below
  // the 5 s loss bound.
  std::chrono::milliseconds min_commit_spacing{200};
  std::size_t pop_chunk = 4096;
};

// The single consumer. Commit policy: every `commit_batch` events or every
// `commit_interval`, whichever first; any warning-or-worse event commits
// as soon as min_commit_spacing allows (incident evidence should not wait 5 s
// to become durable, but a storm of warnings must not turn into a storm of fsyncs).
// After each successful commit the batch is fed to the detector.
class Writer {
 public:
  Writer(Store& store, EventQueue& q, Detector& det, Emitter& self, std::int64_t boot_ordinal, WriterConfig cfg)
      : store_(store), q_(q), det_(det), self_(self), boot_ordinal_(boot_ordinal), cfg_(cfg) {}

  // Runs until the queue is closed and drained, then performs a final commit.
  void run();

  struct Stats {
    std::uint64_t committed = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t commits = 0;
    std::uint64_t commit_failures = 0;
    std::uint64_t shed = 0;
    std::uint64_t incidents_opened = 0;
  };
  Stats stats() const { return stats_; }

  // One commit attempt (public for tests). Returns true when pending is empty afterwards.
  bool commit();
  std::vector<Event>& pending() { return pending_; }

 private:
  void add_drop_events();
  void shed_if_needed();

  Store& store_;
  EventQueue& q_;
  Detector& det_;
  Emitter& self_;
  std::int64_t boot_ordinal_;
  WriterConfig cfg_;
  std::vector<Event> pending_;
  Stats stats_;
  bool degraded_ = false;
  std::uint64_t writer_shed_unreported_ = 0;
};

}  // namespace hm
