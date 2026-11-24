// hm: inspect what hmd recorded.
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <thread>
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
    "  report [-o FILE] [--incident ID] [--redact]\n"
    "                              one self-contained HTML file: coverage, boots and every\n"
    "                              incident's diagnosis; open it in a browser or send it\n"
    "  drill                       start and SIGKILL a harmless 'hm-drill' process, so the\n"
    "                              running agent records a real incident end to end\n"
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
  if (::access(a.db.c_str(), F_OK) == 0 && ::access(a.db.c_str(), R_OK) != 0) {
    return make_error("no_access", "cannot read " + a.db +
                                       ": add yourself to the hostmortem group (sudo usermod -aG hostmortem \"$USER\", "
                                       "then log in again) or run hm with sudo");
  }
  auto s = Store::open(a.db, Store::Mode::read_only);
  if (!s && s.error().code == "sqlite_cantopen") {
    return make_error("no_store", "no store at " + a.db +
                                      " (is hmd running? sudo systemctl enable --now hmd; or set --db or HM_DB)");
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
  // Root (sudo hm explain) never opens another user's store read-write, so it
  // cannot leave root-owned WAL files behind that would stop hmd writing.
  struct stat sb{};
  const bool foreign_root = ::geteuid() == 0 && ::stat(a.db.c_str(), &sb) == 0 && sb.st_uid != 0;
  auto rw = foreign_root ? Result<std::unique_ptr<Store>>(make_error("skip", "read-only for root"))
                         : Store::open(a.db, Store::Mode::read_write);
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

std::string default_report_name() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, "hostmortem-report-%Y%m%d-%H%M%S.html", &tm);
  return buf;
}

int cmd_report(const Args& a) {
  auto s = open_ro(a);
  if (!s) return fail(s.error().message);
  auto& st = **s;
  HostReport r;
  r.generated_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
  r.synthetic = is_synthetic(st);
  r.redacted = a.flag("--redact");
  auto boots = st.boots();
  auto count = st.event_count();
  auto incs = st.incidents(false);
  if (!boots || !count || !incs) return fail("query failed");
  r.boots = *boots;
  r.event_count = *count;
  if (!r.boots.empty()) {
    EventQuery q;
    q.from = TimePoint{r.boots.back().ordinal, r.boots.back().boot_id, 0};
    q.kind_prefix = kind::collector_status;
    if (auto ev = st.query(q)) r.coverage = availability_from(*ev);
  }
  std::vector<std::int64_t> ids;
  if (auto one = a.get("--incident"); !one.empty()) {
    ids.push_back(std::strtoll(one.c_str(), nullptr, 10));
  } else {
    for (auto it = incs->rbegin(); it != incs->rend(); ++it) ids.push_back(it->id);  // newest first
  }
  for (auto id : ids) {
    auto d = diagnose_incident(st, id);
    if (!d) return fail(d.error().message);
    r.incidents.push_back(r.redacted ? redact_diagnosis(std::move(d).value()) : std::move(d).value());
  }
  if (r.redacted) {
    for (auto& [name, e] : r.coverage.collectors) e.reason = redact_ips(e.reason);
  }
  const std::string out_path = a.get("-o", default_report_name());
  {
    std::ofstream out(out_path);
    out << render_html_report(r);
    if (!out) return fail("cannot write " + out_path);
  }
  std::cout << "wrote " << out_path << " (" << r.incidents.size() << (r.incidents.size() == 1 ? " incident" : " incidents")
            << (r.redacted ? ", addresses redacted" : "") << "). Open it in any browser; it makes no network requests.\n";
  return 0;
}

// The drill is a real fault, made harmless: a child process renamed
// "hm-drill" (which hmd always watches) is started, seen, then SIGKILLed.
// The running agent records the exit through its normal path (process watch,
// queue, store, detector), so the incident it produces is a real one.
int cmd_drill(const Args& a) {
  constexpr const char* kName = "hm-drill";
  std::optional<std::int64_t> newest_before;
  {
    auto s = open_ro(a);
    if (!s) return fail(s.error().message);
    if (is_synthetic(**s)) return fail("this is a synthetic store; the drill needs the live agent's store", 2);
    if (auto incs = (*s)->incidents(false); incs && !incs->empty()) newest_before = incs->back().id;
  }
  const pid_t pid = ::fork();
  if (pid < 0) return fail("fork failed");
  if (pid == 0) {
    ::prctl(PR_SET_NAME, kName, 0, 0, 0);
    for (;;) ::pause();
  }
  std::cout << "drill: started '" << kName << "' (pid " << pid << "); hmd polls processes once a second\n";
  std::this_thread::sleep_for(std::chrono::seconds(3));
  ::kill(pid, SIGKILL);
  int wstatus = 0;
  ::waitpid(pid, &wstatus, 0);
  std::cout << "drill: sent SIGKILL to pid " << pid << "; waiting for the agent to record it\n";

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto s = open_ro(a);
    if (!s) continue;
    EventQuery q;
    q.kind_prefix = kind::process_exit;
    q.source = "process";
    auto evs = (*s)->query(q);
    if (!evs) continue;
    bool seen = false;
    for (const auto& se : *evs) {
      seen = seen || (se.event.subject == kName && se.event.attr_int("pid").value_or(0) == pid);
    }
    if (!seen) continue;
    auto incs = (*s)->incidents(false);
    if (!incs || incs->empty()) continue;
    const auto& inc = incs->back();
    const bool fresh = !newest_before || inc.id > *newest_before;
    std::cout << "drill: recorded. The exit is event evidence in incident " << inc.id
              << (fresh ? "" : " (an incident was already open, so the drill joined it)") << ".\n\n"
              << "  " << std::left << std::setw(20) << ("hm explain " + std::to_string(inc.id)) << "diagnosis in the terminal\n"
              << "  " << std::setw(20) << "hm report" << "every incident plus coverage, as one HTML file to open or share\n";
    return 0;
  }
  return fail("the agent did not record the drill within 30 s. Is hmd running and at least v0.2.0? "
              "Check: systemctl status hmd; hm doctor");
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
      {"--incident", true}, {"-o", true},
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
      std::cout << "hm " HM_VERSION "\n";
      return 0;
    } else if (s.rfind("--", 0) == 0 || s == "-o") {
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
  if (a.cmd == "report") return cmd_report(a);
  if (a.cmd == "drill") return cmd_drill(a);
  std::cerr << kUsage;
  return 2;
}
