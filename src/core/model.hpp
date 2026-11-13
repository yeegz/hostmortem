// Analysis records persisted by the store: incidents, hypotheses, evidence.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hm {

// A point on the cross-boot timeline. Within a boot, CLOCK_BOOTTIME orders
// events; across boots, the boot ordinal does. Wall time never orders.
struct TimePoint {
  std::int64_t boot_ordinal = 0;
  std::string boot_id;
  std::int64_t mono_ns = 0;

  friend bool operator<(const TimePoint& a, const TimePoint& b) {
    if (a.boot_ordinal != b.boot_ordinal) return a.boot_ordinal < b.boot_ordinal;
    return a.mono_ns < b.mono_ns;
  }
};

enum class IncidentStatus { open, closed, interrupted };

struct Incident {
  std::int64_t id = 0;
  TimePoint opened;
  std::int64_t opened_wall_ns = 0;
  TimePoint closed;  // boot_ordinal == 0 while open
  std::string trigger;  // e.g. "probe_fail_streak", "link_down"
  std::string subject;  // e.g. "dns:resolver", "eth0"
  std::int64_t trigger_event_id = 0;
  IncidentStatus status = IncidentStatus::open;
};

enum class Verdict { supported, possible, ruled_out };
enum class EvidenceRole { for_, against, missing };

struct Evidence {
  std::int64_t event_id = 0;  // 0 for "missing" evidence: there is no event
  EvidenceRole role = EvidenceRole::for_;
  std::string note;
};

struct Hypothesis {
  std::string playbook;
  std::string title;
  Verdict verdict = Verdict::possible;
  int score = 0;
  std::string explanation;
  std::vector<Evidence> evidence;
  std::vector<std::string> next_steps;
};

const char* to_string(IncidentStatus s) noexcept;
const char* to_string(Verdict v) noexcept;
const char* to_string(EvidenceRole r) noexcept;

}  // namespace hm
