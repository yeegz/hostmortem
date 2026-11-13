# Interview prep: explain Hostmortem without notes

Study guide for the owner. Everything here is in the code or was measured;
nothing is aspirational. For each hard part: what it is, why this design, what
was rejected, how it fails, what proves it and where it lives. Then likely
questions with short, honest answers.

A reviewer's main worry was "can he explain it unaided?". The test: close this
file and draw each diagram, then answer the questions out loud.

---

## 0. The one-minute version

Hostmortem is a recorder for Linux edge boxes (gateways, kiosks, homelab
servers). When the box loses DNS, its link or a service and someone reboots it,
the evidence normally disappears. Hostmortem's agent (`hmd`) keeps writing
evidence to a SQLite file: kernel messages, link and route changes, CPU, memory
and TCP counters, and its own active probes (DNS, ICMP, TCP). The evidence
survives the reboot. Afterwards the CLI (`hm explain`) reconstructs the incident
and lists hypotheses as supported, possible or ruled out, each citing event ids.
When the evidence does not settle it, the answer is "unknown", along with what
could not be seen.

Built in C++20. Linux-only interfaces. No AI in the diagnosis.

```
collectors (one thread each) ──> bounded 2-lane queue ──> writer ──> SQLite (WAL, FULL)
 procfs · netlink · kmsg ·         drops are counted       batches     │
 prober · process watch            and written down        + detector ─┘ opens/closes incidents
                                                          hm explain ──> 12 playbooks ──> text/JSON/HTML
```

---

## 1. The bounded two-lane queue

**Where:** `src/pipeline/event_queue.hpp`, used by `src/agent/writer.cpp`.

**Design.** Five collector threads push; one writer thread pops (MPSC). A mutex
and a condition variable guard two `std::deque`s:
- a **high lane** for severity warning or worse (capacity 4,096), and
- a **low lane** for routine samples (16,384).

`push()` never blocks. When a lane is full, the new event is dropped and counted
per (source, lane). The writer turns those counts into `pipeline.drop` events,
so data loss shows up in the timeline itself. At 80 % full, `push()` returns
`pressure` and the procfs sampler halves its rate.

**Why two lanes.** A flood of routine samples must never push out the one
kernel OOM line an incident depends on.

**Rejected:**
- Blocking producers. A stalled writer would stall the netlink reader, and then
  the *kernel* drops notifications (ENOBUFS) that we could not even count.
- One lane that drops the oldest event. A sample flood would evict the evidence.
- Lock-free ring. Not justified without profiling showing lock contention.

**Concurrency detail.** `notify_one()` is called after releasing the lock, so the
woken consumer doesn't immediately block on a held mutex. Lost wake-ups can't
happen, because the consumer waits with a predicate
(`closed || !high.empty() || !low.empty()`) and the state changes under the lock
before the notify.

**Failure modes handled:** floods (drops counted), shutdown (`close()` lets the
writer drain whatever is queued), a full disk (the writer bounds what it holds
pending and sheds low-priority events first, counting what it sheds).

**Proof:** `EventQueue.ConcurrentProducersAccountForEveryEvent`. Six threads push
20,000 events each; the test asserts popped + dropped = pushed and that no event
is delivered twice. It runs under TSan in CI. Also
`Writer.CommitsDrainsAndRecordsDropsAsEvents`.

---

## 2. Durability: SQLite WAL, and the bug the hard-reset test found

**Where:** `src/store/store.cpp`, `docs/adr/0004-…`, `docs/adr/0009-synchronous-full.md`.

**Design.** One SQLite file in WAL mode. The writer commits a batch every 5 s
(`commit_interval`), when 500 or more events are pending, or soon after any
warning-level event, with commits at least 200 ms apart. `INSERT OR IGNORE` on
`UNIQUE(boot_id, source, seq)` makes re-delivery harmless. Startup runs
`PRAGMA quick_check`; a corrupt file is renamed aside and a fresh store is
created, with a `store.recovered` event. Migrations are numbered and run in a
transaction.

**The finding (tell this story).** I first used `synchronous=NORMAL`, assuming a
power cut would lose at most one commit interval. A test that hard-resets a VM
mid-incident (QEMU `system_reset`, `scripts/hard-reset-test.sh`) found that **all
of boot 1 was lost**. In WAL mode, NORMAL never fsyncs a commit, only
checkpoints, so "committed" data was still in the kernel page cache. An earlier
test that SIGKILLed the agent couldn't see this, because the kernel keeps its
cache when a process dies. The fix was `synchronous=FULL`, which fsyncs every
commit. The same test then passed: 0.59 s of evidence lost, and the incident
survived across both boots.

