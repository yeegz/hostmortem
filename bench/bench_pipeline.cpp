// Pipeline throughput and event-to-commit latency: N producer threads push
// synthetic events through the real EventQueue and Writer into a real SQLite
// store on disk. Prints one CSV row per run; run_bench.sh repeats and
// summarises. No number without its script (blueprint section 14).
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "agent/writer.hpp"
#include "core/kinds.hpp"

using namespace hm;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
  const int producers = argc > 1 ? std::atoi(argv[1]) : 4;
  const int rate_per_producer = argc > 2 ? std::atoi(argv[2]) : 5000;  // events/s each
  const int seconds = argc > 3 ? std::atoi(argv[3]) : 5;
  const std::string dir = argc > 4 ? argv[4] : "/tmp";
  const std::string path = dir + "/hm-bench-" + std::to_string(::getpid()) + ".db";

  auto store = Store::open(path, Store::Mode::read_write);
  if (!store) {
    std::fprintf(stderr, "open: %s\n", store.error().message.c_str());
    return 1;
  }
  (void)(*store)->register_boot("bench", 0, "bench");
  EventQueue q(16384, 4096);
  Emitter self(q, "hmd", "bench", 0);
  Detector det(DetectorConfig{});
  WriterConfig wc;  // production defaults: 500-event batches, 5 s interval
  Writer writer(**store, q, det, self, 1, wc);
  std::thread wt([&] { writer.run(); });

  std::atomic<std::uint64_t> pushed{0}, dropped{0};
  const auto t0 = Clock::now();
  std::vector<std::thread> ps;
  for (int p = 0; p < producers; ++p) {
    ps.emplace_back([&, p] {
      Emitter em(q, "bench" + std::to_string(p), "bench", 0);
      const auto period = std::chrono::nanoseconds(1'000'000'000LL / rate_per_producer);
      auto next = Clock::now();
      const auto end = t0 + std::chrono::seconds(seconds);
      std::uint64_t n = 0;
      while (Clock::now() < end) {
        auto r = em.emit(kind::net_tcp, Severity::debug, "tcp",
                         {{"out_segs", std::int64_t{1000}}, {"retrans_ppm", 1.5}, {"n", static_cast<std::int64_t>(n++)}});
        if (r == EventQueue::Push::dropped) dropped.fetch_add(1, std::memory_order_relaxed);
        else pushed.fetch_add(1, std::memory_order_relaxed);
        next += period;
        std::this_thread::sleep_until(next);
      }
    });
  }
  for (auto& t : ps) t.join();
  const auto produce_end = Clock::now();
  q.close();
  wt.join();
  const auto drain_end = Clock::now();

  // Event-to-commit latency is bounded by the commit policy; measure the
  // drain tail separately, and the store's view of what landed.
  const auto stats = writer.stats();
  const double secs = std::chrono::duration<double>(produce_end - t0).count();
  const double drain_ms = std::chrono::duration<double, std::milli>(drain_end - produce_end).count();
  const auto bytes = (*store)->used_bytes();
  std::printf("producers,offered_eps,achieved_eps,pushed,dropped,committed,commits,drain_ms,db_kib\n");
  std::printf("%d,%d,%.0f,%llu,%llu,%llu,%llu,%.1f,%lld\n", producers, producers * rate_per_producer,
              static_cast<double>(pushed.load()) / secs, static_cast<unsigned long long>(pushed.load()),
              static_cast<unsigned long long>(dropped.load()), static_cast<unsigned long long>(stats.committed),
              static_cast<unsigned long long>(stats.commits), drain_ms,
              static_cast<long long>(bytes ? *bytes / 1024 : -1));
  store->reset();
  std::error_code ec;
  for (const char* suf : {"", "-wal", "-shm"}) std::filesystem::remove(path + suf, ec);
  return stats.committed + dropped.load() >= pushed.load() ? 0 : 2;
}
