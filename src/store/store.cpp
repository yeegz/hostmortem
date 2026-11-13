#include "store/store.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "core/json_codec.hpp"

namespace hm {

using sql::Stmt;

namespace {

// Numbered, append-only migrations. Never edit an applied migration; add one.
constexpr const char* kMigrations[] = {
    // 1: initial schema (blueprint section 12.5, attrs as JSON text)
    R"sql(
CREATE TABLE boots(
  ordinal INTEGER PRIMARY KEY AUTOINCREMENT,
  boot_id TEXT NOT NULL UNIQUE,
  first_seen_wall INTEGER NOT NULL,
  kernel TEXT NOT NULL);
CREATE TABLE events(
  id INTEGER PRIMARY KEY,
  boot_id TEXT NOT NULL REFERENCES boots(boot_id),
  source TEXT NOT NULL,
  seq INTEGER NOT NULL,
  mono_ns INTEGER NOT NULL,
  wall_ns INTEGER NOT NULL,
  wall_quality INTEGER NOT NULL,
  kind TEXT NOT NULL,
  severity INTEGER NOT NULL,
  subject TEXT NOT NULL DEFAULT '',
  attrs TEXT NOT NULL DEFAULT '{}',
  UNIQUE(boot_id, source, seq));
CREATE INDEX ev_time ON events(boot_id, mono_ns);
CREATE INDEX ev_kind ON events(kind, wall_ns);
CREATE TABLE incidents(
  id INTEGER PRIMARY KEY,
  opened_boot TEXT NOT NULL,
  opened_mono INTEGER NOT NULL,
  opened_wall INTEGER NOT NULL,
  closed_boot TEXT,
  closed_mono INTEGER,
  trigger TEXT NOT NULL,
  subject TEXT NOT NULL DEFAULT '',
  trigger_event INTEGER,
  status TEXT NOT NULL CHECK(status IN ('open','closed','interrupted')));
CREATE TABLE hypotheses(
  id INTEGER PRIMARY KEY,
  incident_id INTEGER NOT NULL REFERENCES incidents(id) ON DELETE CASCADE,
  playbook TEXT NOT NULL,
  title TEXT NOT NULL,
  verdict TEXT NOT NULL CHECK(verdict IN ('supported','possible','ruled_out')),
  score INTEGER NOT NULL,
  explanation TEXT NOT NULL,
  next_steps TEXT NOT NULL DEFAULT '[]');
CREATE TABLE evidence(
  hypothesis_id INTEGER NOT NULL REFERENCES hypotheses(id) ON DELETE CASCADE,
  event_id INTEGER,
  role TEXT NOT NULL CHECK(role IN ('for','against','missing')),
  note TEXT NOT NULL DEFAULT '');
CREATE INDEX evidence_event ON evidence(event_id);
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql",
};
constexpr int kLatestSchema = static_cast<int>(std::size(kMigrations));

Result<sql::DbPtr> open_raw(const std::string& path, Store::Mode mode) {
  sqlite3* raw = nullptr;
  const int flags = mode == Store::Mode::read_only
                        ? SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX
                        : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
  sql::DbPtr db(raw);  // owns the handle even when open failed
  if (rc != SQLITE_OK) return sql::sqlite_error(db.get(), rc, "open " + path);
  sqlite3_busy_timeout(db.get(), 2000);
  sqlite3_extended_result_codes(db.get(), 1);
  return db;
}

// Returns "ok" or the first quick_check complaint.
Result<std::string> run_quick_check(sqlite3* db) {
  auto st = Stmt::prepare(db, "PRAGMA quick_check");
  if (!st) return st.error();
  auto row = st->step();
  if (!row) return row.error();
  if (!*row) return std::string("no result");
  return st->col_text(0);
}

std::string status_str(IncidentStatus s) { return to_string(s); }

IncidentStatus status_from(const std::string& s) {
  if (s == "closed") return IncidentStatus::closed;
  if (s == "interrupted") return IncidentStatus::interrupted;
  return IncidentStatus::open;
}

constexpr const char* kEventCols =
    "e.id, e.boot_id, e.source, e.seq, e.mono_ns, e.wall_ns, e.wall_quality, e.kind, "
    "e.severity, e.subject, e.attrs, b.ordinal";

StoredEvent read_event_row(const Stmt& st) {
  StoredEvent se;
  Event& e = se.event;
  e.id = st.col_int(0);
  e.boot_id = st.col_text(1);
  e.source = st.col_text(2);
  e.seq = static_cast<std::uint64_t>(st.col_int(3));
  e.mono_ns = st.col_int(4);
  e.wall_ns = st.col_int(5);
  e.wall_quality = static_cast<WallQuality>(st.col_int(6));
  e.kind = st.col_text(7);
  e.severity = static_cast<Severity>(st.col_int(8));
  e.subject = st.col_text(9);
  if (auto a = attrs_from_json(st.col_text(10))) e.attrs = std::move(a).value();
  se.boot_ordinal = st.col_int(11);
  return se;
}

class Txn {
 public:
  explicit Txn(sqlite3* db) : db_(db) {}
  Status begin() {
    auto s = sql::exec(db_, "BEGIN IMMEDIATE");
    active_ = s.ok();
    return s;
  }
  Status commit() {
    auto s = sql::exec(db_, "COMMIT");
    if (s) active_ = false;
    return s;
  }
  ~Txn() {
    if (active_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  Txn(const Txn&) = delete;
  Txn& operator=(const Txn&) = delete;

 private:
  sqlite3* db_;
  bool active_ = false;
};

}  // namespace

const char* to_string(IncidentStatus s) noexcept {
  switch (s) {
    case IncidentStatus::open: return "open";
    case IncidentStatus::closed: return "closed";
    case IncidentStatus::interrupted: return "interrupted";
  }
  return "open";
}
const char* to_string(Verdict v) noexcept {
  switch (v) {
    case Verdict::supported: return "supported";
    case Verdict::possible: return "possible";
    case Verdict::ruled_out: return "ruled_out";
  }
  return "possible";
}
const char* to_string(EvidenceRole r) noexcept {
  switch (r) {
    case EvidenceRole::for_: return "for";
    case EvidenceRole::against: return "against";
    case EvidenceRole::missing: return "missing";
  }
  return "for";
}

Result<std::unique_ptr<Store>> Store::open(const std::string& path, Mode mode) {
  auto db = open_raw(path, mode);
  if (!db) return db.error();

  std::optional<std::string> recovered;
  auto check = run_quick_check(db->get());
  const bool bad = !check || *check != "ok";
  if (bad) {
    if (mode == Mode::read_only) {
      return make_error("sqlite_corrupt",
                        "store failed quick_check: " + (check ? *check : check.error().message));
    }
    db->reset();
    const std::string aside = path + ".corrupt-" + std::to_string(std::time(nullptr));
    std::error_code ec;
    std::filesystem::rename(path, aside, ec);
    if (ec) return make_error("recover_failed", "cannot set aside corrupt store: " + ec.message());
    for (const char* suffix : {"-wal", "-shm"}) {
      std::filesystem::rename(path + suffix, aside + suffix, ec);  // may not exist
    }
    recovered = aside;
    db = open_raw(path, mode);
    if (!db) return db.error();
  }

  std::unique_ptr<Store> store(new Store(std::move(db).value()));
  store->read_only_ = mode == Mode::read_only;
  store->recovered_from_ = std::move(recovered);
  sqlite3* raw = store->db_.get();

  if (!store->read_only_) {
    // auto_vacuum only takes effect before the first table exists.
    if (auto s = sql::exec(raw, "PRAGMA auto_vacuum=INCREMENTAL"); !s) return s.error();
    {
      // Statements must be finalized before COMMIT, hence the scope.
      auto wal = Stmt::prepare(raw, "PRAGMA journal_mode=WAL");
      if (!wal) return wal.error();
      if (auto r = wal->step(); !r) return r.error();
      if (wal->col_text(0) != "wal") return make_error("no_wal", "filesystem refused WAL mode");
    }
    // FULL, not NORMAL: in WAL mode NORMAL never fsyncs a commit (only
    // checkpoints), so a power cut or hard reset can lose far more than one
    // commit interval, even a whole new store. The nested-VM hard-reset test
    // (scripts/hard-reset-test.sh) lost all of boot 1 with NORMAL. Commits are
    // batched (5 s or 500 events), so FULL costs about one fsync per commit.
    if (auto s = sql::exec(raw, "PRAGMA synchronous=FULL"); !s) return s.error();
    if (auto s = store->migrate(); !s) return s.error();
  } else {
    auto v = Stmt::prepare(raw, "PRAGMA user_version");
    if (!v) return v.error();
    if (auto r = v->step(); !r) return r.error();
    store->schema_version_ = static_cast<int>(v->col_int(0));
    if (store->schema_version_ == 0) return make_error("empty_store", "store has no schema yet");
    if (store->schema_version_ > kLatestSchema) {
      return make_error("schema_too_new", "store schema is newer than this binary");
    }
  }
  if (auto s = sql::exec(raw, "PRAGMA foreign_keys=ON"); !s) return s.error();
  if (auto s = store->prepare_statements(); !s) return s.error();
  return store;
}

Store::~Store() = default;

Status Store::migrate() {
  sqlite3* db = db_.get();
  int current = 0;
  {
    auto v = Stmt::prepare(db, "PRAGMA user_version");
    if (!v) return v.error();
    if (auto r = v->step(); !r) return r.error();
    current = static_cast<int>(v->col_int(0));
  }
  if (current > kLatestSchema) return make_error("schema_too_new", "store schema is newer than this binary");
  for (int next = current + 1; next <= kLatestSchema; ++next) {
    Txn txn(db);
    if (auto s = txn.begin(); !s) return s;
    if (auto s = sql::exec(db, kMigrations[next - 1]); !s) return s;
    const std::string bump = "PRAGMA user_version=" + std::to_string(next);
    if (auto s = sql::exec(db, bump.c_str()); !s) return s;
    if (auto s = txn.commit(); !s) return s;
    current = next;
  }
  schema_version_ = current;
  return ok_status();
}

Status Store::prepare_statements() {
  if (read_only_) return ok_status();
  auto st = Stmt::prepare(db_.get(),
                          "INSERT OR IGNORE INTO events(boot_id, source, seq, mono_ns, wall_ns, "
                          "wall_quality, kind, severity, subject, attrs) "
                          "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
  if (!st) return st.error();
  insert_stmt_ = std::move(st).value();
  return ok_status();
}

Result<std::int64_t> Store::register_boot(const std::string& boot_id, std::int64_t wall_ns,
                                          const std::string& kernel) {
  auto st = Stmt::prepare(db_.get(),
                          "INSERT INTO boots(boot_id, first_seen_wall, kernel) SELECT ?1,?2,?3 "
                          "WHERE NOT EXISTS (SELECT 1 FROM boots WHERE boot_id=?1)");
  if (!st) return st.error();
  st->bind(1, boot_id).bind(2, wall_ns).bind(3, kernel);
  if (auto s = st->exec(); !s) return s.error();
  return boot_ordinal(boot_id);
}

Result<std::int64_t> Store::boot_ordinal(const std::string& boot_id) {
  auto st = Stmt::prepare(db_.get(), "SELECT ordinal FROM boots WHERE boot_id=?1");
  if (!st) return st.error();
  st->bind(1, boot_id);
  auto r = st->step();
  if (!r) return r.error();
  if (!*r) return make_error("unknown_boot", boot_id);
  return st->col_int(0);
}

Result<std::vector<BootInfo>> Store::boots() {
  auto st = Stmt::prepare(db_.get(), "SELECT ordinal, boot_id, first_seen_wall, kernel FROM boots ORDER BY ordinal");
  if (!st) return st.error();
  std::vector<BootInfo> out;
  for (;;) {
    auto r = st->step();
    if (!r) return r.error();
    if (!*r) break;
    out.push_back({st->col_int(0), st->col_text(1), st->col_int(2), st->col_text(3)});
  }
  return out;
}

Result<std::map<std::string, std::uint64_t>> Store::max_seq_by_source(const std::string& boot_id) {
  auto st = Stmt::prepare(db_.get(), "SELECT source, MAX(seq) FROM events WHERE boot_id=?1 GROUP BY source");
  if (!st) return st.error();
  st->bind(1, boot_id);
  std::map<std::string, std::uint64_t> out;
  for (;;) {
    auto r = st->step();
    if (!r) return r.error();
    if (!*r) break;
    out[st->col_text(0)] = static_cast<std::uint64_t>(st->col_int(1));
  }
  return out;
}

Result<Store::InsertStats> Store::insert_batch(std::vector<Event>& events) {
  if (read_only_) return make_error("sqlite_readonly", "store opened read-only");
  InsertStats stats;
  if (events.empty()) return stats;
  sqlite3* db = db_.get();
  Txn txn(db);
  if (auto s = txn.begin(); !s) return s.error();

  // Events may arrive for a boot not yet registered (ingest, first batch).
  std::set<std::string> seen_boots;
  // WHERE NOT EXISTS rather than INSERT OR IGNORE: an ignored insert still
  // advances the AUTOINCREMENT sequence, which made boot ordinals skip (2 boots
  // showed as "boot 6"). Ordering stayed right; the numbers did not.
  auto boot_stmt = Stmt::prepare(db, "INSERT INTO boots(boot_id, first_seen_wall, kernel) SELECT ?1,?2,'unknown' "
                                     "WHERE NOT EXISTS (SELECT 1 FROM boots WHERE boot_id=?1)");
  if (!boot_stmt) return boot_stmt.error();
  for (const auto& e : events) {
    if (!seen_boots.insert(e.boot_id).second) continue;
    boot_stmt->reset();
    boot_stmt->bind(1, e.boot_id).bind(2, e.wall_ns);
    if (auto s = boot_stmt->exec(); !s) return s.error();
  }

  for (auto& e : events) {
    insert_stmt_.reset();
    insert_stmt_.bind(1, e.boot_id)
        .bind(2, e.source)
        .bind(3, static_cast<std::int64_t>(e.seq))
        .bind(4, e.mono_ns)
        .bind(5, e.wall_ns)
        .bind(6, static_cast<int>(e.wall_quality))
        .bind(7, e.kind)
        .bind(8, static_cast<int>(e.severity))
        .bind(9, e.subject)
        .bind(10, attrs_to_json(e.attrs));
    if (auto s = insert_stmt_.exec(); !s) {
      insert_stmt_.reset();
      for (auto& ev : events) ev.id = 0;
      return s.error();
    }
    if (sqlite3_changes(db) == 1) {
      e.id = sqlite3_last_insert_rowid(db);
      ++stats.inserted;
    } else {
      e.id = 0;
      ++stats.duplicates;
    }
  }
  insert_stmt_.reset();
  if (auto s = txn.commit(); !s) {
    for (auto& ev : events) ev.id = 0;
    return s.error();
  }
  return stats;
}

Result<std::vector<StoredEvent>> Store::query(const EventQuery& q) {
  std::string sql = std::string("SELECT ") + kEventCols + " FROM events e JOIN boots b ON b.boot_id=e.boot_id WHERE 1=1";
  if (q.from) sql += " AND (b.ordinal > ?1 OR (b.ordinal = ?1 AND e.mono_ns >= ?2))";
  if (q.to) sql += " AND (b.ordinal < ?3 OR (b.ordinal = ?3 AND e.mono_ns <= ?4))";
  if (q.since_wall_ns) sql += " AND e.wall_ns >= ?5";
  if (!q.kind_prefix.empty()) sql += " AND substr(e.kind, 1, length(?6)) = ?6";
  if (!q.source.empty()) sql += " AND e.source = ?7";
  sql += " ORDER BY b.ordinal, e.mono_ns, e.source, e.seq LIMIT ?8";
  auto st = Stmt::prepare(db_.get(), sql);
  if (!st) return st.error();
  if (q.from) st->bind(1, q.from->boot_ordinal).bind(2, q.from->mono_ns);
  if (q.to) st->bind(3, q.to->boot_ordinal).bind(4, q.to->mono_ns);
  if (q.since_wall_ns) st->bind(5, *q.since_wall_ns);
  if (!q.kind_prefix.empty()) st->bind(6, q.kind_prefix);
  if (!q.source.empty()) st->bind(7, q.source);
  st->bind(8, static_cast<std::int64_t>(q.limit));
  std::vector<StoredEvent> out;
  for (;;) {
    auto r = st->step();
    if (!r) return r.error();
    if (!*r) break;
    out.push_back(read_event_row(*st));
  }
  return out;
}

Result<std::optional<StoredEvent>> Store::event_by_id(std::int64_t id) {
  auto st = Stmt::prepare(db_.get(), std::string("SELECT ") + kEventCols +
                                         " FROM events e JOIN boots b ON b.boot_id=e.boot_id WHERE e.id=?1");
  if (!st) return st.error();
  st->bind(1, id);
  auto r = st->step();
  if (!r) return r.error();
  if (!*r) return std::optional<StoredEvent>{};
  return std::optional<StoredEvent>{read_event_row(*st)};
}

Result<std::int64_t> Store::event_count() {
  auto st = Stmt::prepare(db_.get(), "SELECT COUNT(*) FROM events");
  if (!st) return st.error();
  if (auto r = st->step(); !r) return r.error();
  return st->col_int(0);
}

Result<std::int64_t> Store::open_incident(const Incident& inc) {
  auto st = Stmt::prepare(db_.get(),
                          "INSERT INTO incidents(opened_boot, opened_mono, opened_wall, trigger, subject, "
                          "trigger_event, status) VALUES(?1,?2,?3,?4,?5,?6,'open')");
  if (!st) return st.error();
  st->bind(1, inc.opened.boot_id).bind(2, inc.opened.mono_ns).bind(3, inc.opened_wall_ns)
      .bind(4, inc.trigger).bind(5, inc.subject);
  if (inc.trigger_event_id) st->bind(6, inc.trigger_event_id);
  else st->bind_null(6);
  if (auto s = st->exec(); !s) return s.error();
  return sqlite3_last_insert_rowid(db_.get());
}

Status Store::close_incident(std::int64_t id, const TimePoint& at, IncidentStatus status) {
  auto st = Stmt::prepare(db_.get(),
                          "UPDATE incidents SET closed_boot=?1, closed_mono=?2, status=?3 WHERE id=?4");
  if (!st) return st.error();
  st->bind(1, at.boot_id).bind(2, at.mono_ns).bind(3, std::string_view(status_str(status))).bind(4, id);
  return st->exec();
}

Result<std::vector<Incident>> Store::incidents(bool open_only) {
  std::string sql =
      "SELECT i.id, i.opened_boot, i.opened_mono, i.opened_wall, i.closed_boot, i.closed_mono, "
      "i.trigger, i.subject, i.trigger_event, i.status, ob.ordinal, cb.ordinal "
      "FROM incidents i JOIN boots ob ON ob.boot_id=i.opened_boot "
      "LEFT JOIN boots cb ON cb.boot_id=i.closed_boot";
  if (open_only) sql += " WHERE i.status='open'";
  sql += " ORDER BY ob.ordinal, i.opened_mono";
  auto st = Stmt::prepare(db_.get(), sql);
  if (!st) return st.error();
  std::vector<Incident> out;
  for (;;) {
    auto r = st->step();
    if (!r) return r.error();
    if (!*r) break;
    Incident inc;
    inc.id = st->col_int(0);
    inc.opened.boot_id = st->col_text(1);
    inc.opened.mono_ns = st->col_int(2);
    inc.opened_wall_ns = st->col_int(3);
    if (!st->col_null(4)) {
      inc.closed.boot_id = st->col_text(4);
      inc.closed.mono_ns = st->col_int(5);
      inc.closed.boot_ordinal = st->col_int(11);
    }
    inc.trigger = st->col_text(6);
    inc.subject = st->col_text(7);
    inc.trigger_event_id = st->col_null(8) ? 0 : st->col_int(8);
    inc.status = status_from(st->col_text(9));
    inc.opened.boot_ordinal = st->col_int(10);
    out.push_back(std::move(inc));
  }
  return out;
}

Result<std::optional<Incident>> Store::incident(std::int64_t id) {
  auto all = incidents(false);
  if (!all) return all.error();
  for (auto& inc : *all) {
    if (inc.id == id) return std::optional<Incident>{std::move(inc)};
  }
  return std::optional<Incident>{};
}

Status Store::replace_hypotheses(std::int64_t incident_id, const std::vector<Hypothesis>& hs) {
  sqlite3* db = db_.get();
  Txn txn(db);
  if (auto s = txn.begin(); !s) return s;
  auto del = Stmt::prepare(db, "DELETE FROM hypotheses WHERE incident_id=?1");
  if (!del) return del.error();
  del->bind(1, incident_id);
  if (auto s = del->exec(); !s) return s;
  auto ins = Stmt::prepare(db,
                           "INSERT INTO hypotheses(incident_id, playbook, title, verdict, score, "
                           "explanation, next_steps) VALUES(?1,?2,?3,?4,?5,?6,?7)");
  if (!ins) return ins.error();
  auto ev = Stmt::prepare(db, "INSERT INTO evidence(hypothesis_id, event_id, role, note) VALUES(?1,?2,?3,?4)");
  if (!ev) return ev.error();
  for (const auto& h : hs) {
    ins->reset();
    ins->bind(1, incident_id).bind(2, h.playbook).bind(3, h.title)
        .bind(4, std::string_view(to_string(h.verdict))).bind(5, h.score).bind(6, h.explanation)
        .bind(7, nlohmann::json(h.next_steps).dump());
    if (auto s = ins->exec(); !s) return s;
    const std::int64_t hid = sqlite3_last_insert_rowid(db);
    for (const auto& e : h.evidence) {
      ev->reset();
      ev->bind(1, hid);
      if (e.event_id) ev->bind(2, e.event_id);
      else ev->bind_null(2);
      ev->bind(3, std::string_view(to_string(e.role))).bind(4, e.note);
      if (auto s = ev->exec(); !s) return s;
    }
  }
  return txn.commit();
}

Result<std::optional<std::string>> Store::meta(const std::string& key) {
  auto st = Stmt::prepare(db_.get(), "SELECT value FROM meta WHERE key=?1");
  if (!st) return st.error();
  st->bind(1, key);
  auto r = st->step();
  if (!r) return r.error();
  if (!*r) return std::optional<std::string>{};
  return std::optional<std::string>{st->col_text(0)};
}

Status Store::set_meta(const std::string& key, const std::string& value) {
  auto st = Stmt::prepare(db_.get(), "INSERT INTO meta(key, value) VALUES(?1,?2) "
                                     "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
  if (!st) return st.error();
  st->bind(1, key).bind(2, value);
  return st->exec();
}

Result<SipKey> Store::name_hash_key() {
  auto hex_to_key = [](const std::string& hex) -> std::optional<SipKey> {
    if (hex.size() != 32) return std::nullopt;
    SipKey k{};
    for (std::size_t i = 0; i < 16; ++i) {
      unsigned v = 0;
      if (std::sscanf(hex.c_str() + 2 * i, "%2x", &v) != 1) return std::nullopt;
      k[i] = static_cast<std::uint8_t>(v);
    }
    return k;
  };
  auto existing = meta("name_hash_key");
  if (!existing) return existing.error();
  if (*existing) {
    if (auto k = hex_to_key(**existing)) return *k;
    return make_error("bad_meta", "name_hash_key is malformed");
  }
  if (read_only_) return make_error("no_key", "no name hash key yet");
  SipKey k{};
  std::ifstream rnd("/dev/urandom", std::ios::binary);
  if (!rnd.read(reinterpret_cast<char*>(k.data()), static_cast<std::streamsize>(k.size()))) {
    return make_error("no_entropy", "cannot read /dev/urandom");
  }
  std::string hex;
  char buf[3];
  for (auto b : k) {
    std::snprintf(buf, sizeof buf, "%02x", b);
    hex += buf;
  }
  if (auto s = set_meta("name_hash_key", hex); !s) return s.error();
  return k;
}

Result<std::int64_t> Store::used_bytes() {
  auto one = [&](const char* pragma) -> Result<std::int64_t> {
    auto st = Stmt::prepare(db_.get(), pragma);
    if (!st) return st.error();
    if (auto r = st->step(); !r) return r.error();
    return st->col_int(0);
  };
  auto pages = one("PRAGMA page_count");
  auto free_pages = one("PRAGMA freelist_count");
  auto size = one("PRAGMA page_size");
  if (!pages) return pages.error();
  if (!free_pages) return free_pages.error();
  if (!size) return size.error();
  return (*pages - *free_pages) * *size;
}

Result<std::int64_t> Store::apply_retention(std::int64_t now_wall_ns, int retention_days,
                                            std::int64_t max_bytes) {
  // Age-based deletion only trusts wall time that was synchronised when
  // recorded; an unsynchronised RTC-less box can believe it is 1970 or 2040,
  // and that must not wipe the store. The size cap always applies.
  const std::int64_t cutoff = now_wall_ns - static_cast<std::int64_t>(retention_days) * 86'400LL * 1'000'000'000LL;
  std::int64_t deleted_total = 0;
  for (int round = 0; round < 1000; ++round) {
    auto used = used_bytes();
    if (!used) return used.error();
    const bool over_size = *used > max_bytes;
    auto st = Stmt::prepare(db_.get(),
        "DELETE FROM events WHERE id IN ("
        " SELECT e.id FROM events e JOIN boots b ON b.boot_id=e.boot_id"
        " WHERE e.id NOT IN (SELECT event_id FROM evidence WHERE event_id IS NOT NULL)"
        "   AND e.id NOT IN (SELECT trigger_event FROM incidents WHERE trigger_event IS NOT NULL)"
        "   AND (?1 OR (e.wall_quality = 2 AND e.wall_ns < ?2))"
        " ORDER BY b.ordinal, e.mono_ns LIMIT 2000)");
    if (!st) return st.error();
    st->bind(1, over_size ? 1 : 0).bind(2, cutoff);
    if (auto s = st->exec(); !s) return s.error();
    const int n = sqlite3_changes(db_.get());
    deleted_total += n;
    if (n == 0) break;
    if (over_size) {
      // Freed pages only count once moved to the freelist; re-measure.
      continue;
    }
  }
  if (deleted_total > 0) {
    if (auto s = sql::exec(db_.get(), "PRAGMA incremental_vacuum"); !s) return s.error();
  }
  return deleted_total;
}

Status Store::quick_check() {
  auto r = run_quick_check(db_.get());
  if (!r) return r.error();
  if (*r != "ok") return make_error("sqlite_corrupt", *r);
  return ok_status();
}

Status Store::set_max_pages(std::int64_t pages) {
  const std::string s = "PRAGMA max_page_count=" + std::to_string(pages);
  auto st = Stmt::prepare(db_.get(), s);
  if (!st) return st.error();
  auto r = st->step();
  if (!r) return r.error();
  return ok_status();
}

Status Store::backup_to(const std::string& path) {
  auto st = Stmt::prepare(db_.get(), "VACUUM INTO ?1");
  if (!st) return st.error();
  st->bind(1, path);
  return st->exec();
}

}  // namespace hm
