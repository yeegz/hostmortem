// hm: inspect what hmd recorded.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "analysis/correlator.hpp"
#include "analysis/detector.hpp"
#include "analysis/report.hpp"
#include "cli/doctor.hpp"
#include "core/config.hpp"
#include "core/json_codec.hpp"
#include "core/kinds.hpp"
#include "store/store.hpp"

namespace {

using namespace hm;

const char* kUsage =
    "usage: hm [--db FILE] <command> [options]\n"
    "\n"
    "commands:\n"
    "  status                      boots, event counts, open incidents\n"
    "  timeline [--last 15m] [--all] [--kind PREFIX] [--source S] [--verbose] [--limit N] [--json]\n"
    "  incidents [--json]          list incidents\n"
    "  explain ID [--json] [--html FILE]\n"
    "                              evidence-linked hypotheses for an incident\n"
    "  doctor [--json]             what this host lets Hostmortem observe\n"
    "  export [--redact]           all events as JSON lines on stdout\n"
    "  ingest FILE.jsonl           load synthetic/lab events into a demo store (requires --db)\n"
    "  backup FILE                 consistent copy of the store (VACUUM INTO)\n"
    "\n"
    "The store defaults to $HM_DB or /var/lib/hostmortem/events.db.\n";

struct Args {
  std::string db;
  bool db_given = false;
  std::string cmd;
  std::vector<std::string> pos;
  std::map<std::string, std::string> opt;
  bool flag(const std::string& f) const { return opt.count(f) > 0; }
  std::string get(const std::string& f, const std::string& def = "") const {
    auto it = opt.find(f);
    return it == opt.end() ? def : it->second;
  }
};

int fail(const std::string& msg, int code = 1) {
  std::cerr << "hm: " << msg << "\n";
  return code;
}

Result<std::unique_ptr<Store>> open_ro(const Args& a) {
  auto s = Store::open(a.db, Store::Mode::read_only);
  if (!s && s.error().code == "sqlite_cantopen") {
    return make_error("no_store", "no store at " + a.db + " (is hmd running? set --db or HM_DB)");
  }
  return s;
}

bool is_synthetic(Store& s) {
  auto m = s.meta("synthetic");
  return m && *m && **m == "1";
}

std::string pretty_fact(const StoredEvent& se) {
  return format_event_line(se, 0);
}

int cmd_status(const Args& a) {
  auto s = open_ro(a);
  if (!s) return fail(s.error().message);
  auto& st = **s;
  if (is_synthetic(st)) std::cout << "!! SYNTHETIC DATA store (hm ingest)\n";
  auto boots = st.boots();
  auto count = st.event_count();
  auto incs = st.incidents(false);
  if (!boots || !count || !incs) return fail("query failed");
  std::cout << "store: " << a.db << "  schema v" << st.schema_version() << "  events: " << *count << "\n";
  std::cout << "boots recorded: " << boots->size() << "\n";
  for (const auto& b : *boots) {
    std::cout << "  boot " << b.ordinal << "  " << b.boot_id.substr(0, 8) << "  kernel " << b.kernel << "  first seen "
              << format_wall(b.first_seen_wall_ns, WallQuality::synced).substr(0, 20) << "\n";
  }
  int open = 0;
  for (const auto& i : *incs) open += i.status == IncidentStatus::open;
  std::cout << "incidents: " << incs->size() << " (" << open << " open)\n";
  return 0;
}

int cmd_timeline(const Args& a) {
  auto s = open_ro(a);
  if (!s) return fail(s.error().message);
  auto& st = **s;
  auto boots = st.boots();
  if (!boots) return fail(boots.error().message);
  if (boots->empty()) {
    std::cout << "(no events recorded yet)\n";
    return 0;
  }
  EventQuery q;
  q.kind_prefix = a.get("--kind");
  q.source = a.get("--source");
  q.limit = static_cast<std::size_t>(std::strtoull(a.get("--limit", "100000").c_str(), nullptr, 10));
  if (!a.flag("--all")) {
    auto last = parse_duration(a.get("--last", "15m"));
    if (!last) return fail("bad --last duration", 2);
    // Relative to the newest event of the latest boot, by CLOCK_BOOTTIME.
    EventQuery probe;
    probe.from = TimePoint{boots->back().ordinal, boots->back().boot_id, 0};
    auto evs = st.query(probe);
    if (!evs) return fail(evs.error().message);
    const std::int64_t newest = evs->empty() ? 0 : evs->back().event.mono_ns;
    q.from = TimePoint{boots->back().ordinal, boots->back().boot_id,
                       newest - std::chrono::duration_cast<std::chrono::nanoseconds>(*last).count()};
  }
  auto evs = st.query(q);
  if (!evs) return fail(evs.error().message);
  const bool verbose = a.flag("--verbose");
  if (a.flag("--json")) {
    for (const auto& se : *evs) {
      if (!verbose && se.event.severity == Severity::debug) continue;
      auto j = event_to_json(se.event);
      j["boot_ordinal"] = se.boot_ordinal;
      std::cout << j.dump() << "\n";
    }
    return 0;
  }
  if (is_synthetic(st)) std::cout << "!! SYNTHETIC DATA store (hm ingest)\n";
  std::int64_t boot = -1;
  std::size_t hidden = 0;
  for (const auto& se : *evs) {
    if (!verbose && se.event.severity == Severity::debug) {
      ++hidden;
      continue;
    }
    if (se.boot_ordinal != boot) {
      std::cout << "---- boot " << se.boot_ordinal << " (" << se.event.boot_id.substr(0, 8) << ") ----  offsets are CLOCK_BOOTTIME seconds\n";
      boot = se.boot_ordinal;
    }
    std::cout << pretty_fact(se) << "\n";
  }
  if (hidden) std::cout << "(" << hidden << " routine samples and successful probes hidden; --verbose shows them)\n";
  return 0;
}

int cmd_incidents(const Args& a) {
  auto s = open_ro(a);
  if (!s) return fail(s.error().message);
  auto incs = (*s)->incidents(false);
  if (!incs) return fail(incs.error().message);
  if (a.flag("--json")) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& i : *incs) {
      j.push_back({{"id", i.id}, {"trigger", i.trigger}, {"subject", i.subject}, {"status", to_string(i.status)},
                   {"opened_boot_ordinal", i.opened.boot_ordinal}, {"opened_mono_ns", i.opened.mono_ns},
                   {"opened_wall_ns", i.opened_wall_ns},
                   {"spans_boots", i.closed.boot_ordinal != 0 && i.closed.boot_ordinal != i.opened.boot_ordinal}});
    }
    std::cout << j.dump(2) << "\n";
    return 0;
  }
  if (is_synthetic(**s)) std::cout << "!! SYNTHETIC DATA store (hm ingest)\n";
  if (incs->empty()) {
    std::cout << "No incidents recorded.\n";
    return 0;
  }
  std::cout << "  ID  STATUS       BOOT  OPENED (wall, display only)  TRIGGER               SUBJECT\n";
  for (const auto& i : *incs) {
    char line[256];
    std::snprintf(line, sizeof line, "%4lld  %-11s  %4lld  %-27s  %-20s  %s%s", static_cast<long long>(i.id),
                  to_string(i.status), static_cast<long long>(i.opened.boot_ordinal),
                  format_wall(i.opened_wall_ns, WallQuality::synced).substr(0, 20).c_str(), i.trigger.c_str(),
                  i.subject.c_str(),
                  i.closed.boot_ordinal != 0 && i.closed.boot_ordinal != i.opened.boot_ordinal ? "  (spans reboot)" : "");
    std::cout << line << "\n";
  }
  return 0;
}

