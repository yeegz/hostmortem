#include "analysis/report.hpp"

#include <ctime>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/json_codec.hpp"

namespace hm {

namespace {

std::string fmt_offset(std::int64_t ns) {
  char buf[32];
  const double s = static_cast<double>(ns) / 1e9;
  std::snprintf(buf, sizeof buf, "%+9.1fs", s);
  return buf;
}

std::string attrs_brief(const Attrs& attrs) {
  std::string out;
  for (const auto& [k, v] : attrs) {
    if (!out.empty()) out += ' ';
    out += k + '=';
    std::visit(
        [&](const auto& x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, std::string>) out += x;
          else if constexpr (std::is_same_v<T, bool>) out += x ? "true" : "false";
          else if constexpr (std::is_same_v<T, double>) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", x);
            out += buf;
          } else out += std::to_string(x);
        },
        v);
  }
  return out;
}

// Terminal output: strip control characters from stored strings.
std::string safe(std::string_view s) {
  std::string out;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    out += (u < 0x20 || u == 0x7f) ? '?' : c;
  }
  return out;
}

const char* verdict_label(Verdict v) {
  switch (v) {
    case Verdict::supported: return "SUPPORTED";
    case Verdict::possible: return "POSSIBLE ";
    case Verdict::ruled_out: return "RULED OUT";
  }
  return "?";
}

}  // namespace

