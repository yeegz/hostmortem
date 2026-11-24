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

// Recorder hardware, matching the project site: graphite panels in layers
// (recessed wells, raised bevelled cards), one phosphor green for what is
// known, steel for what is only possible, green and red status lights, and
// hatching for what could not be seen. No gradients on text, no glows.
// Printing switches to dark ink on white. Everything inline: the CSP allows
// no other source.
constexpr const char* kCss =
    ":root{--bg:#141a17;--card:#1c2420;--card2:#202a25;--well:#0b0f0d;--fg:#e3ebe5;--fg2:#aebbb3;--muted:#97a79e;"
    "--line:rgba(160,185,170,.14);--acc:#6fdc9a;--acc-ink:#9be8b8;--steel:#a7bdb0;--red:#ff5f6b;--ok:#6fdc9a;"
    "--bevel:inset 0 1px 0 rgba(255,255,255,.07),inset 0 -1px 0 rgba(0,0,0,.35);"
    "--lift:0 1px 0 rgba(0,0,0,.5),0 10px 24px -12px rgba(0,0,0,.7);"
    "--sunk:inset 0 2px 5px rgba(0,0,0,.55),inset 0 0 0 1px rgba(0,0,0,.45),0 1px 0 rgba(255,255,255,.05);color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0 auto;max-width:1040px;padding:28px 16px 48px;color:var(--fg);font:15px/1.6 system-ui,-apple-system,'Segoe UI',sans-serif;"
    "background:linear-gradient(180deg,#19211d,var(--bg) 420px)}"
    "code,.mono,th,summary,caption{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "h1{font-size:26px;line-height:1.15;letter-spacing:-.02em;margin:0 0 10px;color:#f1f6f2}"
    "h2{font-size:18px;letter-spacing:-.01em;margin:36px 0 12px;color:#edf3ee}"
    "h3,h4{font-size:15px;margin:22px 0 8px;color:#edf3ee}.hyp h3,.hyp h4{margin:0 0 6px}"
    "p{color:var(--fg2);max-width:80ch}.mono{font-size:12px;color:var(--muted)}"
    "a{color:var(--acc-ink);text-decoration-color:rgba(155,232,184,.4);text-underline-offset:3px}"
    "a:hover{color:#fff}a:focus-visible,summary:focus-visible,.wrap:focus-visible{outline:2px solid var(--acc);outline-offset:2px;border-radius:4px}"
    // Raised, bevelled cards: each incident in a report, each hypothesis.
    "main>section[id]{margin:22px 0;padding:6px 22px 18px;border-radius:18px;background:var(--card);"
    "border:1px solid var(--line);box-shadow:var(--bevel),var(--lift)}"
    ".hyp{margin:10px 0;padding:14px 16px;border-radius:12px;background:var(--card2);border:1px solid var(--line);box-shadow:var(--bevel)}"
    ".hyp ul{margin:8px 0 0;padding-left:20px;color:var(--fg2)}.hyp li{margin:3px 0}"
    ".hyp a{font:500 12px/1 ui-monospace,Menlo,monospace;text-decoration:none;padding:3px 6px;border-radius:5px;"
    "background:var(--well);box-shadow:var(--sunk)}"
    ".hyp a:hover{background:var(--acc);color:#062014}"
    // Verdicts: keycap pills in titles, an inset callout for the conclusion.
    "span.v{display:inline-block;padding:2px 10px;border-radius:6px;font-size:12px;font-weight:600;margin-right:6px;vertical-align:1px}"
    "span.supported{color:#062014;background:var(--acc);box-shadow:inset 0 1px 0 rgba(255,255,255,.35),0 2px 0 #2f8a57}"
    "span.possible{color:#e6eee8;background:#2b3530;box-shadow:inset 0 1px 0 rgba(255,255,255,.12),0 2px 0 #121b26}"
    "span.ruled_out{color:var(--muted);background:transparent;box-shadow:inset 0 0 0 1px var(--line);text-decoration:line-through}"
    "p.v{font-weight:600;color:var(--fg);padding:12px 16px;border-radius:10px;background:var(--well);box-shadow:var(--sunk)}"
    "p.v.supported{border-left:3px solid var(--acc)}p.v.unknown{border-left:3px solid var(--steel)}"
    "td.v{font-weight:600}td.supported{color:var(--acc-ink)}td.unknown{color:var(--steel)}"
    // Recessed wells: tables sit below the panel surface.
    ".wrap{overflow-x:auto;border-radius:12px;background:var(--well);box-shadow:var(--sunk);margin:10px 0}"
    "table{border-collapse:collapse;width:100%;font-size:13px}"
    "th,td{text-align:left;padding:7px 10px;border-bottom:1px solid rgba(160,185,170,.08);vertical-align:top}"
    "th{font-size:12px;font-weight:500;color:var(--muted);background:rgba(160,185,170,.05)}"
    "tbody tr:hover{background:rgba(111,220,154,.05)}"
    "tr:target{background:rgba(111,220,154,.13);box-shadow:inset 3px 0 0 var(--acc)}"
    "caption{text-align:left;color:var(--muted);font-size:12px;padding:10px 10px 4px}"
    "details{margin:14px 0 4px}"
    "summary{cursor:pointer;display:inline-block;font-size:13px;padding:8px 14px;border-radius:8px;color:var(--fg);"
    "background:#2b3530;box-shadow:inset 0 1px 0 rgba(255,255,255,.10),0 3px 0 #0c110e}"
    "summary:active{transform:translateY(2px);box-shadow:inset 0 1px 0 rgba(255,255,255,.08),0 1px 0 #0c110e}"
    "details[open] summary{margin-bottom:6px}"
    // What it could not see: hatched, like a gap in the tape.
    "ul.blind{padding:12px 16px 12px 34px;border-radius:12px;color:var(--fg2);"
    "background:repeating-linear-gradient(135deg,rgba(160,185,170,.07) 0 8px,transparent 8px 16px),#121915;box-shadow:inset 0 0 0 1px rgba(160,185,170,.2)}"
    ".head{display:flex;align-items:center;gap:12px;margin:4px 0 8px}.head h1{margin:0}"
    ".mark{width:34px;height:34px;flex:0 0 auto;border-radius:8px;box-shadow:0 2px 0 #2f8a57,0 6px 14px -6px rgba(0,0,0,.8)}"
    ".brand{font-weight:700;font-size:17px;letter-spacing:-.01em;color:#edf3ee}"
    ".st{display:inline-flex;align-items:center;gap:7px}.st::before{content:'';width:8px;height:8px;border-radius:50%;"
    "box-shadow:inset 0 -1px 1px rgba(0,0,0,.35)}"
    ".st-active::before{background:var(--ok)}.st-degraded::before{background:#d6cf5a}"
    ".st-unavailable{color:#ffb3b9}.st-unavailable::before{background:var(--red)}"
    ".st-unknown::before{background:transparent;box-shadow:inset 0 0 0 1.5px var(--muted)}"
    ".banner{padding:12px 16px;border-radius:10px;color:#ffd3d6;background:rgba(255,95,107,.10);box-shadow:inset 0 0 0 1px rgba(255,95,107,.45)}"
    "@media print{:root{--bg:#fff;--card:#fff;--card2:#fff;--well:#fff;--fg:#111;--fg2:#333;--muted:#444;--line:#ccc;--acc-ink:#17512f;--steel:#354940;color-scheme:light}"
    "body,ul.blind,summary,span.v{background:#fff}h1,h2,h3,h4,.brand,span.v{color:#111}"
    ".hyp a,ul.blind,span.v{box-shadow:none}.st-unavailable,.banner{color:#722631}.banner{background:#fff}"
    "main>section[id],.hyp,.wrap,p.v,summary{box-shadow:none;border:1px solid #ccc}details>*{display:block}}";

// The project mark as inline SVG (an element, not a fetched resource, so the
// strict CSP still holds): a trace that spikes, breaks at a reboot, resumes.
constexpr const char* kMark =
    "<svg class=\"mark\" viewBox=\"0 0 32 32\" aria-hidden=\"true\"><defs><linearGradient id=\"hmk\" x1=\"0\" y1=\"0\" x2=\"0\" y2=\"1\">"
    "<stop offset=\"0\" stop-color=\"#26302b\"/><stop offset=\"1\" stop-color=\"#121814\"/></linearGradient></defs>"
    "<rect width=\"32\" height=\"32\" rx=\"8\" fill=\"url(#hmk)\"/>"
    "<rect x=\".5\" y=\".5\" width=\"31\" height=\"31\" rx=\"7.5\" fill=\"none\" stroke=\"#fff\" stroke-opacity=\".1\"/>"
    "<path d=\"M4.5 18.5H9.2L11.6 9L14.6 24.5L16.4 18.5M22.4 18.5H25.6\" fill=\"none\" stroke=\"#6fdc9a\" stroke-width=\"2.4\" "
    "stroke-linecap=\"round\" stroke-linejoin=\"round\"/><path d=\"M19.4 6.5V25.5\" stroke=\"#8a9a91\" stroke-width=\"1.4\" "
    "stroke-linecap=\"round\" stroke-dasharray=\"1.8 2.2\"/><circle cx=\"27.4\" cy=\"18.5\" r=\"1.7\" fill=\"#6fdc9a\"/></svg>";

void page_open(std::ostringstream& os, const std::string& title) {
  os << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"referrer\" content=\"no-referrer\"><title>"
     << html_escape(title) << "</title><style>" << kCss << "</style></head><body><main>";
}

void table_open(std::ostringstream& os, std::string_view name) {
  os << "<div class=\"wrap\" role=\"region\" tabindex=\"0\" aria-label=\"" << html_escape(name)
     << "\"><table>";
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
       << (hy.verdict == Verdict::ruled_out ? "ruled out" : to_string(hy.verdict)) << "</span> " << html_escape(hy.title) << " <span class=\"mono\">(score " << hy.score
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
    os << "<" << hn(h + 1) << ">Could not see</" << hn(h + 1) << "><ul class=\"blind\">";
    for (const auto& u : d.unobservable) os << "<li><strong>" << html_escape(u.what) << "</strong>: " << html_escape(u.reason) << "</li>";
    os << "</ul>";
  }
  os << "<details" << (events_open ? " open" : "") << "><summary>Events in the window (" << d.events.size()
     << ")</summary>";
  table_open(os, "Events in the window");
  os << "<caption>Ordered by boot, then CLOCK_BOOTTIME; offsets from incident open</caption>"
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
  os << "<header class=\"head\">" << kMark << "<span class=\"brand\">Hostmortem</span></header>";
  write_incident(os, d, 1, true);
  os << "<p class=\"mono\">Generated by hm explain (Hostmortem " HM_VERSION "). Event ids link to their rows.</p></main></body></html>\n";
  return os.str();
}

