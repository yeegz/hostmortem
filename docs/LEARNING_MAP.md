# Learning map

What each module teaches, why it is built this way, what can go wrong, and the
questions you should be able to answer without notes before calling it yours
(the research's explain-back gate, section 22).

| Module | Concepts | Why this design | Alternatives rejected | Failure modes handled | Explain-back questions |
|---|---|---|---|---|---|
| `core/result.hpp`, `unique_fd.hpp` | RAII, move-only ownership, errors as values, `[[nodiscard]]` | One owner per resource; no exception crosses a thread | exceptions, raw `int` fds | double close, leaked fds on error paths | Why is `UniqueFd` move-only? What happens to an fd if `open_socket()` returns early? |
| `pipeline/event_queue.hpp` | mutex/condvar, lost wake-ups, backpressure, bounded memory | Never block producers; reserve capacity for evidence | lock-free ring, blocking push, drop-oldest | floods, slow writer, shutdown drain | Why notify after unlocking? Why is the predicate wait immune to lost wake-ups? What breaks if push blocked? |
| `store/` | SQLite WAL, `synchronous`, transactions, idempotent ingest, migrations, quick_check | Durable enough for SD cards without an fsync per event | PostgreSQL, TSDB, custom log | crash mid-transaction, corruption, SQLITE_FULL, clock-driven retention bugs | What exactly survives a power cut with WAL+NORMAL? Why must statements be finalised before COMMIT (the bug we hit)? |
| `core/clock*`, ADR-0005 | CLOCK_BOOTTIME vs MONOTONIC vs REALTIME, adjtimex, boot_id | Ordering must survive bad wall clocks | wall-time ordering | RTC-less boot in 1970, NTP steps, suspend | Why BOOTTIME? Why can mono not be compared across boots? |
| `collectors/netlink_*` | rtnetlink, multicast groups, NLM_F_DUMP/MULTI, NLMSG_OK, ENOBUFS | Event-driven link/route state with resync | polling `/sys/class/net` | kernel notification overrun, truncated messages | What does ENOBUFS mean on a netlink socket and why re-dump? |
| `collectors/kmsg_*` | `/dev/kmsg` record format, seq gaps (EPIPE), dmesg_restrict, CAP_SYSLOG | Kernel seq as event seq makes re-reads idempotent | journald (libsystemd dependency) | ring overwrite, permission denied, printk clock vs BOOTTIME | Why does the kmsg collector use two emitters? |
| `collectors/prober.cpp` | getaddrinfo vs raw DNS, connected UDP, ping sockets vs raw ICMP, non-blocking connect, SO_ERROR | Active-vs-passive comparison separates local from upstream faults | single resolver probe | blocking DNS (worker thread), spoofed replies, missing privileges | Why can getaddrinfo not run on the probe thread? Why does a connected UDP socket report ECONNREFUSED? |
| `collectors/procfs_*` | procfs formats, counters vs rates, PSI, conntrack stats | Deltas, not totals; reset-safe | `/proc` via shell tools | counter reset, missing files on old kernels | What is PSI "some" vs "full"? |
| `analysis/detector.cpp` | deterministic state machines, EWMA baselines | Same input → same incidents (replayable) | ML anomaly detection | storms teaching the baseline, cross-boot timers | Why is the baseline only updated from non-excursion samples? |
| `analysis/playbooks.cpp`, `correlator.cpp` | evidence-based reasoning, explicit uncertainty, ruling out | Never confidently wrong; "unknown" is valid | probabilities, LLM verdicts | absence-based conclusions, concurrent-but-unrelated probes (the nearest-pairing bug) | When does Hostmortem say "unknown"? Why pair probes by nearest time? |
| `agent/agent.cpp` | signalfd, sigmask inheritance, std::jthread/stop_token, ordered shutdown | No work in signal handlers | `signal()` handlers + flags | SIGTERM during write, collector crash | Why block signals before starting threads? |
| `lab/run_lab.sh` | netns, veth, iptables, tc netem, capabilities via setpriv | Real faults, reproducible, no hardware | mocks only | a broken lab producing false passes (we hit this) | Why does the lab check the network before injecting faults? |
| `tests/fuzz` | libFuzzer, sanitizers | Parsers handle kernel and user bytes in a privileged daemon | manual review only | out-of-bounds reads, UB | What does `-fsanitize=fuzzer,address,undefined` catch that unit tests miss? |