std::string format_wall(std::int64_t wall_ns, WallQuality q) {
  const std::time_t t = static_cast<std::time_t>(wall_ns / 1'000'000'000LL);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%SZ", &tm);
  std::string s = buf;
  if (q == WallQuality::unsynced) s += " (clock unsynced)";
  else if (q == WallQuality::stepped) s += " (clock stepped)";
  else if (q == WallQuality::unknown) s += " (clock quality unknown)";
  return s;
}

std::string format_event_line(const StoredEvent& se, std::int64_t origin_mono_ns) {
  const Event& e = se.event;
  std::ostringstream os;
  os << '#' << e.id << ' ' << fmt_offset(e.mono_ns - origin_mono_ns) << "  " << safe(e.kind);
  if (!e.subject.empty()) os << " [" << safe(e.subject) << ']';
  os << "  " << safe(attrs_brief(e.attrs));
  return os.str();
}

std::string render_text(const Diagnosis& d, bool synthetic) {
  std::ostringstream os;
  std::map<std::int64_t, const StoredEvent*> by_id;
  for (const auto& se : d.events) by_id[se.event.id] = &se;

  if (synthetic) os << "!! SYNTHETIC DATA: this store was filled by `hm ingest`, not by a live agent.\n\n";
  os << "Incident " << d.incident.id << "  trigger=" << d.incident.trigger << "  subject=" << safe(d.incident.subject)
     << "  status=" << to_string(d.incident.status) << '\n';
  os << "Opened  " << format_wall(d.incident.opened_wall_ns, WallQuality::synced).substr(0, 20)
     << " (wall clock, display only)\n";
  os << "Window  boot " << d.window_start.boot_ordinal << " -> boot " << d.window_end.boot_ordinal
     << (d.spans_boots ? "  ** spans a reboot **" : "") << "  (" << d.events.size() << " events)\n\n";

  os << "CONCLUSION: " << (d.conclusion == "supported" ? "SUPPORTED" : "UNKNOWN") << " - " << d.conclusion_reason << "\n\n";

  os << "Observed facts (ordered by boot, then CLOCK_BOOTTIME; offsets from incident open):\n";
  std::int64_t current_boot = -1;
  std::size_t shown = 0;
  for (std::int64_t id : d.facts) {
    auto it = by_id.find(id);
    if (it == by_id.end()) continue;
    if (++shown > 40) {
      os << "  ... " << (d.facts.size() - 40) << " more (hm timeline for all)\n";
      break;
    }
    const auto& se = *it->second;
    if (se.boot_ordinal != current_boot) {
      if (current_boot != -1) os << "  ---------------- reboot: boot " << se.boot_ordinal << " ----------------\n";
      current_boot = se.boot_ordinal;
    }
    const std::int64_t origin = se.boot_ordinal == d.incident.opened.boot_ordinal ? d.incident.opened.mono_ns : 0;
    os << "  " << format_event_line(se, origin) << '\n';
  }
  os << '\n';

  os << "Hypotheses (score = evidence points, not a probability):\n";
  for (const auto& h : d.hypotheses) {
    os << "  [" << verdict_label(h.verdict) << "] " << h.title << "  (" << h.playbook << ", score " << h.score << ")\n";
    os << "      " << safe(h.explanation) << '\n';
    for (const auto& e : h.evidence) {
      os << "      " << (e.role == EvidenceRole::for_ ? "+ " : e.role == EvidenceRole::against ? "- " : "? ");
      if (e.event_id) os << '#' << e.event_id << ' ';
      os << safe(e.note) << '\n';
    }
    if (h.verdict != Verdict::ruled_out && !h.next_steps.empty()) {
      os << "      next: " << h.next_steps.front() << '\n';
      for (std::size_t i = 1; i < h.next_steps.size(); ++i) os << "            " << h.next_steps[i] << '\n';
    }
  }
  if (d.hypotheses.empty()) os << "  (no playbook found anything to say about this window)\n";

  if (!d.unobservable.empty()) {
    os << "\nCould not see:\n";
    for (const auto& u : d.unobservable) os << "  - " << u.what << ": " << safe(u.reason) << '\n';
  }
  return os.str();
}

nlohmann::json diagnosis_json(const Diagnosis& d) {
  using nlohmann::json;
  json j;
  j["incident"] = {{"id", d.incident.id},
                   {"trigger", d.incident.trigger},
                   {"subject", d.incident.subject},
                   {"status", to_string(d.incident.status)},
                   {"opened_boot_ordinal", d.incident.opened.boot_ordinal},
                   {"opened_mono_ns", d.incident.opened.mono_ns},
                   {"opened_wall_ns", d.incident.opened_wall_ns}};
  j["window"] = {{"start_boot_ordinal", d.window_start.boot_ordinal},
                 {"start_mono_ns", d.window_start.mono_ns},
                 {"end_boot_ordinal", d.window_end.boot_ordinal},
                 {"end_mono_ns", d.window_end.mono_ns},
                 {"spans_boots", d.spans_boots},
                 {"event_count", d.events.size()}};
  j["conclusion"] = {{"verdict", d.conclusion}, {"reason", d.conclusion_reason}};
  if (d.leading_index >= 0) j["conclusion"]["leading"] = d.hypotheses[static_cast<std::size_t>(d.leading_index)].playbook;
  j["facts"] = d.facts;
  json hs = json::array();
  for (const auto& h : d.hypotheses) {
    json ev = json::array();
    for (const auto& e : h.evidence) {
      json x = {{"role", to_string(e.role)}, {"note", e.note}};
      x["event_id"] = e.event_id ? json(e.event_id) : json(nullptr);
      ev.push_back(std::move(x));
    }
    hs.push_back({{"playbook", h.playbook},
                  {"title", h.title},
                  {"verdict", to_string(h.verdict)},
                  {"score", h.score},
                  {"explanation", h.explanation},
                  {"evidence", std::move(ev)},
                  {"next_steps", h.next_steps}});
  }
  j["hypotheses"] = std::move(hs);
  json un = json::array();
  for (const auto& u : d.unobservable) un.push_back({{"what", u.what}, {"reason", u.reason}});
  j["unobservable"] = std::move(un);
  json evs = json::array();
  for (const auto& se : d.events) {
    json e = event_to_json(se.event);
    e["boot_ordinal"] = se.boot_ordinal;
    evs.push_back(std::move(e));
  }
  j["events"] = std::move(evs);
  return j;
}

std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') out += "&#xfffd;";
        else out += c;
    }
  }
  return out;
}