std::string render_html_report(const HostReport& r) {
  std::ostringstream os;
  page_open(os, "Hostmortem report");
  if (r.synthetic) os << "<p class=\"banner\" role=\"note\"><strong>Synthetic data.</strong> Generated by <code>hm ingest</code>, not a live agent.</p>";
  os << "<header class=\"head\">" << kMark << "<h1>Hostmortem report</h1></header><p>Generated " << html_escape(format_wall(r.generated_wall_ns, WallQuality::synced).substr(0, 20))
     << " by Hostmortem " HM_VERSION ". " << r.event_count << " events recorded across " << r.boots.size()
     << (r.boots.size() == 1 ? " boot" : " boots") << "; " << r.incidents.size()
     << (r.incidents.size() == 1 ? " incident" : " incidents") << " in this report.";
  if (r.redacted) os << " IP and MAC addresses are redacted.";
  os << "</p>";

  os << "<h2>Incidents</h2>";
  if (r.incidents.empty()) {
    os << "<p>No incidents recorded. Run <code>hm drill</code> to record a harmless test incident.</p>";
  } else {
    table_open(os, "Incidents");
    os << "<thead><tr><th scope=\"col\">ID</th><th scope=\"col\">Opened (UTC)</th>"
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
    table_open(os, "Coverage");
    os << "<thead><tr><th scope=\"col\">Collector</th><th scope=\"col\">State</th><th scope=\"col\">Reason</th></tr></thead><tbody>";
    for (const auto& [name, e] : r.coverage.collectors) {
      os << "<tr><td class=\"mono\">" << html_escape(name) << "</td><td><span class=\"st st-" << state_name(e.state) << "\">"
         << state_name(e.state) << "</span></td><td>" << html_escape(e.reason) << "</td></tr>";
    }
    os << "</tbody></table></div>";
  }

  os << "<h2>Boots</h2>";
  table_open(os, "Boots");
  os << "<thead><tr><th scope=\"col\">Boot</th><th scope=\"col\">Boot id</th>"
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
