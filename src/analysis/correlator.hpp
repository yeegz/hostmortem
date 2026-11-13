// Correlator: turns an incident window into ranked, evidence-linked
// hypotheses (FR4). Deterministic rules, no ML, no LLM (ADR-0006).
//
// Scores are integer evidence points mapped to verdicts by fixed thresholds.
// They are NOT probabilities and are never displayed as percentages.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "store/store.hpp"

namespace hm {

enum class CollectorState { active, degraded, unavailable, unknown };

struct Availability {
  struct Entry {
    CollectorState state = CollectorState::unknown;
    std::string reason;
  };
  std::map<std::string, Entry> collectors;  // "kmsg", "prober.icmp", "procfs.psi", ...

  CollectorState state(const std::string& name) const;
  std::string reason(const std::string& name) const;
  bool can_see(const std::string& name) const { return state(name) == CollectorState::active; }
};

// Builds availability from collector.status events (latest per collector wins).
Availability availability_from(const std::vector<StoredEvent>& status_events);

struct Window {
  const Incident& incident;
  const std::vector<StoredEvent>& events;    // [start, end], ordered
  const std::vector<StoredEvent>& baseline;  // the 10 minutes before start
  const Availability& avail;
};

struct Unobservable {
  std::string what;    // "OOM killer activity"
  std::string reason;  // "kmsg collector unavailable: needs CAP_SYSLOG"
};

struct Diagnosis {
  Incident incident;
  TimePoint window_start;
  TimePoint window_end;
  bool spans_boots = false;
  std::vector<std::int64_t> facts;  // ids of the key observed events, in time order
  std::vector<Hypothesis> hypotheses;  // supported, possible, ruled_out; by score
  std::vector<Unobservable> unobservable;
  // "supported" when one hypothesis clearly leads; otherwise "unknown".
  std::string conclusion;
  std::string conclusion_reason;
  std::int64_t leading_index = -1;
  std::vector<StoredEvent> events;  // the window itself, for rendering
};

using Playbook = void (*)(const Window&, std::vector<Hypothesis>&, std::vector<Unobservable>&);

const std::vector<std::pair<std::string, Playbook>>& playbooks();

Diagnosis diagnose(const Incident& inc, const std::vector<StoredEvent>& events,
                   const std::vector<StoredEvent>& baseline, const Availability& avail,
                   TimePoint window_start, TimePoint window_end);

// Fetches the window from the store and diagnoses it. Lookback before the
// incident opens is 60 s; baseline is the 10 minutes before that.
Result<Diagnosis> diagnose_incident(Store& store, std::int64_t incident_id);

// Every supported/possible hypothesis must cite at least one stored event.
// Returns the first violation, for tests and as a release gate.
std::optional<std::string> validate_citations(const Diagnosis& d);

}  // namespace hm
