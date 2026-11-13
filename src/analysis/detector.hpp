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
  // CPU starvation: PSI "some" avg10 at or above this for psi_streak samples
  // in a row. Matches the resource_pressure playbook's 50 % (a real 12-on-4
  // saturation in the lab peaked at 77.7 %, under the old single-sample 80 %).
  double psi_cpu_some10 = 50.0;
  int psi_streak = 3;
  double psi_mem_full10 = 10.0;
  // ICMP path quality, per target: partial loss over the last `loss_window`
  // probes, or `latency_streak` consecutive RTTs far above a learned baseline.
  int loss_window = 10;
  int loss_failures = 3;
  int latency_streak = 3;
  int latency_min_baseline_samples = 5;
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
  int cpu_psi_streak_ = 0;
  struct PathState {
    std::deque<bool> recent;  // true = reply received
    double base_rtt = -1;
    int base_samples = 0;
    int slow_streak = 0;
  };
  std::map<std::string, PathState> paths_;
  std::optional<std::string> path_symptom(const Event& e);
};

}  // namespace hm
