#include "agent/writer.hpp"

#include <algorithm>

#include "core/log.hpp"

namespace hm {

using SteadyClock = std::chrono::steady_clock;

void Writer::add_drop_events() {
  for (const auto& d : q_.take_drops()) {
    pending_.push_back(self_.make(kind::pipeline_drop, Severity::warning, d.source,
                                  {{"origin", d.source}, {"lane", std::string(d.high_lane ? "high" : "low")},
                                   {"count", static_cast<std::int64_t>(d.count)}, {"reason", std::string("queue full")}}));
  }
  if (writer_shed_unreported_ > 0) {
    pending_.push_back(self_.make(kind::pipeline_drop, Severity::warning, "writer",
                                  {{"origin", std::string("writer")}, {"lane", std::string("low")},
                                   {"count", static_cast<std::int64_t>(writer_shed_unreported_)},
                                   {"reason", std::string("store unavailable")}}));
    writer_shed_unreported_ = 0;
  }
}

// While the store refuses writes (disk full), pending grows. Shed the oldest
// low-priority events first, and count what was shed.
void Writer::shed_if_needed() {
  if (pending_.size() <= cfg_.max_pending) return;
  const std::size_t excess = pending_.size() - cfg_.max_pending;
  std::size_t removed = 0;
  auto it = std::remove_if(pending_.begin(), pending_.end(), [&](const Event& e) {
    if (removed >= excess || is_high_priority(e)) return false;
    ++removed;
    return true;
  });
  pending_.erase(it, pending_.end());
  if (pending_.size() > cfg_.max_pending) {  // all high priority: drop oldest anyway
    const std::size_t more = pending_.size() - cfg_.max_pending;
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(more));
    removed += more;
  }
  stats_.shed += removed;
  writer_shed_unreported_ += removed;
}

bool Writer::commit() {
  add_drop_events();
  if (pending_.empty()) return true;
  auto r = store_.insert_batch(pending_);
  if (!r) {
    ++stats_.commit_failures;
    if (!degraded_) {
      log::error("writer", "commit failed: " + r.error().code + ": " + r.error().message);
      degraded_ = true;
    }
    if (r.error().code == "sqlite_full") {
      // Free space by retention at 90 % of the cap, then retry next cycle.
      auto freed = store_.apply_retention(0, cfg_.retention_days, cfg_.max_bytes * 9 / 10);
      if (freed && *freed > 0) log::warn("writer", "disk full: retention freed " + std::to_string(*freed) + " events");
    }
    shed_if_needed();
    return false;
  }
  if (degraded_) {
    log::info("writer", "store writable again");
    degraded_ = false;
  }
  ++stats_.commits;
  stats_.committed += r->inserted;
  stats_.duplicates += r->duplicates;

  // Detector sees committed events in (mono, source, seq) order.
  std::vector<const Event*> fresh;
  for (const auto& e : pending_) {
    if (e.id != 0) fresh.push_back(&e);
  }
  std::sort(fresh.begin(), fresh.end(), [](const Event* a, const Event* b) {
    if (a->mono_ns != b->mono_ns) return a->mono_ns < b->mono_ns;
    return a->id < b->id;
  });
  for (const Event* e : fresh) {
    StoredEvent se{*e, boot_ordinal_};
    for (auto& action : det_.observe(se)) {
      if (action.type == Detector::Action::Type::open) {
        if (auto id = store_.open_incident(action.incident)) {
          det_.set_open_id(*id);
          ++stats_.incidents_opened;
          log::info("detector", "incident " + std::to_string(*id) + " opened: " + action.incident.trigger + " " +
                                    action.incident.subject);
        } else {
          log::error("detector", "cannot persist incident: " + id.error().message);
        }
      } else {
        if (auto s = store_.close_incident(action.incident.id, action.incident.closed, action.incident.status); !s) {
          log::error("detector", "cannot close incident: " + s.error().message);
        }
      }
    }
  }
  pending_.clear();
  return true;
}

void Writer::run() {
  auto last_commit = SteadyClock::now() - cfg_.min_commit_spacing;
  auto next_commit = SteadyClock::now() + cfg_.commit_interval;
  auto next_retention = SteadyClock::now() + cfg_.retention_every;
  std::vector<Event> batch;
  bool open = true;
  bool urgent = false;
  while (open) {
    batch.clear();
    // Wake for the periodic deadline, or sooner when a spaced commit is due.
    const auto wake = urgent || pending_.size() >= cfg_.commit_batch
                          ? std::min(next_commit, last_commit + cfg_.min_commit_spacing)
                          : next_commit;
    open = q_.pop_batch(batch, cfg_.pop_chunk, wake);
    urgent = urgent || std::any_of(batch.begin(), batch.end(), [](const Event& e) { return is_high_priority(e); });
    for (auto& e : batch) pending_.push_back(std::move(e));
    const auto now = SteadyClock::now();
    const bool spaced = now - last_commit >= cfg_.min_commit_spacing;
    const bool due = now >= next_commit || !open || (spaced && (urgent || pending_.size() >= cfg_.commit_batch));
    if (due) {
      commit();
      last_commit = SteadyClock::now();
      next_commit = last_commit + cfg_.commit_interval;
      urgent = false;
    }
    if (now >= next_retention) {
      const auto wall = read_clocks().realtime_ns;
      if (auto n = store_.apply_retention(wall, cfg_.retention_days, cfg_.max_bytes); n && *n > 0) {
        log::info("writer", "retention removed " + std::to_string(*n) + " events");
      }
      next_retention = now + cfg_.retention_every;
    }
  }
  // Final drain: retry a few times so a clean shutdown commits everything it can.
  for (int i = 0; i < 3 && !commit(); ++i) {
  }
}

}  // namespace hm
