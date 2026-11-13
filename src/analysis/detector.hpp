// Detector: watches committed events and opens/closes incidents (FR3).
//
// Pure and deterministic: time comes only from the events themselves, so the
// same event stream always yields the same incidents. That makes the agent's
// live behaviour and `hm ingest` replays of lab fixtures identical.
//
// One host, one incident at a time. While an incident is open, any further
// symptom extends it. It closes after `quiet` with no symptom. An incident
// open when the machine rebooted stays open into the next boot; the quiet
// timer restarts at the first event of the new boot, so an incident can span
// boots (the case the product exists for).
#pragma once
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "store/store.hpp"

namespace hm {

struct DetectorConfig {
  int probe_fail_streak = 3;
  std::chrono::milliseconds quiet{120000};
  int restart_burst = 3;
  std::chrono::milliseconds restart_window{300000};
  double retrans_floor_ppm = 20000;  // 2 %
  double retrans_baseline_factor = 3.0;
  std::int64_t retrans_min_segs = 50;
  double psi_cpu_some10 = 80.0;
  double psi_mem_full10 = 10.0;
};

class Detector {
 public:
  struct Action {
    enum class Type { open, close } type;
    Incident incident;  // for open: fields to persist; for close: id + closed + status
  };

  explicit Detector(DetectorConfig cfg) : cfg_(cfg) {}

  // Re-attach to an incident left open by a previous agent run.
  void adopt_open(const Incident& inc);
  std::optional<std::int64_t> open_incident_id() const { return open_ ? std::optional(open_->id) : std::nullopt; }
  // Called after the store assigns an id to an incident this detector opened.
  void set_open_id(std::int64_t id) {
    if (open_) open_->id = id;
  }

  std::vector<Action> observe(const StoredEvent& e);

  // Exposed for tests: why does this event count as a symptom, if it does?
  std::optional<std::string> symptom(const StoredEvent& e);

 private:
  struct Open {
    std::int64_t id = 0;
    TimePoint last_symptom;
  };
  DetectorConfig cfg_;
  std::optional<Open> open_;
  std::map<std::string, int> fail_streak_;
  struct StreakStart {
    TimePoint at;
    std::int64_t event_id = 0;
    std::int64_t wall_ns = 0;
  };
  std::map<std::string, StreakStart> streak_start_;
  std::map<std::string, std::deque<TimePoint>> restarts_;
  double retrans_baseline_ppm_ = -1;
};

}  // namespace hm
