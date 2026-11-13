#include "collectors/process_watch.hpp"

#include <dirent.h>

#include <charconv>
#include <fstream>
#include <sstream>

namespace hm {

namespace {

std::optional<std::string> slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::optional<int> as_pid(const char* s) {
  int v = 0;
  const std::string_view sv(s);
  auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
  if (ec != std::errc{} || p != sv.data() + sv.size() || v <= 0) return std::nullopt;
  return v;
}

std::int64_t oom_kill_count(const std::string& events_text) {
  std::istringstream in(events_text);
  std::string key;
  std::int64_t v = 0;
  while (in >> key >> v) {
    if (key == "oom_kill") return v;
  }
  return -1;
}

}  // namespace

void ProcessWatch::scan_once() {
  std::map<std::string, std::set<int>> found;
  std::map<std::string, std::string> cgroup_of;
  DIR* d = ::opendir(proc_.c_str());
  if (!d) {
    em_.status("process", "unavailable", "cannot open " + proc_);
    return;
  }
  while (dirent* ent = ::readdir(d)) {
    auto pid = as_pid(ent->d_name);
    if (!pid) continue;
    auto comm = slurp(proc_ + "/" + ent->d_name + "/comm");
    if (!comm) continue;  // raced with exit
    if (!comm->empty() && comm->back() == '\n') comm->pop_back();
    for (const auto& n : names_) {
      if (*comm != n) continue;
      found[n].insert(*pid);
      if (auto cg = slurp(proc_ + "/" + ent->d_name + "/cgroup")) {
        // cgroup v2 line: "0::/system.slice/foo.service"
        if (auto pos = cg->find("0::"); pos != std::string::npos) {
          auto path = cg->substr(pos + 3);
          if (auto nl = path.find('\n'); nl != std::string::npos) path.resize(nl);
          cgroup_of[n] = path;
        }
      }
    }
  }
  ::closedir(d);

  const ClockReading now = read_clocks();
  for (const auto& n : names_) {
    auto& w = state_[n];
    const auto& pids = found[n];
    if (!first_) {
      for (int old : w.pids) {
        if (pids.count(old)) continue;
        em_.emit(kind::process_exit, Severity::warning, n, {{"name", n}, {"pid", static_cast<std::int64_t>(old)}});
        w.last_exit_pid = old;
        w.last_exit_mono = now.boottime_ns;
      }
      for (int pid : pids) {
        if (w.pids.count(pid)) continue;
        const bool restart = w.last_exit_pid != 0 && now.boottime_ns - w.last_exit_mono < 60'000'000'000LL;
        if (restart) {
          em_.emit(kind::process_restart, Severity::warning, n,
                   {{"name", n}, {"old_pid", w.last_exit_pid}, {"new_pid", static_cast<std::int64_t>(pid)}});
          w.last_exit_pid = 0;
        } else {
          em_.emit(kind::process_start, Severity::info, n, {{"name", n}, {"pid", static_cast<std::int64_t>(pid)}});
        }
      }
    } else {
      for (int pid : pids) {
        em_.emit(kind::process_start, Severity::info, n, {{"name", n}, {"pid", static_cast<std::int64_t>(pid)}, {"initial", true}});
      }
    }
    w.pids = pids;
    if (auto it = cgroup_of.find(n); it != cgroup_of.end()) w.cgroup = it->second;
    if (!w.cgroup.empty()) {
      if (auto ev = slurp(cg_root_ + w.cgroup + "/memory.events")) {
        const auto count = oom_kill_count(*ev);
        if (w.oom_kills >= 0 && count > w.oom_kills) {
          em_.emit(kind::cgroup_oom_kill, Severity::error, n,
                   {{"name", n}, {"cgroup", w.cgroup}, {"delta", count - w.oom_kills}});
        }
        if (count >= 0) w.oom_kills = count;
      }
    }
  }
  if (first_) {
    em_.status("process", names_.empty() ? "unavailable" : "active",
               names_.empty() ? "no processes configured in [watch] processes" : "");
  }
  first_ = false;
}

void ProcessWatch::run(std::stop_token stop) {
  if (names_.empty()) {
    em_.status("process", "unavailable", "no processes configured in [watch] processes");
    return;
  }
  do {
    scan_once();
  } while (sleep_for(stop, std::chrono::seconds(1)));
}

}  // namespace hm