std::string render_html(const Diagnosis& d, bool synthetic) {
  std::ostringstream os;
  os << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Hostmortem incident "
     << d.incident.id
     << "</title><style>"
        ":root{--bg:#fff;--fg:#1b1d21;--muted:#5b616b;--line:#d9dce1;--ok:#1d6b3a;--warn:#8a5a00;--fail:#a1262b;--unk:#4b4f8a}"
        "@media (prefers-color-scheme:dark){:root{--bg:#121417;--fg:#e8eaee;--muted:#a3a9b3;--line:#2c3036;--ok:#6fcf8f;--warn:#e9b85a;--fail:#f08b8f;--unk:#a9adf0}}"
        "body{background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,sans-serif;margin:0 auto;max-width:960px;padding:16px}"
        "code,pre,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}"
        "h1{font-size:20px}h2{font-size:16px;margin-top:28px;border-bottom:1px solid var(--line);padding-bottom:4px}"
        ".v{font-weight:600}.supported{color:var(--ok)}.possible{color:var(--warn)}.ruled_out{color:var(--muted)}.unknown{color:var(--unk)}"
        "table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:4px 6px;border-bottom:1px solid var(--line);vertical-align:top}"
        ".banner{border:2px solid var(--warn);padding:8px;margin-bottom:12px}"
        ".hyp{border:1px solid var(--line);padding:8px 12px;margin:10px 0}"
        "a{color:inherit}"
        "</style></head><body><main>";
  if (synthetic) os << "<p class=\"banner\" role=\"note\"><strong>Synthetic data.</strong> Generated by <code>hm ingest</code>, not a live agent.</p>";
  os << "<h1>Incident " << d.incident.id << ": " << html_escape(d.incident.trigger) << " (" << html_escape(d.incident.subject) << ")</h1>";
  os << "<p>Status: " << to_string(d.incident.status) << ". Window: boot " << d.window_start.boot_ordinal << " to boot "
     << d.window_end.boot_ordinal << (d.spans_boots ? " <strong>(spans a reboot)</strong>" : "") << ", " << d.events.size()
     << " events.</p>";
  os << "<p class=\"v " << (d.conclusion == "supported" ? "supported" : "unknown") << "\">Conclusion: "
     << (d.conclusion == "supported" ? "supported" : "unknown") << ". " << html_escape(d.conclusion_reason) << "</p>";

  os << "<h2>Hypotheses</h2><p>Scores are evidence points, not probabilities.</p>";
  for (const auto& h : d.hypotheses) {
    os << "<section class=\"hyp\"><h3><span class=\"v " << to_string(h.verdict) << "\">" << to_string(h.verdict)
       << "</span>: " << html_escape(h.title) << " <span class=\"mono\">(score " << h.score << ")</span></h3><p>"
       << html_escape(h.explanation) << "</p><ul>";
    for (const auto& e : h.evidence) {
      os << "<li>" << (e.role == EvidenceRole::for_ ? "For" : e.role == EvidenceRole::against ? "Against" : "Missing") << ": ";
      if (e.event_id) os << "<a href=\"#e" << e.event_id << "\">#" << e.event_id << "</a> ";
      os << html_escape(e.note) << "</li>";
    }
    os << "</ul>";
    if (h.verdict != Verdict::ruled_out && !h.next_steps.empty()) {
      os << "<p>Next steps:</p><ul>";
      for (const auto& s : h.next_steps) os << "<li>" << html_escape(s) << "</li>";
      os << "</ul>";
    }
    os << "</section>";
  }
  if (!d.unobservable.empty()) {
    os << "<h2>Could not see</h2><ul>";
    for (const auto& u : d.unobservable) os << "<li><strong>" << html_escape(u.what) << "</strong>: " << html_escape(u.reason) << "</li>";
    os << "</ul>";
  }
  os << "<h2>Events</h2><table><caption>All events in the window, ordered by boot then CLOCK_BOOTTIME</caption>"
        "<thead><tr><th scope=\"col\">ID</th><th scope=\"col\">Boot</th><th scope=\"col\">Offset</th><th scope=\"col\">Kind</th>"
        "<th scope=\"col\">Subject</th><th scope=\"col\">Attributes</th></tr></thead><tbody>";
  for (const auto& se : d.events) {
    const std::int64_t origin = se.boot_ordinal == d.incident.opened.boot_ordinal ? d.incident.opened.mono_ns : 0;
    os << "<tr id=\"e" << se.event.id << "\"><td class=\"mono\">#" << se.event.id << "</td><td>" << se.boot_ordinal
       << "</td><td class=\"mono\">" << fmt_offset(se.event.mono_ns - origin) << "</td><td class=\"mono\">"
       << html_escape(se.event.kind) << "</td><td>" << html_escape(se.event.subject) << "</td><td class=\"mono\">"
       << html_escape(attrs_brief(se.event.attrs)) << "</td></tr>";
  }
  os << "</tbody></table></main></body></html>\n";
  return os.str();
}

}  // namespace hm