**The cost and the fix for it.** An fsync per 500-event commit slowed throughput
sharply. The writer now spaces commits at least 200 ms apart, so each fsync
covers everything queued. Current numbers: see `docs/PERFORMANCE.md`.

**Rejected:** PostgreSQL on an edge box (too heavy); a time-series database (wrong
shape: we store discrete evidence); a hand-written log (we'd need our own
recovery and query engine).

**Failure modes:**
- Crash mid-transaction → the transaction rolls back, earlier commits stay
  (`Store.CrashMidTransactionLeavesConsistentStore` forks a child that `_exit`s
  mid-transaction).
- Disk full → `SQLITE_FULL` fails the whole batch atomically, retention runs at
  90 % of the cap, and pending events stay bounded
  (`Store.DiskFullFailsBatchAtomically` uses `PRAGMA max_page_count`;
  `Writer.DiskFullShedsLowPriorityAndRecovers`).
- Corrupt file → set aside and rebuilt (`Store.CorruptFileIsSetAsideAndRecreated`).
- A guard test pins `synchronous=FULL` (`Store.CommitsAreFsyncedSynchronousFull`).

**The limit to state honestly:** a VM reset is not a physical power cut. QEMU's
host cache keeps what the guest already wrote, so the test proves what the
agent and SQLite flushed, not how a particular SD card behaves.

---

## 3. Time: boot order first, then CLOCK_BOOTTIME

**Where:** `src/core/clock_linux.cpp`, `src/store/store.cpp` (boots table),
`docs/adr/0005-time-and-ordering.md`.

**Design.** Every event carries:
- `boot_id` (`/proc/sys/kernel/random/boot_id`, regenerated every boot);
- a per-source sequence number;
- CLOCK_BOOTTIME (`mono_ns`);
- CLOCK_REALTIME (`wall_ns`);
- a wall-clock quality flag from `adjtimex()` (synced or unsynced).

Ordering is by **boot ordinal** (the order boots were first seen), then by
`mono_ns`. Wall time is display-only. A `clock.step` event is recorded when
REALTIME jumps more than 1 s relative to BOOTTIME between samples.

**Why.** Boxes without a real-time clock boot in 1970 and then jump when NTP
arrives. Wall-time ordering scrambles the timeline exactly when it matters.
BOOTTIME rather than MONOTONIC because BOOTTIME keeps counting through suspend.

**Subtleties:**
- Mono time can't be compared across boots, so the detector restarts its quiet
  timer at the first event of a new boot instead of subtracting.
- Kernel log timestamps are MONOTONIC-based; kmsg events are tagged
  `ts_source=printk`.
- Age-based retention only deletes events recorded with a *synced* wall clock,
  so a box that believes it's 1970 can't wipe its own history.
- Boot ordinals once skipped (2 boots showed as "boot 6"), because
  `INSERT OR IGNORE` burns AUTOINCREMENT numbers. Fixed with `WHERE NOT EXISTS`.

**Proof:** `Store.OrdersByBootOrdinalThenMonotonic`,
`Store.AgeRetentionIgnoresUnsyncedWallTime`, `Store.BootOrdinalsStayConsecutive`,
`Detector.IncidentSurvivesReboot`, and the hard-reset test (`spans_boots=true`).

---

## 4. Detection and the playbooks, and why "unknown" is an answer

**Where:** `src/analysis/detector.cpp`, `playbooks.cpp`, `correlator.cpp`;
`docs/playbooks.md`; `docs/adr/0006-…`.

**Detector.** A pure function of the committed event stream: the same input
always gives the same incidents, which is why `hm ingest` replays lab fixtures
exactly. It opens an incident on symptoms such as:
- 3 failed probes in a row,
- partial ICMP loss (3 of the last 10 probes lost, with replies in between),
- RTT 3 times over a baseline learned only from normal samples,
- link down or default route removed,
- OOM, NIC watchdog, clock step, process exit, a restart burst,
- a retransmission spike, or high pressure-stall (PSI) readings.

One incident at a time. It closes after 2 minutes with no symptom (configurable)
and stays open across reboots.

