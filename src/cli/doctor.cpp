#include "cli/doctor.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "analysis/correlator.hpp"
#include "core/clock.hpp"
#include "core/kinds.hpp"
#include "store/store.hpp"

namespace hm {

namespace {

struct Check {
  std::string name;
  std::string status;  // ok | missing | info
  std::string detail;
  std::string cost;    // what is lost when missing
};

std::uint64_t cap_eff() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("CapEff:", 0) == 0) return std::stoull(line.substr(7), nullptr, 16);
  }
  return 0;
}

bool has_cap(std::uint64_t caps, int bit) { return (caps >> bit) & 1ULL; }

std::string kernel_major_minor_check(const std::string& rel, int& major, int& minor) {
  major = minor = 0;
  std::sscanf(rel.c_str(), "%d.%d", &major, &minor);
  return rel;
}

}  // namespace

int run_doctor(const std::string& db_path, bool json) {
  std::vector<Check> checks;
  int major = 0, minor = 0;
  const std::string rel = kernel_major_minor_check(kernel_release(), major, minor);
  checks.push_back({"kernel", "info", rel, ""});
  const bool ringbuf = major > 5 || (major == 5 && minor >= 8);
  checks.push_back({"bpf ring buffer (5.8+)", ringbuf ? "ok" : "missing", ringbuf ? "supported" : "kernel older than 5.8",
                    "v0.2 eBPF probes would fall back to perf buffers"});
  const bool btf = std::filesystem::exists("/sys/kernel/btf/vmlinux");
  checks.push_back({"BTF (/sys/kernel/btf/vmlinux)", btf ? "ok" : "missing", btf ? "present" : "absent",
                    "v0.2 CO-RE probes cannot load; counters only"});
  const bool psi = std::filesystem::exists("/proc/pressure/cpu");
  checks.push_back({"PSI (/proc/pressure)", psi ? "ok" : "missing", psi ? "present" : "absent",
                    "CPU/memory/IO starvation cannot be measured"});
  const bool ct = std::filesystem::exists("/proc/net/stat/nf_conntrack");
  checks.push_back({"conntrack stats", ct ? "ok" : "missing", ct ? "present" : "nf_conntrack not loaded here",
                    "conntrack insert races and table-full drops are invisible"});
  const bool cg2 = std::filesystem::exists("/sys/fs/cgroup/cgroup.controllers");
  checks.push_back({"cgroup v2", cg2 ? "ok" : "missing", cg2 ? "unified hierarchy" : "not mounted",
                    "cgroup OOM kills of watched services cannot be read"});

  {
    const int fd = ::open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    checks.push_back({"/dev/kmsg readable by this user", fd >= 0 ? "ok" : "missing", fd >= 0 ? "yes" : "no (CAP_SYSLOG or dmesg_restrict=0)",
                      "OOM kills, NIC watchdog and link messages are invisible; OOM can never be ruled out"});
    if (fd >= 0) ::close(fd);
  }
  {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
    const int raw = fd >= 0 ? -1 : ::socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP);
    const bool any = fd >= 0 || raw >= 0;
    checks.push_back({"ICMP probes", any ? "ok" : "missing",
                      fd >= 0 ? "unprivileged ping socket" : raw >= 0 ? "raw socket (CAP_NET_RAW)" : "neither ping socket nor CAP_NET_RAW",
                      "gateway reachability unknown, so link faults cannot be separated from upstream faults"});
    if (fd >= 0) ::close(fd);
    if (raw >= 0) ::close(raw);
  }
  const auto caps = cap_eff();
  std::ostringstream capstr;
  capstr << (::geteuid() == 0 ? "running as root" : "uid " + std::to_string(::geteuid()));
  for (auto [bit, name] : std::vector<std::pair<int, const char*>>{{12, "NET_ADMIN"}, {13, "NET_RAW"}, {34, "SYSLOG"}, {38, "PERFMON"}, {39, "BPF"}}) {
    capstr << ' ' << (has_cap(caps, bit) ? '+' : '-') << name;
  }
  checks.push_back({"capabilities (this process)", "info", capstr.str(), ""});

  nlohmann::json store_j;
  std::vector<std::pair<std::string, std::pair<std::string, std::string>>> collector_rows;
  if (std::filesystem::exists(db_path)) {
    auto st = Store::open(db_path, Store::Mode::read_only);
    if (!st) {
      checks.push_back({"store", "missing", st.error().message, "no history available"});
    } else {
      auto& s = **st;
      auto qc = s.quick_check();
      auto count = s.event_count();
      auto bytes = s.used_bytes();
      auto synthetic = s.meta("synthetic");
      const bool synth = synthetic && *synthetic && **synthetic == "1";
      checks.push_back({"store", qc ? "ok" : "missing",
                        db_path + " schema v" + std::to_string(s.schema_version()) + ", " +
                            std::to_string(count ? *count : -1) + " events, " +
                            std::to_string(bytes ? *bytes / 1024 : -1) + " KiB" + (synth ? ", SYNTHETIC" : "") +
                            (qc ? ", quick_check ok" : ", quick_check FAILED"),
                        "store damaged"});
      auto boots = s.boots();
      if (boots && !boots->empty()) {
        EventQuery q;
        q.from = TimePoint{boots->back().ordinal, boots->back().boot_id, 0};
        q.kind_prefix = kind::collector_status;
        if (auto ev = s.query(q)) {
          const auto avail = availability_from(*ev);
          for (const auto& [name, e] : avail.collectors) {
            const char* state = e.state == CollectorState::active ? "active"
                                : e.state == CollectorState::degraded ? "degraded"
                                : e.state == CollectorState::unavailable ? "unavailable" : "unknown";
            collector_rows.push_back({name, {state, e.reason}});
          }
        }
      }
    }
  } else {
    checks.push_back({"store", "info", db_path + " does not exist yet (start hmd)", ""});
  }

  if (json) {
    nlohmann::json j;
    for (const auto& c : checks) j["host"].push_back({{"check", c.name}, {"status", c.status}, {"detail", c.detail}, {"cost_if_missing", c.cost}});
    for (const auto& [n, sr] : collector_rows) j["collectors"].push_back({{"collector", n}, {"state", sr.first}, {"reason", sr.second}});
    std::cout << j.dump(2) << "\n";
    return 0;
  }
  std::cout << "Host checks (what this machine lets Hostmortem see):\n";
  for (const auto& c : checks) {
    const char* mark = c.status == "ok" ? "[ok]     " : c.status == "missing" ? "[missing]" : "[info]   ";
    std::cout << "  " << mark << ' ' << c.name << ": " << c.detail << '\n';
    if (c.status == "missing" && !c.cost.empty()) std::cout << "             cost: " << c.cost << '\n';
  }
  if (!collector_rows.empty()) {
    std::cout << "\nCollectors as last reported by hmd (latest boot):\n";
    for (const auto& [n, sr] : collector_rows) {
      std::cout << "  " << n << ": " << sr.first;
      if (!sr.second.empty()) std::cout << " - " << sr.second;
      std::cout << '\n';
    }
  }
  std::cout << "\nNote: host checks run as the user invoking `hm doctor`; hmd may hold different capabilities.\n";
  return 0;
}

}  // namespace hm