int cmd_explain(const Args& a) {
  if (a.pos.empty()) return fail("explain needs an incident id", 2);
  const auto id = std::strtoll(a.pos[0].c_str(), nullptr, 10);
  // Read-write so hypotheses are persisted for the report and retention; fall
  // back to read-only when the user cannot write the store.
  auto rw = Store::open(a.db, Store::Mode::read_write);
  std::unique_ptr<Store> st;
  if (rw) st = std::move(rw).value();
  else {
    auto ro = open_ro(a);
    if (!ro) return fail(ro.error().message);
    st = std::move(ro).value();
  }
  auto d = diagnose_incident(*st, id);
  if (!d) return fail(d.error().message);
  if (rw) (void)st->replace_hypotheses(id, d->hypotheses);
  const bool synth = is_synthetic(*st);
  if (auto html = a.get("--html"); !html.empty()) {
    std::ofstream out(html);
    out << render_html(*d, synth);
    if (!out) return fail("cannot write " + html);
    std::cerr << "wrote " << html << "\n";
  }
  if (a.flag("--json")) {
    auto j = diagnosis_json(*d);
    j["synthetic"] = synth;
    std::cout << j.dump(2) << "\n";
  } else if (a.get("--html").empty()) {
    std::cout << render_text(*d, synth);
  }
  return 0;
}