**Playbooks.** Twelve deterministic rules, each scoring integer evidence points
*for*, recording evidence *against*, and listing *missing* telemetry. 4 or more
points is supported, 1–3 possible, and a decisive contradiction rules the
hypothesis out. The conclusion is **unknown** if nothing is supported, or if the
top two supported hypotheses are within one point.

**Citation gate.** Every hypothesis that isn't ruled out must cite at least one
stored event id (`validate_citations`). The scenario harness and the tests
enforce it.

**Examples worth knowing cold:**
- *Local resolver vs upstream DNS.* `getaddrinfo()` fails, but a direct UDP query
  to the upstream server succeeds → local resolver. Both fail while the gateway
  answers → upstream. The gateway is unreachable too → link, and the DNS
  failures are a consequence.
- *OOM vs `kill -9`.* A process exits, the kernel log is readable and holds no
  OOM record → OOM is ruled out, and the conclusion is unknown (a crash, a kill
  and a clean stop look the same without eBPF). If the kernel log is
  *unreadable*, OOM stays possible and the gap is listed.

**Bugs found here (good stories):**
- An upstream outage first looked "local", because healthy probes from just
  before the fault counted as concurrent. Fix: pair each failure with the
  *nearest-in-time* probe (`pair_nearest`).
- A latency spike was hidden by comparing whole-window medians. Fix: count the
  probes clearly above baseline.

**Rejected:** ML anomaly detection or an LLM verdict. Neither is explainable or
deterministic, and the research's release gate is zero confident wrong
attributions.

**Proof:** `Correlator.*` (for example `LocalResolverFaultDoesNotBlameUpstream`,
`KillNineRulesOutOomAndSaysUnknown`, `MissingKernelLogMeansNoConfidentVerdict`),
13 scenario fixtures (`ctest -R scenario`) and the real lab.

---

## 5. The fault lab

**Where:** `lab/run_lab.sh` (run via `scripts/lab.sh`); `scripts/hard-reset-test.sh`.

**Design.** Two network namespaces joined by a veth pair: `hm-client` (where the
agent runs) and `hm-up` (gateway 192.0.2.1, dnsmasq DNS, a TCP server). Each
scenario starts a fresh store, checks that DNS, ping and TCP actually work,
records a baseline, injects the fault, recovers, waits for the incident to
close, then compares `hm explain --json` with the expected result.

| Scenario | Injection |
|---|---|
| Upstream DNS down | `iptables -j DROP` on UDP 53 |
| Local resolver broken | resolv.conf pointed at a dead stub |
| Cable pulled | `ip link set down` |
| Packet loss | `tc netem loss 50%` |
| Latency spike | `tc netem delay 300ms 50ms` |
| kill -9 | SIGKILL a watched process |
| Hidden kernel log | the same kill, agent without CAP_SYSLOG and CAP_SYS_ADMIN |
| Agent crash | SIGKILL the agent mid-incident, then restart it |

Separately, the nested-QEMU VM hard-reset test (section 2).

**Lessons:**
- A broken lab produced a **false pass**: dnsmasq never started, so "upstream
  DNS" was right for the wrong reason. Hence the pre-flight check.
- Dropping CAP_SYSLOG did *not* hide `/dev/kmsg`, because the kernel still
  accepts CAP_SYS_ADMIN "for historical reasons". The agent had been right; the
  lab was wrong.

- **A safety failure, and the fix.** The first OOM scenario's 64 MiB cgroup
  limit silently failed to apply, and the test process grew until the
  *system-wide* OOM killer ended it, in a VM shared with another project. Only
  the test process died, but it could have been anything. Now the limit is
  verified before the allocator starts, the allocator caps itself at 512 MiB,
  and the lab grades only evidence created after the injection: an old OOM line
  had produced a false pass.

**Result:** 11 of 11 scenarios as expected (14 Nov 2026), including a real
cgroup OOM kill, CPU saturation and a restart loop.

---

## 6. Sanitizers and fuzzing

