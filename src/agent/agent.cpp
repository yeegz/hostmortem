#include "agent/agent.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "agent/writer.hpp"
#include "collectors/kmsg_collector.hpp"
#include "collectors/netlink_collector.hpp"
#include "collectors/procfs_sampler.hpp"
#include "collectors/process_watch.hpp"
#include "collectors/prober.hpp"
#include "core/log.hpp"
#include "core/unique_fd.hpp"

namespace hm {

namespace {
constexpr const char* kVersion = HM_VERSION;
}

int run_agent(const AgentOptions& opts) {
  const Config& cfg = opts.config;
  // The store reveals topology and failure history: never world-readable,
  // even when started by hand rather than by the systemd unit (UMask=0027).
  ::umask(027);

  // Block termination signals in every thread (threads inherit this mask) and
  // receive them synchronously on a signalfd: no work happens in a signal
  // handler, so there is nothing async-signal-unsafe to get wrong.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGHUP);
  if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    log::error("agent", "pthread_sigmask failed");
    return 1;
  }
  UniqueFd sfd(::signalfd(-1, &mask, SFD_CLOEXEC));
  if (!sfd.valid()) {
    log::error("agent", "signalfd failed");
    return 1;
  }

  auto boot_id = read_boot_id();
  if (!boot_id) {
    log::error("agent", boot_id.error().message);
    return 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(cfg.db_path).parent_path(), ec);
  auto store_r = Store::open(cfg.db_path, Store::Mode::read_write);
  if (!store_r) {
    log::error("agent", "cannot open store " + cfg.db_path + ": " + store_r.error().message);
    return 1;
  }
  std::unique_ptr<Store> store = std::move(store_r).value();
  if (auto synthetic = store->meta("synthetic"); synthetic && *synthetic && **synthetic == "1") {
    log::error("agent", "refusing to record into a store that contains synthetic (hm ingest) data");
    return 1;
  }
  (void)store->set_meta("live", "1");

  const ClockReading start_clock = read_clocks();
  auto prev_boot = store->meta("last_boot_id");
  auto ordinal = store->register_boot(*boot_id, start_clock.realtime_ns, kernel_release());
  if (!ordinal) {
    log::error("agent", "cannot register boot: " + ordinal.error().message);
    return 1;
  }
  (void)store->set_meta("last_boot_id", *boot_id);
  auto seqs = store->max_seq_by_source(*boot_id);
  if (!seqs) {
    log::error("agent", seqs.error().message);
    return 1;
  }
  auto key = store->name_hash_key();
  if (!key) {
    log::error("agent", key.error().message);
    return 1;
  }
  auto seq_for = [&](const std::string& src) -> std::uint64_t {
    auto it = seqs->find(src);
    return it == seqs->end() ? 0 : it->second;
  };

  EventQueue queue(16384, 4096);
  Emitter self(queue, "hmd", *boot_id, seq_for("hmd"));
  Emitter procfs_em(queue, "procfs", *boot_id, seq_for("procfs"));
  Emitter netlink_em(queue, "netlink", *boot_id, seq_for("netlink"));
  Emitter kmsg_em(queue, "kmsg", *boot_id, seq_for("kmsg"));
  Emitter kernel_em(queue, "kernel", *boot_id, 0);  // seq = kernel record seq
  Emitter probe_em(queue, "prober", *boot_id, seq_for("prober"));
  Emitter proc_em(queue, "process", *boot_id, seq_for("process"));

  Attrs start_attrs{{"version", std::string(kVersion)}, {"kernel", kernel_release()},
                    {"boot_ordinal", *ordinal}, {"pid", static_cast<std::int64_t>(::getpid())}};
  if (prev_boot && *prev_boot && **prev_boot != *boot_id) start_attrs["previous_boot"] = **prev_boot;
  const bool same_boot_restart = prev_boot && *prev_boot && **prev_boot == *boot_id;
  start_attrs["restart_same_boot"] = same_boot_restart;
  self.emit(kind::agent_start, Severity::notice, "hmd", std::move(start_attrs));
  if (store->recovered_from()) {
    self.emit(kind::store_recovered, Severity::error, "store",
              {{"aside_path", *store->recovered_from()}, {"reason", std::string("quick_check failed at startup")}});
  }

