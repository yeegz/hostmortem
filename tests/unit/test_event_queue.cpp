#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

#include "pipeline/event_queue.hpp"

using namespace hm;
using namespace std::chrono_literals;

namespace {
Event ev(const std::string& src, std::uint64_t seq, Severity sev = Severity::info) {
  Event e;
  e.source = src;
  e.seq = seq;
  e.severity = sev;
  e.kind = "test";
  return e;
}
auto soon() { return std::chrono::steady_clock::now() + 50ms; }
}  // namespace

TEST(EventQueue, DropsAndCountsWhenLaneFull) {
  EventQueue q(2, 1);
  EXPECT_EQ(q.push(ev("a", 1)), EventQueue::Push::accepted);
  EXPECT_EQ(q.push(ev("a", 2)), EventQueue::Push::pressure);  // 2/2 >= 80 %
  EXPECT_EQ(q.push(ev("a", 3)), EventQueue::Push::dropped);
  EXPECT_EQ(q.push(ev("b", 1)), EventQueue::Push::dropped);
  auto drops = q.take_drops();
  ASSERT_EQ(drops.size(), 2u);
  EXPECT_EQ(drops[0].source, "a");
  EXPECT_EQ(drops[0].count, 1u);
  EXPECT_FALSE(drops[0].high_lane);
  EXPECT_TRUE(q.take_drops().empty()) << "take_drops resets the counters";
}

TEST(EventQueue, HighLaneSurvivesLowLaneFlood) {
  EventQueue q(4, 4);
  for (int i = 0; i < 100; ++i) (void)q.push(ev("sampler", static_cast<std::uint64_t>(i), Severity::debug));
  EXPECT_NE(q.push(ev("kmsg", 1, Severity::error)), EventQueue::Push::dropped);
  std::vector<Event> out;
  ASSERT_TRUE(q.pop_batch(out, 1, soon()));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].source, "kmsg") << "high lane is drained first";
}

TEST(EventQueue, CloseDrainsThenReportsEnd) {
  EventQueue q(8, 8);
  (void)q.push(ev("a", 1));
  q.close();
  EXPECT_EQ(q.push(ev("a", 2)), EventQueue::Push::closed);
  std::vector<Event> out;
  EXPECT_TRUE(q.pop_batch(out, 10, soon()));
  EXPECT_EQ(out.size(), 1u);
  out.clear();
  EXPECT_FALSE(q.pop_batch(out, 10, soon()));
}

TEST(EventQueue, PopTimesOutWhenEmpty) {
  EventQueue q(8, 8);
  std::vector<Event> out;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(q.pop_batch(out, 10, t0 + 30ms));
  EXPECT_TRUE(out.empty());
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 25ms);
}

// Invariant: every pushed event is either popped exactly once or counted as
// dropped; nothing is lost silently and nothing is duplicated. Run under
// TSan in CI (preset tsan).
TEST(EventQueue, ConcurrentProducersAccountForEveryEvent) {
  constexpr int kProducers = 6;
  constexpr int kPerProducer = 20000;
  EventQueue q(512, 128);
  std::atomic<bool> done{false};
  std::set<std::pair<std::string, std::uint64_t>> seen;
  std::size_t popped = 0;
  std::thread consumer([&] {
    std::vector<Event> out;
    for (;;) {
      out.clear();
      const bool more = q.pop_batch(out, 256, std::chrono::steady_clock::now() + 5ms);
      for (auto& e : out) {
        EXPECT_TRUE(seen.insert({e.source, e.seq}).second) << "duplicate delivery";
        ++popped;
      }
      if (!more) break;
    }
  });
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      const std::string src = "p" + std::to_string(p);
      for (int i = 0; i < kPerProducer; ++i) {
        (void)q.push(ev(src, static_cast<std::uint64_t>(i), i % 10 == 0 ? Severity::warning : Severity::debug));
      }
    });
  }
  for (auto& t : producers) t.join();
  q.close();
  consumer.join();
  std::uint64_t dropped = 0;
  for (const auto& d : q.take_drops()) dropped += d.count;
  EXPECT_EQ(popped + dropped, static_cast<std::uint64_t>(kProducers) * kPerProducer);
  EXPECT_EQ(q.stats().dropped, dropped);
}
