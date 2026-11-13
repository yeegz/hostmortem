#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>

#include "store/store.hpp"
#include "test_util.hpp"

using namespace hm;
using hm::test::make_event;
using hm::test::TempDir;

namespace {
std::unique_ptr<Store> open_rw(const std::string& p) {
  auto s = Store::open(p, Store::Mode::read_write);
  EXPECT_TRUE(s.ok()) << (s.ok() ? "" : s.error().message);
  return std::move(s).value();
}
}  // namespace

TEST(Store, CreatesSchemaInWalMode) {
  TempDir d;
  auto s = open_rw(d.file("a.db"));
  EXPECT_EQ(s->schema_version(), 1);
  auto st = sql::Stmt::prepare(s->raw(), "PRAGMA journal_mode");
  ASSERT_TRUE(st.ok());
  ASSERT_TRUE(st->step().ok());
  EXPECT_EQ(st->col_text(0), "wal");
}

TEST(Store, InsertAssignsIdsAndReplayIsIdempotent) {
  TempDir d;
  auto s = open_rw(d.file("a.db"));
  std::vector<Event> batch = {make_event("b1", "prober", 1, 10, "probe.icmp", {{"ok", true}}),
                              make_event("b1", "prober", 2, 11, "probe.icmp", {{"ok", false}})};
  auto r = s->insert_batch(batch);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->inserted, 2u);
  EXPECT_GT(batch[0].id, 0);
  EXPECT_GT(batch[1].id, batch[0].id);

  // Replay the same batch: the duplicate-events scenario. Row count unchanged.
  std::vector<Event> replay = {make_event("b1", "prober", 1, 10, "probe.icmp", {{"ok", true}}),
                               make_event("b1", "prober", 2, 11, "probe.icmp", {{"ok", false}}),
                               make_event("b1", "prober", 3, 12, "probe.icmp", {{"ok", true}})};
  r = s->insert_batch(replay);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->inserted, 1u);
  EXPECT_EQ(r->duplicates, 2u);
  EXPECT_EQ(replay[0].id, 0);
  EXPECT_EQ(*s->event_count(), 3);
}

TEST(Store, AttrsRoundTripWithTypes) {
  TempDir d;
  auto s = open_rw(d.file("a.db"));
  std::vector<Event> b = {make_event("b1", "x", 1, 1, "k",
                                     {{"i", std::int64_t{-5}}, {"d", 2.5}, {"t", true}, {"s", std::string("hi")}})};
  ASSERT_TRUE(s->insert_batch(b).ok());
  auto got = s->event_by_id(b[0].id);
  ASSERT_TRUE(got.ok() && got->has_value());
  const Event& e = (*got)->event;
  EXPECT_EQ(e.attr_int("i"), -5);
  EXPECT_DOUBLE_EQ(*e.attr_num("d"), 2.5);
  EXPECT_EQ(e.attr_bool("t"), true);
  EXPECT_EQ(e.attr_str("s"), "hi");
}

TEST(Store, MaxSeqBySourceContinuesNumbering) {
  TempDir d;
  auto s = open_rw(d.file("a.db"));
  std::vector<Event> b = {make_event("b1", "a", 7, 1, "k"), make_event("b1", "a", 9, 2, "k"),
                          make_event("b1", "b", 3, 3, "k"), make_event("b2", "a", 100, 1, "k")};
  ASSERT_TRUE(s->insert_batch(b).ok());
  auto m = s->max_seq_by_source("b1");
  ASSERT_TRUE(m.ok());
  EXPECT_EQ((*m)["a"], 9u);
  EXPECT_EQ((*m)["b"], 3u);
}

// Cross-boot ordering uses the boot ordinal, never wall time: boot 2 comes
// after boot 1 even if its wall clock is set to 1970.
TEST(Store, OrdersByBootOrdinalThenMonotonic) {
  TempDir d;
  auto s = open_rw(d.file("a.db"));
  ASSERT_TRUE(s->register_boot("first", 1'790'000'000'000'000'000LL, "6.8").ok());
  ASSERT_TRUE(s->register_boot("second", 0, "6.8").ok());
  auto late = make_event("second", "a", 1, 1, "k");
  late.wall_ns = 0;
  late.wall_quality = WallQuality::unsynced;
  std::vector<Event> b = {late, make_event("first", "a", 1, 500, "k"), make_event("first", "a", 2, 20, "k")};
  ASSERT_TRUE(s->insert_batch(b).ok());
  auto all = s->query({});
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all->size(), 3u);
  EXPECT_EQ((*all)[0].event.boot_id, "first");
  EXPECT_EQ((*all)[0].event.mono_ns, 20'000'000'000LL);
  EXPECT_EQ((*all)[1].event.mono_ns, 500'000'000'000LL);
  EXPECT_EQ((*all)[2].event.boot_id, "second");
  EXPECT_EQ((*all)[2].boot_ordinal, 2);
}

TEST(Store, CorruptFileIsSetAsideAndRecreated) {
  TempDir d;
  const auto p = d.file("bad.db");
  {
    std::ofstream f(p, std::ios::binary);
    f << "SQLite format 3";  // a header prefix followed by garbage
    for (int i = 0; i < 4096; ++i) f.put(static_cast<char>(i * 7));
  }
  auto s = Store::open(p, Store::Mode::read_write);
  ASSERT_TRUE(s.ok()) << s.error().message;
  ASSERT_TRUE((*s)->recovered_from().has_value());
  EXPECT_TRUE(std::filesystem::exists(*(*s)->recovered_from()));
  EXPECT_EQ((*s)->schema_version(), 1);
  EXPECT_TRUE((*s)->quick_check().ok());
}

// Disk exhaustion: SQLITE_FULL fails the whole batch atomically and the store
// remains usable once space returns.
TEST(Store, DiskFullFailsBatchAtomically) {
  TempDir d;
  auto s = open_rw(d.file("full.db"));
  std::vector<Event> warm = {make_event("b1", "a", 1, 1, "k")};
  ASSERT_TRUE(s->insert_batch(warm).ok());
  auto pages = sql::Stmt::prepare(s->raw(), "PRAGMA page_count");
  ASSERT_TRUE(pages.ok() && pages->step().ok());
  ASSERT_TRUE(s->set_max_pages(pages->col_int(0)).ok());

  std::vector<Event> big;
  for (std::uint64_t i = 2; i < 2000; ++i) {
    big.push_back(make_event("b1", "a", i, static_cast<std::int64_t>(i), "k", {{"pad", std::string(200, 'x')}}));
  }
  auto r = s->insert_batch(big);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, "sqlite_full");
  EXPECT_EQ(*s->event_count(), 1) << "no partial batch";
  for (const auto& e : big) EXPECT_EQ(e.id, 0);

  ASSERT_TRUE(s->set_max_pages(1'000'000).ok());
  r = s->insert_batch(big);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*s->event_count(), 1999);  // 1 warm-up + 1998 in the batch
}

