// Rendering a Diagnosis for people (terminal, HTML) and machines (JSON).
// HTML reports are self-contained files: no scripts, no remote resources, a
// strict CSP, and every string from the store is escaped (log-injection and
// XSS defence, threat model T6).
#pragma once
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "analysis/correlator.hpp"

namespace hm {

std::string format_event_line(const StoredEvent& se, std::int64_t origin_mono_ns);
std::string render_text(const Diagnosis& d, bool synthetic);
nlohmann::json diagnosis_json(const Diagnosis& d);
std::string render_html(const Diagnosis& d, bool synthetic);
std::string html_escape(std::string_view s);
std::string format_wall(std::int64_t wall_ns, WallQuality q);

// Replaces IPv4 and IPv6/MAC-looking literals with placeholders. Deliberately
// over-eager (a "12:30:45" string is redacted too): redaction means redaction.
std::string redact_ips(std::string_view s);
// A copy of the diagnosis with every stored string passed through redact_ips.
Diagnosis redact_diagnosis(Diagnosis d);

// Everything `hm report` puts in one self-contained HTML file: the host's
// coverage, its boots, and each incident's full diagnosis.
struct HostReport {
  std::int64_t generated_wall_ns = 0;
  std::int64_t event_count = 0;
  std::vector<BootInfo> boots;
  Availability coverage;           // latest boot's collector states
  std::vector<Diagnosis> incidents;  // in the order they are shown
  bool synthetic = false;
  bool redacted = false;
};
std::string render_html_report(const HostReport& r);

}  // namespace hm
