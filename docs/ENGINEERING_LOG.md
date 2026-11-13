# Engineering log

Verified evidence only: each entry names the test or command that showed it.
These are the raw material for the case study and interviews. Nothing here is
a projection.

## 2026-11-13: v0.1 engine built (session 1)

### Bug: un-finalised SQLite statement blocked every migration
- **Symptom:** 14 store and writer tests aborted with "cannot commit
  transaction - SQL statements in progress".
- **Cause:** `PRAGMA journal_mode=WAL` and `PRAGMA user_version` statements
  were still active (stepped once, not reset or finalised) when the migration
  transaction tried to COMMIT. SQLite refuses to commit while a statement is
  mid-execution on the connection.
- **Fix:** scope each PRAGMA statement so RAII finalises it before COMMIT
  (`src/store/store.cpp`).
- **Shows:** RAII lifetimes matter at the API level, not only for memory.

### Bug: a real latency spike was hidden by window medians
- **Found by:** the `latency_spike` scenario fixture (`ctest -R scenario`).
- **Cause:** `path_degradation` compared the median RTT of the whole incident
  window against the baseline. Healthy probes before and after a 90-second spike
  outnumbered the slow ones, so the median stayed normal.
- **Fix:** count probes clearly above baseline (≥ 2 probes over max(3×, +50 ms)).

### Bug: "possible" hypothesis with no cited event
- **Found by:** the citation gate (`validate_citations`) on the `missing_kmsg` fixture.
- **Cause:** when the kernel log is unreadable, OOM was kept as "possible"
  but cited nothing.
- **Fix:** cite the process exits that make OOM a candidate, phrased as
  "consistent with an OOM kill but cannot be confirmed".

### Gap: partial loss and pure latency never opened an incident
- **Found by:** designing the real-fault lab. Netem 300 ms delay never fails a
  1-second probe; 50 % loss rarely gives 3 consecutive failures.
- **Fix:** detector triggers `path_loss` (≥ 3 of the last 10 ICMP probes lost,
  with replies in between) and `latency_excursion` (3 consecutive RTTs over an
  EWMA baseline learned only from normal samples). Tests:
  `Detector.PartialLossOpensPathLoss`, `Detector.SustainedLatencyOpensExcursion`.

### Lab bug that produced a false pass
- **What happened:** the first real `dns_upstream` lab run "passed". dnsmasq had
  actually failed to start (relative `--pid-file` path; dnsmasq changes to `/`
  when it daemonizes), so DNS was down from the start and "upstream DNS" was
  right for the wrong reason.
- **Fix:** an absolute pid path and a pre-flight check (DNS, ping and TCP must
  work before any fault is injected); a failed pre-flight is a lab failure.
- **Lesson:** a passing test on a broken harness is worse than a failing one.

### Bug: an upstream DNS outage looked like a local resolver fault
- **Found by:** the real `dns_upstream` lab scenario, once the lab was fixed.
  The conclusion was "unknown: local_resolver and upstream_dns equally supported".
- **Cause:** "concurrent" meant any probe within ±15 s. Healthy direct queries
  from just before the fault started counted as "direct succeeded while the
  resolver failed".
- **Fix:** pair each failure with the nearest-in-time probe from the other
  family (`pair_nearest` in `playbooks.cpp`). Both real DNS scenarios then pass.

### Kernel detail: CAP_SYS_ADMIN also opens /dev/kmsg
- **Found by:** the real `missing_kmsg` lab scenario. The agent ruled OOM out
  even though `setpriv` had removed CAP_SYSLOG (CapEff bit 34 confirmed cleared).
- **Cause:** the kernel's `check_syslog_permissions()` still accepts
  CAP_SYS_ADMIN "for historical reasons", and the privileged container kept it.
  The agent was right. The lab was wrong.
- **Fix:** the lab drops both capabilities; `hm doctor` now shows SYS_ADMIN.

### Sanitizer findings that were not bugs, recorded so nobody re-investigates
- ASan: one `hm ingest` took 54 s instead of 0.09 s. gdb showed the time in
  `__asan::FakeStack::Allocate`: stack-use-after-return tracking on
  nlohmann/json's large frames. That sub-check is off in the asan test preset;
  all other ASan/UBSan checks stay on.
- TSan: "unexpected memory mapping" at startup on kernel 6.8 arm64 (larger ASLR
  entropy). The tests run under `setarch -R`, and gtest discovery moved to test
  time (`DISCOVERY_MODE PRE_TEST`) so it also runs under it.

### Final real-fault lab, 13 Nov 2026
8 of 8 scenarios as expected; agent SIGKILL: quick_check ok, 1.97 s of evidence lost, one incident.

### Fuzzer finding: one kernel line could silence kernel-log collection
- **Found by:** `fuzz_kmsg` (libFuzzer + ASan/UBSan), within 60 s of its first run
  (14,507 inputs). Input kept at `tests/fuzz/regressions/kmsg-netdev-watchdog-at-end`.
- **Cause:** a message ending right after `NETDEV WATCHDOG` made
  `substr(pos + 16)` start past the end and throw `std::out_of_range`. In the
  agent, the collector thread catches it and exits, so the kernel log would stop
  being recorded for the rest of the boot: exactly the evidence the NIC-stall
  and OOM playbooks depend on.
- **Fix:** a bounds-checked slice; regression test
  `Kmsg.WatchdogMarkerAtEndOfMessageDoesNotThrow`. Re-fuzzed for 120 s:
  10,132,027 inputs, no crash. Other targets, 60 s each, no crash: json_event
  462,729; config 920,422; procfs 304,273; netlink 1,676,653; dns 3,025,888.

### CI on GitHub runners (first push, 13 Nov 2026)
debug, asan and tsan passed on ubuntu-24.04 (amd64) and ubuntu-24.04-arm,
the first amd64 verification. fuzz-smoke failed (missing libclang-rt-18-dev)
and lint failed (shellcheck: unguarded cd, an unused variable); both fixed.

### The biggest finding so far: NORMAL durability lost a whole boot (13 Nov 2026)
- **Test:** `scripts/hard-reset-test.sh` boots static hmd in a nested QEMU VM
  (Ubuntu 6.8 kernel, ext4 on virtio). With the gateway unreachable an incident
  opens; then QEMU `system_reset` (no shutdown, no sync).
- **With synchronous=NORMAL:** boot 2 found an empty store; all 258 boot-1
  writes were gone. FR2 ("at most one commit interval") was false.
- **With synchronous=FULL:** PASS. 2 boots, 43 boot-1 events survived, newest
  0.59 s before the reset (+ ≤ 1 s tick resolution), store quick_check ok, one
  incident opened in boot 1 and closed in boot 2 (`spans_boots=true`). ADR-0009.
- **Side finding:** `INSERT OR IGNORE` on the AUTOINCREMENT boots table burned
  ordinals (2 boots showed as "boot 6"); fixed with `WHERE NOT EXISTS`, test
  `Store.BootOrdinalsStayConsecutive`.
- **Cost found by the benchmark:** fsync per 500-event commit dropped throughput.
  Spaced commits written; re-measurement pending (HANDOFF).
- **Limit of the test:** a VM reset, not a physical power cut. QEMU's host
  cache keeps what the guest wrote to its disk, so this proves what hmd and
  SQLite flushed, not the disk hardware.