TEST(Store, RetentionDeletesOldestFirstAndKeepsEvidence) {
  TempDir d;
  auto s = open_rw(d.file("ret.db"));
  std::vector<Event> b;
  for (std::uint64_t i = 1; i <= 3000; ++i) {
    b.push_back(make_event("b1", "a", i, static_cast<std::int64_t>(i), "k", {{"pad", std::string(300, 'y')}}));
  }
  ASSERT_TRUE(s->insert_batch(b).ok());
  // Evidence references the very first event: retention must keep it.
  Incident inc;
  inc.opened = {1, "b1", 5'000'000'000LL};
  inc.trigger = "test";
  auto iid = s->open_incident(inc);
  ASSERT_TRUE(iid.ok());
  Hypothesis h{"pb", "t", Verdict::supported, 5, "#1", {{b[0].id, EvidenceRole::for_, "kept"}}, {}};
  ASSERT_TRUE(s->replace_hypotheses(*iid, {h}).ok());

  const auto before = *s->used_bytes();
  auto n = s->apply_retention(0, 14, before / 2);
  ASSERT_TRUE(n.ok());
  EXPECT_GT(*n, 0);
  EXPECT_LE(*s->used_bytes(), before / 2);
  EXPECT_TRUE(s->event_by_id(b[0].id)->has_value()) << "evidence-linked event survives";
  EXPECT_FALSE(s->event_by_id(b[1].id)->has_value()) << "oldest unlinked event goes first";
  EXPECT_TRUE(s->event_by_id(b.back().id)->has_value()) << "newest event survives";
}

TEST(Store, AgeRetentionIgnoresUnsyncedWallTime) {
  TempDir d;
  auto s = open_rw(d.file("age.db"));
  auto old_synced = make_event("b1", "a", 1, 1, "k");
  old_synced.wall_ns = 1'000'000'000LL;  // 1970: very old
  auto old_unsynced = make_event("b1", "a", 2, 2, "k");
  old_unsynced.wall_ns = 1'000'000'000LL;
  old_unsynced.wall_quality = WallQuality::unsynced;
  std::vector<Event> b = {old_synced, old_unsynced};
  ASSERT_TRUE(s->insert_batch(b).ok());
  auto n = s->apply_retention(1'790'000'000'000'000'000LL, 14, 1LL << 40);
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(*n, 1);
  EXPECT_FALSE(s->event_by_id(b[0].id)->has_value());
  EXPECT_TRUE(s->event_by_id(b[1].id)->has_value()) << "an unsynchronised clock must not trigger deletion";
}

// Kill a writer mid-transaction (SIGKILL-equivalent _exit) and verify that
// committed batches survive, the in-flight batch vanishes, and the file passes
// quick_check: the "device reboot" scenario at process level.
TEST(Store, CrashMidTransactionLeavesConsistentStore) {
  TempDir d;
  const auto p = d.file("crash.db");
  { auto s = open_rw(p); }
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto s = Store::open(p, Store::Mode::read_write);
    if (!s) ::_exit(10);
    std::vector<Event> committed;
    for (std::uint64_t i = 1; i <= 100; ++i) committed.push_back(make_event("b1", "a", i, static_cast<std::int64_t>(i), "k"));
    if (!(*s)->insert_batch(committed)) ::_exit(11);
    sqlite3_exec((*s)->raw(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_exec((*s)->raw(), "INSERT INTO events(boot_id,source,seq,mono_ns,wall_ns,wall_quality,kind,severity) "
                               "VALUES('b1','a',999,1,1,2,'inflight',1)", nullptr, nullptr, nullptr);
    ::_exit(0);  // no COMMIT, no close: like a power cut for this process
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  auto s = open_rw(p);
  EXPECT_FALSE(s->recovered_from().has_value());
  EXPECT_TRUE(s->quick_check().ok());
  EXPECT_EQ(*s->event_count(), 100);
}

TEST(Store, ReadOnlyOpenOfMissingFileFails) {
  TempDir d;
  auto s = Store::open(d.file("nope.db"), Store::Mode::read_only);
  EXPECT_FALSE(s.ok());
}

TEST(Store, NameHashKeyIsStable) {
  TempDir d;
  const auto p = d.file("k.db");
  SipKey k1, k2;
  {
    auto s = open_rw(p);
    k1 = *s->name_hash_key();
  }
  {
    auto s = open_rw(p);
    k2 = *s->name_hash_key();
  }
  EXPECT_EQ(k1, k2);
}
