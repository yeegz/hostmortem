#include "analysis/report.hpp"

#include <algorithm>
#include <cctype>
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

namespace {

constexpr const char* kCss =
    ":root{--bg:#f2eee4;--panel:#ebe5d8;--fg:#1d1b17;--muted:#6a6355;--line:#d4cbb8;--ok:#b3370a;--warn:#1d1b17;--fail:#b3370a;--unk:#1d1b17;--accent:#e2470e}"
    "@media (prefers-color-scheme:dark){:root{--bg:#13120f;--panel:#1b1915;--fg:#ece6d8;--muted:#9a927f;--line:#2f2b24;--ok:#ff8a57;--warn:#ece6d8;--fail:#ff8a57;--unk:#ece6d8;--accent:#ff6a2b}}"
    "body{background:var(--bg);color:var(--fg);font:15px/1.6 system-ui,sans-serif;margin:0 auto;max-width:1000px;padding:24px 16px}"
    "code,pre,.mono,h1,h2,h3{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}"
    "h1{font-size:22px;letter-spacing:-.02em;line-height:1.2}h2{font-size:15px;margin-top:36px;border-top:2px solid var(--fg);padding-top:10px}"
    "h3,h4{font-size:14px;margin:0 0 6px}"
    "h3{margin-top:20px}.hyp h3,.hyp h4{margin-top:0}.mono{font-size:12px}"
    ".v{font-weight:600}.supported{color:var(--ok)}.possible{color:var(--warn)}.ruled_out{color:var(--muted);text-decoration:line-through}"
    ".unknown{text-decoration:underline;text-decoration-color:var(--accent);text-decoration-thickness:3px;text-underline-offset:4px}"
    "table{border-collapse:collapse;width:100%;font-size:13px}th,td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);vertical-align:top}"
    "th{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--muted)}tr:target{background:var(--panel);outline:2px solid var(--accent)}"
    "caption{text-align:left;color:var(--muted);font-size:13px;padding:4px 0}"
    ".banner{border:2px dashed var(--accent);padding:10px 12px;margin-bottom:16px}"
    ".hyp{border-top:1px solid var(--line);padding:14px 0}"
    "details{margin:12px 0}summary{cursor:pointer;font-family:ui-monospace,Menlo,monospace;font-size:13px}"
    "a{color:var(--fail)}a:focus-visible,summary:focus-visible{outline:2px solid var(--accent);outline-offset:2px}"
    ".wrap{overflow-x:auto}";

void page_open(std::ostringstream& os, const std::string& title) {
  os << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"referrer\" content=\"no-referrer\"><title>"
     << html_escape(title) << "</title><style>" << kCss << "</style></head><body><main>";
}

const char* state_name(CollectorState s) {
  switch (s) {
    case CollectorState::active: return "active";
    case CollectorState::degraded: return "degraded";
    case CollectorState::unavailable: return "unavailable";
    case CollectorState::unknown: break;
  }
  return "unknown";
}

std::string anchor(std::int64_t incident, std::int64_t event) {
  return "i" + std::to_string(incident) + "e" + std::to_string(event);
}

// One incident's diagnosis. `h` is the heading level of the incident title;
// event rows get ids unique per incident, so several incidents whose windows
// overlap can share one page.
void write_incident(std::ostringstream& os, const Diagnosis& d, int h, bool events_open) {
  const auto hn = [&](int lvl) { return "h" + std::to_string(std::min(lvl, 6)); };
  const std::int64_t iid = d.incident.id;
  os << "<" << hn(h) << ">Incident " << iid << ": " << html_escape(d.incident.trigger) << " ("
     << html_escape(d.incident.subject) << ")</" << hn(h) << ">";
  os << "<p>Opened " << html_escape(format_wall(d.incident.opened_wall_ns, WallQuality::synced).substr(0, 20))
     << " (wall clock, display only). Status: " << to_string(d.incident.status) << ". Window: boot "
     << d.window_start.boot_ordinal << " to boot " << d.window_end.boot_ordinal
     << (d.spans_boots ? " <strong>(spans a reboot)</strong>" : "") << ", " << d.events.size() << " events.</p>";
  os << "<p class=\"v " << (d.conclusion == "supported" ? "supported" : "unknown") << "\">Conclusion: "
     << (d.conclusion == "supported" ? "supported" : "unknown") << ". " << html_escape(d.conclusion_reason) << "</p>";

  os << "<" << hn(h + 1) << ">Hypotheses</" << hn(h + 1) << "><p>Scores are evidence points, not probabilities.</p>";
  for (const auto& hy : d.hypotheses) {
    os << "<section class=\"hyp\"><" << hn(h + 2) << "><span class=\"v " << to_string(hy.verdict) << "\">"
       << to_string(hy.verdict) << "</span>: " << html_escape(hy.title) << " <span class=\"mono\">(score " << hy.score
       << ")</span></" << hn(h + 2) << "><p>" << html_escape(hy.explanation) << "</p><ul>";
    for (const auto& e : hy.evidence) {
      os << "<li>" << (e.role == EvidenceRole::for_ ? "For" : e.role == EvidenceRole::against ? "Against" : "Missing") << ": ";
      if (e.event_id) os << "<a href=\"#" << anchor(iid, e.event_id) << "\">#" << e.event_id << "</a> ";
      os << html_escape(e.note) << "</li>";
    }
    os << "</ul>";
    if (hy.verdict != Verdict::ruled_out && !hy.next_steps.empty()) {
      os << "<p>Next steps:</p><ul>";
      for (const auto& s : hy.next_steps) os << "<li>" << html_escape(s) << "</li>";
      os << "</ul>";
    }
    os << "</section>";
  }
  if (d.hypotheses.empty()) os << "<p>No playbook found anything to say about this window.</p>";
  if (!d.unobservable.empty()) {
    os << "<" << hn(h + 1) << ">Could not see</" << hn(h + 1) << "><ul>";
    for (const auto& u : d.unobservable) os << "<li><strong>" << html_escape(u.what) << "</strong>: " << html_escape(u.reason) << "</li>";
    os << "</ul>";
  }
  os << "<details" << (events_open ? " open" : "") << "><summary>Events in the window (" << d.events.size()
     << ")</summary><div class=\"wrap\"><table><caption>Ordered by boot, then CLOCK_BOOTTIME; offsets from incident open</caption>"
        "<thead><tr><th scope=\"col\">ID</th><th scope=\"col\">Boot</th><th scope=\"col\">Offset</th><th scope=\"col\">Kind</th>"
        "<th scope=\"col\">Subject</th><th scope=\"col\">Attributes</th></tr></thead><tbody>";
  for (const auto& se : d.events) {
    const std::int64_t origin = se.boot_ordinal == d.incident.opened.boot_ordinal ? d.incident.opened.mono_ns : 0;
    os << "<tr id=\"" << anchor(iid, se.event.id) << "\"><td class=\"mono\">#" << se.event.id << "</td><td>" << se.boot_ordinal
       << "</td><td class=\"mono\">" << fmt_offset(se.event.mono_ns - origin) << "</td><td class=\"mono\">"
       << html_escape(se.event.kind) << "</td><td>" << html_escape(se.event.subject) << "</td><td class=\"mono\">"
       << html_escape(attrs_brief(se.event.attrs)) << "</td></tr>";
  }
  os << "</tbody></table></div></details>";
}

}  // namespace