  DetectorConfig dcfg;
  dcfg.probe_fail_streak = cfg.probe_fail_streak;
  dcfg.quiet = cfg.incident_quiet;
  Detector detector(dcfg);
  if (auto open = store->incidents(true); open && !open->empty()) {
    detector.adopt_open(open->back());
    log::info("agent", "resuming open incident " + std::to_string(open->back().id));
  }

  WriterConfig wcfg;
  wcfg.commit_batch = cfg.commit_batch;
  wcfg.commit_interval = cfg.commit_interval;
  wcfg.retention_days = cfg.retention_days;
  wcfg.max_bytes = cfg.max_store_mb << 20;
  Writer writer(*store, queue, detector, self, *ordinal, wcfg);
  std::thread writer_thread([&] { writer.run(); });

  std::vector<std::unique_ptr<Collector>> collectors;
  collectors.push_back(std::make_unique<ProcfsSampler>(procfs_em, cfg.sample_interval));
  collectors.push_back(std::make_unique<NetlinkCollector>(netlink_em));
  if (opts.enable_kmsg) collectors.push_back(std::make_unique<KmsgCollector>(kmsg_em, kernel_em));
  else kmsg_em.status("kmsg", "unavailable", "disabled by --no-kmsg");
  collectors.push_back(std::make_unique<Prober>(probe_em, cfg, *key));
  collectors.push_back(std::make_unique<ProcessWatch>(proc_em, cfg.watch_processes));

  std::vector<std::jthread> threads;
  threads.reserve(collectors.size());
  for (auto& c : collectors) {
    Collector* raw = c.get();
    threads.emplace_back([raw](std::stop_token st) {
      try {
        raw->run(st);
      } catch (const std::exception& e) {
        // No exception crosses a thread boundary: report and let the others run.
        log::error(raw->name(), std::string("collector crashed: ") + e.what());
      }
    });
  }
  log::info("agent", "recording to " + cfg.db_path + " (boot " + std::to_string(*ordinal) + ")");

  // Wait for a signal or the run-for deadline.
  const auto deadline = opts.run_for ? std::chrono::steady_clock::now() + *opts.run_for
                                     : std::chrono::steady_clock::time_point::max();
  int exit_signal = 0;
  for (;;) {
    pollfd p{sfd.get(), POLLIN, 0};
    int timeout = 1000;
    if (opts.run_for) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) break;
      timeout = static_cast<int>(std::min<long long>(left, 1000));
    }
    if (::poll(&p, 1, timeout) > 0) {
      signalfd_siginfo si{};
      if (::read(sfd.get(), &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
        exit_signal = static_cast<int>(si.ssi_signo);
        break;
      }
    }
  }

  // Ordered shutdown: stop producers, record the stop, drain, commit, close.
  log::info("agent", "shutting down");
  for (auto& t : threads) t.request_stop();
  threads.clear();  // joins
  self.emit(kind::agent_stop, Severity::notice, "hmd",
            {{"signal", static_cast<std::int64_t>(exit_signal)}, {"reason", std::string(exit_signal ? "signal" : "run_for elapsed")}});
  queue.close();
  writer_thread.join();
  const auto st = writer.stats();
  const auto qs = queue.stats();
  log::info("agent", "committed=" + std::to_string(st.committed) + " duplicates=" + std::to_string(st.duplicates) +
                         " dropped=" + std::to_string(qs.dropped) + " incidents_opened=" + std::to_string(st.incidents_opened));
  return 0;
}

}  // namespace hm
