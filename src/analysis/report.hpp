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

}  // namespace hm