std::string render_html(const Diagnosis& d, bool synthetic) {
  std::ostringstream os;
  page_open(os, "Hostmortem incident " + std::to_string(d.incident.id));
  if (synthetic) os << "<p class=\"banner\" role=\"note\"><strong>Synthetic data.</strong> Generated by <code>hm ingest</code>, not a live agent.</p>";
  write_incident(os, d, 1, true);
  os << "<p class=\"mono\">Generated by hm explain (Hostmortem " HM_VERSION "). Event ids link to their rows.</p></main></body></html>\n";
  return os.str();
}

std::string render_html_report(const HostReport& r) {
  std::ostringstream os;
  page_open(os, "Hostmortem report");
  if (r.synthetic) os << "<p class=\"banner\" role=\"note\"><strong>Synthetic data.</strong> Generated by <code>hm ingest</code>, not a live agent.</p>";
  os << "<h1>Hostmortem report</h1><p>Generated " << html_escape(format_wall(r.generated_wall_ns, WallQuality::synced).substr(0, 20))
     << " by Hostmortem " HM_VERSION ". " << r.event_count << " events recorded across " << r.boots.size()
     << (r.boots.size() == 1 ? " boot" : " boots") << "; " << r.incidents.size()
     << (r.incidents.size() == 1 ? " incident" : " incidents") << " in this report.";
  if (r.redacted) os << " IP and MAC addresses are redacted.";
  os << "</p>";

  os << "<h2>Incidents</h2>";
  if (r.incidents.empty()) {
    os << "<p>No incidents recorded. Run <code>hm drill</code> to record a harmless test incident.</p>";
  } else {
    os << "<div class=\"wrap\"><table><thead><tr><th scope=\"col\">ID</th><th scope=\"col\">Opened (UTC)</th>"
          "<th scope=\"col\">Trigger</th><th scope=\"col\">Subject</th><th scope=\"col\">Conclusion</th></tr></thead><tbody>";
    for (const auto& d : r.incidents) {
      std::string lead = d.conclusion == "supported" && d.leading_index >= 0
                             ? d.hypotheses[static_cast<std::size_t>(d.leading_index)].title
                             : "unknown";
      os << "<tr><td class=\"mono\"><a href=\"#inc" << d.incident.id << "\">" << d.incident.id << "</a></td><td class=\"mono\">"
         << html_escape(format_wall(d.incident.opened_wall_ns, WallQuality::synced).substr(0, 20)) << "</td><td class=\"mono\">"
         << html_escape(d.incident.trigger) << "</td><td>" << html_escape(d.incident.subject) << (d.spans_boots ? " (spans reboot)" : "")
         << "</td><td class=\"v " << (d.conclusion == "supported" ? "supported" : "unknown") << "\">" << html_escape(lead) << "</td></tr>";
    }
    os << "</tbody></table></div>";
  }

  os << "<h2>Coverage</h2><p>What the agent could observe on the latest boot. Every diagnosis lists the gaps that affect it.</p>";
  if (r.coverage.collectors.empty()) {
    os << "<p>No collector status recorded yet.</p>";
  } else {
    os << "<div class=\"wrap\"><table><thead><tr><th scope=\"col\">Collector</th><th scope=\"col\">State</th><th scope=\"col\">Reason</th></tr></thead><tbody>";
    for (const auto& [name, e] : r.coverage.collectors) {
      os << "<tr><td class=\"mono\">" << html_escape(name) << "</td><td>" << state_name(e.state) << "</td><td>" << html_escape(e.reason) << "</td></tr>";
    }
    os << "</tbody></table></div>";
  }

  os << "<h2>Boots</h2><div class=\"wrap\"><table><thead><tr><th scope=\"col\">Boot</th><th scope=\"col\">Boot id</th>"
        "<th scope=\"col\">Kernel</th><th scope=\"col\">First seen (UTC)</th></tr></thead><tbody>";
  for (const auto& b : r.boots) {
    os << "<tr><td>" << b.ordinal << "</td><td class=\"mono\">" << html_escape(b.boot_id.substr(0, 8)) << "</td><td class=\"mono\">"
       << html_escape(b.kernel) << "</td><td class=\"mono\">" << html_escape(format_wall(b.first_seen_wall_ns, WallQuality::synced).substr(0, 20))
       << "</td></tr>";
  }
  os << "</tbody></table></div>";

  for (const auto& d : r.incidents) {
    os << "<section id=\"inc" << d.incident.id << "\">";
    write_incident(os, d, 2, false);
    os << "</section>";
  }
  os << "<p class=\"mono\">Generated by hm report (Hostmortem " HM_VERSION
        "). This file is self-contained: no scripts, no network requests.</p></main></body></html>\n";
  return os.str();
}

