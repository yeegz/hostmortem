#pragma once
#include <map>
#include <set>

#include "collectors/collector.hpp"

namespace hm {

// Polls /proc for watched process names (comm) once a second and reports
// exits, starts and restarts; reads the process's cgroup v2 memory.events to
// catch cgroup OOM kills even when the kernel log is unreadable.
//
// Known limit (documented in COMPATIBILITY.md): a process that crashes and
// is restarted within one poll interval is seen only as a pid change, and its
// exit status is never visible. eBPF sched_process_exit (v0.2) closes this gap.
class ProcessWatch final : public Collector {
 public:
  ProcessWatch(Emitter& em, std::vector<std::string> names, std::string proc_root = "/proc",
               std::string cgroup_root = "/sys/fs/cgroup")
      : em_(em), names_(std::move(names)), proc_(std::move(proc_root)), cg_root_(std::move(cgroup_root)) {}
  std::string_view name() const override { return "process"; }
  void run(std::stop_token stop) override;
  void scan_once();

 private:
  struct Watched {
    std::set<int> pids;
    std::string cgroup;  // last cgroup path seen for this name
    std::int64_t oom_kills = -1;
    std::int64_t last_exit_pid = 0;
    std::int64_t last_exit_mono = 0;
  };
  Emitter& em_;
  std::vector<std::string> names_;
  std::string proc_;
  std::string cg_root_;
  std::map<std::string, Watched> state_;
  bool first_ = true;
};

}  // namespace hm