// Replaces IPv4 literals with 'x.x.x.x' (keeps the last octet for 10/172/192
// private ranges distinguishable? No: redaction means redaction).
std::string redact_ips(const std::string& s) {
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t j = i;
    int dots = 0, digits = 0;
    while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) {
      if (s[j] == '.') ++dots; else ++digits;
      ++j;
    }
    if (j > i && dots == 3 && digits >= 4) {
      out += "x.x.x.x";
      i = j;
    } else if (j > i) {
      out.append(s, i, j - i);
      i = j;
    } else if (s[i] != ':' ) {
      out += s[i++];
    } else {
      // crude IPv6: a run of hex and colons with at least two colons
      std::size_t k = i;
      int colons = 0;
      while (k < s.size() && (std::isxdigit(static_cast<unsigned char>(s[k])) || s[k] == ':')) {
        if (s[k] == ':') ++colons;
        ++k;
      }
      if (colons >= 2) {
        // back up over the hex prefix already copied
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

int cmd_export(const Args& a) {
  auto s = open_ro(a);
  if (!s) return fail(s.error().message);
  EventQuery q;
  q.limit = static_cast<std::size_t>(-1) >> 1;
  auto evs = (*s)->query(q);
  if (!evs) return fail(evs.error().message);
  const bool redact = a.flag("--redact");
  for (auto& se : *evs) {
    Event e = se.event;
    e.id = 0;
    if (redact) {
      e.subject = redact_ips(e.subject);
      for (auto& [k, v] : e.attrs) {
        if (auto* str = std::get_if<std::string>(&v)) *str = redact_ips(*str);
      }
    }
    std::cout << event_to_json_line(e) << "\n";
  }
  return 0;
}

int cmd_ingest(const Args& a) {
  if (!a.db_given) return fail("ingest writes a demo store: pass --db explicitly", 2);
  if (a.pos.empty()) return fail("ingest needs a JSONL file", 2);
  std::ifstream in(a.pos[0]);
  if (!in) return fail("cannot read " + a.pos[0]);
  auto s = Store::open(a.db, Store::Mode::read_write);
  if (!s) return fail(s.error().message);
  auto& st = **s;
  if (auto live = st.meta("live"); live && *live && **live == "1") {
    return fail("refusing to ingest synthetic events into a live agent store", 2);
  }
  if (auto r = st.set_meta("synthetic", "1"); !r) return fail(r.error().message);

  std::vector<Event> batch;
  std::string line;
  std::size_t lineno = 0, rejected = 0, inserted = 0, dups = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#') continue;
    auto e = event_from_json_line(line);
    if (!e) {
      ++rejected;
      std::cerr << "hm: line " << lineno << " rejected: " << e.error().message << "\n";
      continue;
    }
    batch.push_back(std::move(e).value());
  }
  auto r = st.insert_batch(batch);
  if (!r) return fail(r.error().message);
  inserted = r->inserted;
  dups = r->duplicates;

  // Replay the detector over the whole store in timeline order so incidents
  // come out exactly as the live agent would have opened them.
  if (inserted > 0) {
    if (auto existing = st.incidents(false); existing && existing->empty()) {
      EventQuery q;
      auto all = st.query(q);
      if (!all) return fail(all.error().message);
      Detector det(DetectorConfig{});
      for (const auto& se : *all) {
        for (auto& act : det.observe(se)) {
          if (act.type == Detector::Action::Type::open) {
            auto id = st.open_incident(act.incident);
            if (id) det.set_open_id(*id);
          } else {
            (void)st.close_incident(act.incident.id, act.incident.closed, act.incident.status);
          }
        }
      }
    }
  }
  std::cout << "ingested " << inserted << " events (" << dups << " duplicates ignored, " << rejected
            << " rejected). Store marked SYNTHETIC.\n";
  return rejected > 0 ? 3 : 0;
}

int cmd_backup(const Args& a) {
  if (a.pos.empty()) return fail("backup needs a destination path", 2);
  auto s = Store::open(a.db, Store::Mode::read_write);
  if (!s) return fail(s.error().message);
  if (auto r = (*s)->backup_to(a.pos[0]); !r) return fail(r.error().message);
  std::cout << "backup written to " << a.pos[0] << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  const char* env = std::getenv("HM_DB");
  a.db = env ? env : "/var/lib/hostmortem/events.db";
  a.db_given = env != nullptr;
  static const std::map<std::string, bool> takes_value = {
      {"--last", true}, {"--kind", true}, {"--source", true}, {"--limit", true}, {"--html", true},
      {"--all", false}, {"--verbose", false}, {"--json", false}, {"--redact", false}};
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s == "--db" && i + 1 < argc) {
      a.db = argv[++i];
      a.db_given = true;
    } else if (s == "-h" || s == "--help") {
      std::cout << kUsage;
      return 0;
    } else if (s == "--version") {
      std::cout << "hm 0.1.0-dev\n";
      return 0;
    } else if (s.rfind("--", 0) == 0) {
      auto it = takes_value.find(s);
      if (it == takes_value.end()) return fail("unknown option " + s, 2);
      if (it->second) {
        if (i + 1 >= argc) return fail(s + " needs a value", 2);
        a.opt[s] = argv[++i];
      } else {
        a.opt[s] = "1";
      }
    } else if (a.cmd.empty()) {
      a.cmd = s;
    } else {
      a.pos.push_back(s);
    }
  }
  if (a.cmd == "status") return cmd_status(a);
  if (a.cmd == "timeline") return cmd_timeline(a);
  if (a.cmd == "incidents") return cmd_incidents(a);
  if (a.cmd == "explain") return cmd_explain(a);
  if (a.cmd == "doctor") return run_doctor(a.db, a.flag("--json"));
  if (a.cmd == "export") return cmd_export(a);
  if (a.cmd == "ingest") return cmd_ingest(a);
  if (a.cmd == "backup") return cmd_backup(a);
  std::cerr << kUsage;
  return 2;
}
