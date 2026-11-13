# ADR-0004: SQLite WAL, synchronous=NORMAL, batched commits

**Context.** FR2: a power cut loses at most the last commit interval and never
corrupts the store. Edge boxes run on SD cards, where every fsync costs both
latency and flash wear.

**Decision.** One SQLite file with `journal_mode=WAL`, `synchronous=NORMAL` and
`auto_vacuum=INCREMENTAL`. The writer commits every 500 events or every 5 s.
Any event at warning or above triggers an immediate commit. Insert is `INSERT OR
IGNORE` on `UNIQUE(boot_id, source, seq)`, so replays are idempotent. Startup
runs `PRAGMA quick_check`. A store that fails it is renamed to
`<path>.corrupt-<time>`, a fresh one is created, and a `store.recovered` event
is written. Migrations are numbered, append-only and transactional
(`user_version`).

**What "durable" means here.** With WAL + NORMAL, a committed transaction
survives a process crash (it is in the WAL file). On power loss the last
transactions since the most recent WAL sync may roll back; the database stays
consistent. A successful `insert_batch()` therefore means "committed to the
WAL", not "on the platter". `synchronous=FULL` would add an fsync per commit.

**Disk full.** `SQLITE_FULL` fails the whole batch atomically. The writer then
runs retention at 90 % of the cap, keeps pending events up to a bound, and
sheds low-priority events first, counting what it sheds. It retries on the next
cycle. Tested with `PRAGMA max_page_count`.

**Retention.** Oldest boot first, never events referenced as evidence or as an
incident trigger. Age-based deletion only applies to events recorded with a
synchronised wall clock, so an RTC-less box that boots in 1970 cannot wipe its
own history.

**Alternatives.** PostgreSQL per edge box (unjustified footprint); a TSDB
(wrong shape: we store discrete evidence, not dense series); a custom
append-only log (no query engine, own corruption handling).
