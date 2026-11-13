#include "analysis/correlator.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "core/kinds.hpp"

namespace hm {

namespace {
constexpr std::int64_t kSec = 1'000'000'000LL;
constexpr std::int64_t kLookback = 60 * kSec;
constexpr std::int64_t kBaseline = 600 * kSec;

CollectorState parse_state(const std::string& s) {
  if (s == "active") return CollectorState::active;
  if (s == "degraded") return CollectorState::degraded;
  if (s == "unavailable") return CollectorState::unavailable;
  return CollectorState::unknown;
}

int verdict_rank(Verdict v) {
  switch (v) {
    case Verdict::supported: return 0;
    case Verdict::possible: return 1;
    case Verdict::ruled_out: return 2;
  }
  return 3;
}

bool is_fact(const Event& e) {
  if (e.kind.rfind("probe.", 0) == 0) return !e.attr_bool("ok").value_or(true);
  return e.severity >= Severity::warning || e.kind == kind::link_state || e.kind == kind::route_del ||
         e.kind == kind::clock_step || e.kind.rfind("process.", 0) == 0 || e.kind == kind::agent_start;
}
}  // namespace

CollectorState Availability::state(const std::string& name) const {
  auto it = collectors.find(name);
  if (it != collectors.end()) return it->second.state;
  // "prober.icmp" falls back to "prober" when no finer status was reported.
  const auto dot = name.find('.');
  if (dot != std::string::npos) {
    it = collectors.find(name.substr(0, dot));
    if (it != collectors.end()) return it->second.state;
  }
  return CollectorState::unknown;
}

std::string Availability::reason(const std::string& name) const {
  auto it = collectors.find(name);
  if (it == collectors.end()) {
    const auto dot = name.find('.');
    if (dot != std::string::npos) it = collectors.find(name.substr(0, dot));
  }
  if (it == collectors.end()) return name + " status never reported";
  if (it->second.reason.empty()) return name + " " + (it->second.state == CollectorState::active ? "active" : "not active");
  return name + ": " + it->second.reason;
}

Availability availability_from(const std::vector<StoredEvent>& status_events) {
  Availability a;
  for (const auto& se : status_events) {
    if (se.event.kind != kind::collector_status) continue;
    auto& entry = a.collectors[se.event.subject];
    entry.state = parse_state(se.event.attr_str("state").value_or(""));
    entry.reason = se.event.attr_str("reason").value_or("");
  }
  return a;
}

Diagnosis diagnose(const Incident& inc, const std::vector<StoredEvent>& events,
                   const std::vector<StoredEvent>& baseline, const Availability& avail,
                   TimePoint window_start, TimePoint window_end) {
  Diagnosis d;
  d.incident = inc;
  d.window_start = window_start;
  d.window_end = window_end;
  d.spans_boots = window_end.boot_ordinal != window_start.boot_ordinal;
  d.events = events;

  for (const auto& se : events) {
    if (is_fact(se.event)) d.facts.push_back(se.event.id);
  }

  const Window w{inc, events, baseline, avail};
  for (const auto& [name, fn] : playbooks()) fn(w, d.hypotheses, d.unobservable);

  std::stable_sort(d.hypotheses.begin(), d.hypotheses.end(), [](const Hypothesis& a, const Hypothesis& b) {
    if (verdict_rank(a.verdict) != verdict_rank(b.verdict)) return verdict_rank(a.verdict) < verdict_rank(b.verdict);
    return a.score > b.score;
  });

  // De-duplicate unobservable items (several playbooks may report the same gap).
  std::set<std::string> seen;
  std::vector<Unobservable> uniq;
  for (auto& u : d.unobservable) {
    if (seen.insert(u.what).second) uniq.push_back(std::move(u));
  }
  d.unobservable = std::move(uniq);

  // When to say "unknown" (blueprint 12.6): nothing reaches supported, or two
  // supported hypotheses are within one point of each other.
  std::vector<std::size_t> supported;
  for (std::size_t i = 0; i < d.hypotheses.size(); ++i) {
    if (d.hypotheses[i].verdict == Verdict::supported) supported.push_back(i);
  }
  if (supported.empty()) {
    d.conclusion = "unknown";
    d.conclusion_reason = "No hypothesis reached the supported threshold.";
    if (!d.unobservable.empty()) d.conclusion_reason += " Some layers were not observable (listed below).";
  } else if (supported.size() >= 2 &&
             d.hypotheses[supported[0]].score - d.hypotheses[supported[1]].score <= 1) {
    d.conclusion = "unknown";
    d.conclusion_reason = "Competing explanations are equally supported: " + d.hypotheses[supported[0]].title +
                          " and " + d.hypotheses[supported[1]].title + ".";
  } else {
    d.conclusion = "supported";
    d.leading_index = static_cast<std::int64_t>(supported[0]);
    d.conclusion_reason = "Leading explanation: " + d.hypotheses[supported[0]].title + ".";
  }
  return d;
}

Result<Diagnosis> diagnose_incident(Store& store, std::int64_t incident_id) {
  auto inc = store.incident(incident_id);
  if (!inc) return inc.error();
  if (!*inc) return make_error("no_incident", "no incident with id " + std::to_string(incident_id));
  const Incident& i = **inc;

  TimePoint start = i.opened;
  start.mono_ns = std::max<std::int64_t>(0, start.mono_ns - kLookback);
  TimePoint end;
  if (i.closed.boot_ordinal != 0) {
    end = i.closed;
  } else {
    end = {std::numeric_limits<std::int64_t>::max(), "", std::numeric_limits<std::int64_t>::max()};
  }

  EventQuery q;
  q.from = start;
  q.to = end;
  auto events = store.query(q);
  if (!events) return events.error();

  EventQuery bq;
  bq.from = TimePoint{start.boot_ordinal, start.boot_id, std::max<std::int64_t>(0, start.mono_ns - kBaseline)};
  bq.to = TimePoint{start.boot_ordinal, start.boot_id, start.mono_ns - 1};
  auto baseline = store.query(bq);
  if (!baseline) return baseline.error();

  // Collector status from the beginning of the opening boot to the window end.
  EventQuery sq;
  sq.from = TimePoint{start.boot_ordinal, start.boot_id, 0};
  sq.to = end;
  sq.kind_prefix = kind::collector_status;
  auto status = store.query(sq);
  if (!status) return status.error();

  if (end.boot_ordinal == std::numeric_limits<std::int64_t>::max() && !events->empty()) {
    const auto& last = events->back();
    end = {last.boot_ordinal, last.event.boot_id, last.event.mono_ns};
  }
  return diagnose(i, *events, *baseline, availability_from(*status), start, end);
}

std::optional<std::string> validate_citations(const Diagnosis& d) {
  for (const auto& h : d.hypotheses) {
    if (h.verdict == Verdict::ruled_out) continue;
    const bool cites = std::any_of(h.evidence.begin(), h.evidence.end(), [](const Evidence& e) {
      return e.role == EvidenceRole::for_ && e.event_id != 0;
    });
    if (!cites) return h.playbook + " has no cited supporting event";
    if (h.explanation.find('#') == std::string::npos) return h.playbook + " explanation cites no event id";
  }
  return std::nullopt;
}

}  // namespace hm