- **ASan + UBSan** and **TSan**: presets `asan` and `tsan`, run in CI on amd64
  and arm64. Two environment quirks, recorded so nobody re-investigates them:
  - ASan's stack-use-after-return check made one ingest take 54 s instead of
    0.09 s (fake-stack allocation on nlohmann/json's huge stack frames), so that
    one sub-check is off.
  - TSan refuses to start under kernel 6.x's larger address-space randomisation
    on arm64, so its tests run under `setarch -R`.
- **libFuzzer**: six targets (JSON ingest, config, kmsg, procfs, netlink, DNS).
  - `fuzz_kmsg` found a **real crash within 60 s**: a kernel line ending right
    after `NETDEV WATCHDOG` made `substr(pos + 16)` throw `std::out_of_range`.
  - In the agent, that would have ended the kernel-log thread for the rest of
    the boot.
  - Fixed with a bounds-checked slice and a regression test; the crashing input
    is kept in `tests/fuzz/regressions/`. Re-fuzzed: 10.1 M inputs, no crash.

---

## 7. Likely interview questions, with short honest answers

1. **Why C++ and not Rust or Go?** The roles I'm aiming at (embedded and Linux)
   ask for C/C++, and I wanted to learn RAII, ownership and native tooling.
   Rust would have been fine; Go's garbage-collected footprint is a worse fit on
   a Raspberry Pi.
2. **How do you avoid lost wake-ups in the queue?** State changes under the
   mutex, then notify; the consumer waits with a predicate, so a notify that
   arrives before the wait doesn't matter.
3. **Why not block producers when the queue is full?** Because then the netlink
   reader stops reading, the kernel overflows its buffer and drops notifications
   I can't count. Dropping in my code is measurable; dropping in the kernel isn't.
4. **What exactly survives a power cut?** Every committed batch, because commits
   fsync the WAL. Events still queued in memory are lost: at most one commit
   interval (5 s), and warnings commit within about 200 ms. Measured in a VM
   reset: 0.59 s lost.
5. **Why did NORMAL fail?** In WAL mode it doesn't fsync on commit, only at
   checkpoints, so committed data sat in the page cache. SIGKILL doesn't drop the
   page cache; a reset does. My first test used the wrong failure.
6. **Is a VM reset the same as pulling the plug?** No. It proves what my code and
   SQLite flushed. Disk-level behaviour (SD cards that lie about flushing) needs
   real hardware, which I haven't tested yet.
7. **Why CLOCK_BOOTTIME?** It's monotonic and keeps counting through suspend. Wall
   time can jump years on boxes without a real-time clock.
8. **How do you order events across two boots?** By the order the boot IDs were
   first seen, then by BOOTTIME within each boot. Never by wall time.
9. **How is ingest idempotent?** `UNIQUE(boot_id, source, seq)` plus
   `INSERT OR IGNORE`. Kernel records reuse the kernel's own sequence number, so
   re-reading the log after a restart creates exact duplicates that are ignored.
10. **Why not exactly-once delivery?** It isn't achievable across crashes without
    a lot more machinery. At-least-once plus idempotent writes gives the same
    result for a single writer.
11. **How does it tell a local DNS fault from an upstream one?** It compares
    `getaddrinfo()` through the system resolver with a raw UDP query to the
    upstream server, pairing each failure with the nearest probe in time.
12. **What if a collector can't run?** It reports `collector.status` as
    unavailable with a reason, and every diagnosis lists what couldn't be seen.
    For example, without the kernel log, OOM can never be ruled out.
13. **When does it say "unknown"?** When nothing reaches the supported threshold,
    or when the top two hypotheses are within one point.
14. **Why no machine learning?** Diagnoses have to be explainable, deterministic
    and cite evidence. A rule I can't explain can't be trusted on someone else's
    gateway.
15. **What did fuzzing find?** An out-of-range `substr` on a crafted kernel line,
    which would have silently stopped kernel-log collection. Fixed, with a
    regression test.
16. **What's the biggest limitation today?** Retransmissions are counted for the
    whole host, and exit codes aren't visible; both need eBPF (v0.2). There are
    no numbers from a Raspberry Pi or real hardware yet.
17. **How is the agent kept from being a security risk?** It runs as a dedicated
    user with two capabilities (CAP_SYSLOG, CAP_NET_RAW) and a hardened systemd
    unit. It has no network listener, never captures packet payloads, and
    hashes DNS names with a per-install key. All parsers are fuzzed.
18. **What would you do differently?** Write the reset test before choosing the
    durability setting; I trusted a SIGKILL test for a power-loss claim. And make
    fault-injection tests fail safe by construction: my first OOM test escaped
    its cgroup and triggered the system-wide OOM killer.
19. **Did you write it yourself?** I directed it. Claude Code agents did much of
    the typing; the decisions, test design and debugging of the findings above
    are what I can explain and defend. (Say this plainly.)
20. **What's next?** Verify the throughput after the durability fix, build the
    `.deb` and test on a Raspberry Pi, then add four eBPF tracepoints with
    fallbacks.
