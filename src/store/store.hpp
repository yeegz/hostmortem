// The evidence store: one SQLite file in WAL mode.
//
// Durability contract (ADR-0004): journal_mode=WAL, synchronous=NORMAL.
// A committed transaction survives a process crash. On power loss the most
// recent commits (at most one commit interval, 5 s by default) may roll back,
// but the database is never left inconsistent. A successful insert_batch()
// return means "committed to the WAL", not "on the platter".
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/event.hpp"
#include "core/model.hpp"
#include "core/result.hpp"
#include "core/siphash.hpp"
#include "store/sqlite_handle.hpp"

namespace hm {

struct EventQuery {
  std::optional<TimePoint> from;  // inclusive
  std::optional<TimePoint> to;    // inclusive
  std::optional<std::int64_t> since_wall_ns;
  std::string kind_prefix;
  std::string source;
  std::size_t limit = 100000;
};

struct StoredEvent {
  Event event;
  std::int64_t boot_ordinal = 0;
};

struct BootInfo {
  std::int64_t ordinal = 0;
  std::string boot_id;
  std::int64_t first_seen_wall_ns = 0;
  std::string kernel;
};

class Store {
 public:
  enum class Mode { read_write, read_only };

  // Opens (creating if needed), runs PRAGMA quick_check and migrations.
  // If the file fails quick_check in read_write mode it is renamed aside to
  // "<path>.corrupt-<unix-time>" and a fresh store is created; recovered_from()
  // then reports the aside path so the agent can record store.recovered.
  static Result<std::unique_ptr<Store>> open(const std::string& path, Mode mode);

  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  const std::optional<std::string>& recovered_from() const { return recovered_from_; }
  int schema_version() const { return schema_version_; }

  // Boots
  Result<std::int64_t> register_boot(const std::string& boot_id, std::int64_t wall_ns,
                                     const std::string& kernel);
  Result<std::vector<BootInfo>> boots();
  Result<std::int64_t> boot_ordinal(const std::string& boot_id);

  // Highest seq per source for a boot, so an agent restart within the same
  // boot continues numbering instead of colliding with (and being deduped
  // against) its own earlier events.
  Result<std::map<std::string, std::uint64_t>> max_seq_by_source(const std::string& boot_id);

  struct InsertStats {
    std::size_t inserted = 0;
    std::size_t duplicates = 0;
  };
  // One transaction. Idempotent on (boot_id, source, seq): replays are
  // counted as duplicates and leave the row count unchanged. Inserted events
  // get their id set. On failure the transaction is rolled back and no event
  // in the batch is persisted.
  Result<InsertStats> insert_batch(std::vector<Event>& events);

  Result<std::vector<StoredEvent>> query(const EventQuery& q);
  Result<std::optional<StoredEvent>> event_by_id(std::int64_t id);
  Result<std::int64_t> event_count();

  // Incidents
  Result<std::int64_t> open_incident(const Incident& inc);
  Status close_incident(std::int64_t id, const TimePoint& at, IncidentStatus status);
  Result<std::vector<Incident>> incidents(bool open_only = false);
  Result<std::optional<Incident>> incident(std::int64_t id);
  Status replace_hypotheses(std::int64_t incident_id, const std::vector<Hypothesis>& hs);

  // Meta key/value
  Result<std::optional<std::string>> meta(const std::string& key);
  Status set_meta(const std::string& key, const std::string& value);
  // Per-install SipHash key, created on first use.
  Result<SipKey> name_hash_key();

  // Retention: deletes oldest-boot-first events that no stored evidence
  // references, until both limits hold. Returns rows deleted.
  Result<std::int64_t> apply_retention(std::int64_t now_wall_ns, int retention_days,
                                       std::int64_t max_bytes);
  Result<std::int64_t> used_bytes();
  Status quick_check();

  // Test hook: caps the file size so SQLITE_FULL can be produced on demand.
  Status set_max_pages(std::int64_t pages);

  Status backup_to(const std::string& path);  // VACUUM INTO

  sqlite3* raw() const { return db_.get(); }

 private:
  explicit Store(sql::DbPtr db) : db_(std::move(db)) {}
  Status migrate();
  Status prepare_statements();

  sql::DbPtr db_;
  bool read_only_ = false;
  int schema_version_ = 0;
  std::optional<std::string> recovered_from_;
  sql::Stmt insert_stmt_;
};

}  // namespace hm