std::string redact_ips(std::string_view s) {
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t j = i;
    int dots = 0, digits = 0;
    while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) {
      if (s[j] == '.') ++dots;
      else ++digits;
      ++j;
    }
    if (j > i && dots == 3 && digits >= 4) {
      out += "x.x.x.x";
      i = j;
    } else if (j > i) {
      out.append(s, i, j - i);
      i = j;
    } else if (s[i] != ':') {
      out += s[i++];
    } else {
      // IPv6 or MAC: a run of hex digits and colons with at least two colons.
      std::size_t k = i;
      int colons = 0;
      while (k < s.size() && (std::isxdigit(static_cast<unsigned char>(s[k])) || s[k] == ':')) {
        if (s[k] == ':') ++colons;
        ++k;
      }
      if (colons >= 2) {
        std::size_t back = out.size();
        while (back > 0 && std::isxdigit(static_cast<unsigned char>(out[back - 1]))) --back;
        out.resize(back);
        out += "x:x::x";
        i = k;
      } else {
        out += s[i++];
      }
    }
  }
  return out;
}

Diagnosis redact_diagnosis(Diagnosis d) {
  auto event = [](Event& e) {
    e.subject = redact_ips(e.subject);
    for (auto& [k, v] : e.attrs) {
      if (auto* str = std::get_if<std::string>(&v)) *str = redact_ips(*str);
    }
  };
  d.incident.subject = redact_ips(d.incident.subject);
  d.conclusion_reason = redact_ips(d.conclusion_reason);
  for (auto& h : d.hypotheses) {
    h.explanation = redact_ips(h.explanation);
    for (auto& e : h.evidence) e.note = redact_ips(e.note);
    for (auto& s : h.next_steps) s = redact_ips(s);
  }
  for (auto& u : d.unobservable) u.reason = redact_ips(u.reason);
  for (auto& se : d.events) event(se.event);
  return d;
}

}  // namespace hm
