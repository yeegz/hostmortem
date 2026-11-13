#include <gtest/gtest.h>

#include "../unit/test_util.hpp"
#include "agent/writer.hpp"
#include "core/kinds.hpp"

using namespace hm;
using namespace std::chrono_literals;

TEST(Writer, CommitsDrainsAndRecordsDropsAsEvents) {
  test::TempDir d;
  auto store = std::move(Store::open(d.file("w.db"), Store::Mode::read_write)).value();
  EventQueue q(4, 2);
  Emitter self(q, "hmd", "boot", 0);
  Emitter src(q, "procfs", "boot", 0);
  Detector det(DetectorConfig{});
  WriterConfig cfg;
  cfg.commit_interval = 50ms;
  Writer w(*store, q, det, self, 1, cfg);
  ASSERT_TRUE(store->register_boot("boot", 0, "test").ok());

  for (int i = 0; i < 10; ++i) (void)src.emit(kind::sys_cpu, Severity::debug, "cpu", {{"busy_pct", 1.0}});
  q.close();
  w.run();

  EventQuery qd;
  qd.kind_prefix = kind::pipeline_drop;
  auto drops = store->query(qd);
  ASSERT_TRUE(drops.ok());
  ASSERT_EQ(drops->size(), 1u);
  EXPECT_EQ((*drops)[0].event.attr_int("count"), 6) << "4 accepted, 6 dropped and the loss is itself recorded";
  EXPECT_EQ(*store->event_count(), 5);
}

TEST(Writer, OpensIncidentFromCommittedEvents) {
  test::TempDir d;
  auto store = std::move(Store::open(d.file("w.db"), Store::Mode::read_write)).value();
  EventQueue q(100, 100);
  Emitter self(q, "hmd", "boot", 0);
  Emitter src(q, "netlink", "boot", 0);
  Detector det(DetectorConfig{});
  Writer w(*store, q, det, self, 1, WriterConfig{});
  ASSERT_TRUE(store->register_boot("boot", 0, "test").ok());
  (void)src.emit(kind::link_state, Severity::warning, "eth0",
                 {{"ifname", std::string("eth0")}, {"oper", std::string("down")}, {"carrier", false}});
  q.close();
  w.run();
  auto incs = store->incidents(false);
  ASSERT_TRUE(incs.ok());
  ASSERT_EQ(incs->size(), 1u);
  EXPECT_EQ((*incs)[0].trigger, "link_down");
  EXPECT_NE((*incs)[0].trigger_event_id, 0);
}

// Disk full: commits fail, pending is bounded by shedding low-priority events,
// high-priority evidence is kept, and once space returns everything left is
// committed together with a pipeline.drop record of what was shed.
TEST(Writer, DiskFullShedsLowPriorityAndRecovers) {
  test::TempDir d;
  auto store = std::move(Store::open(d.file("w.db"), Store::Mode::read_write)).value();
  ASSERT_TRUE(store->register_boot("boot", 0, "test").ok());
  EventQueue q(10, 10);
  Emitter self(q, "hmd", "boot", 0);
  Emitter src(q, "x", "boot", 0);
  Detector det(DetectorConfig{});
  WriterConfig cfg;
  cfg.max_pending = 50;
  Writer w(*store, q, det, self, 1, cfg);

  auto pages = sql::Stmt::prepare(store->raw(), "PRAGMA page_count");
  ASSERT_TRUE(pages.ok() && pages->step().ok());
  ASSERT_TRUE(store->set_max_pages(pages->col_int(0)).ok());

  for (int i = 0; i < 200; ++i) {
    w.pending().push_back(src.make(kind::sys_cpu, Severity::debug, "cpu", {{"pad", std::string(500, 'z')}}));
  }
  w.pending().push_back(src.make(kind::kernel_oom, Severity::error, "app", {{"pid", std::int64_t{1}}}));
  EXPECT_FALSE(w.commit());
  EXPECT_LE(w.pending().size(), 50u);
  bool kept_oom = false;
  for (const auto& e : w.pending()) kept_oom |= e.kind == kind::kernel_oom;
  EXPECT_TRUE(kept_oom) << "high-priority evidence must survive shedding";
  EXPECT_GT(w.stats().shed, 0u);

  ASSERT_TRUE(store->set_max_pages(1'000'000).ok());
  EXPECT_TRUE(w.commit());
  EventQuery qd;
  qd.kind_prefix = kind::pipeline_drop;
  auto drops = store->query(qd);
  ASSERT_TRUE(drops.ok());
  ASSERT_FALSE(drops->empty());
  EXPECT_EQ((*drops)[0].event.attr_str("reason"), "store unavailable");
}
