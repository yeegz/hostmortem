# Testing

All commands run inside the Ubuntu 24.04 dev container via `scripts/dev.sh`
(Docker; on macOS, Colima works). Nothing here needs real hardware.

| Layer | What | Command |
|---|---|---|
| Unit (portable) | queue, store, codecs, config, SipHash, parsers, detector, correlator, reports | `scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'` |
| Integration (Linux) | writer + store + detector, collectors against fake `/proc` trees, real rtnetlink dump | same `ctest` run (`hm_linux_tests`) |
| Scenario fixtures | 13 synthetic event streams replayed through `hm ingest` + `hm explain --json`, checked against `# expect:` headers | `ctest -R scenario` |
| Real-fault lab | network namespaces + dnsmasq + iptables/tc netem/ip link/kill -9/dropped capabilities/agent SIGKILL | `scripts/lab.sh [scenario...]` (privileged container) |
| Sanitizers | ASan+UBSan, and separately TSan (they cannot be combined) | `scripts/dev.sh 'cmake --preset asan && cmake --build --preset asan && ctest --preset asan'` and the same with `tsan` |
| Fuzzing (found 1 real crash, fixed) | libFuzzer on JSON ingest, config, kmsg, procfs, netlink, DNS | `scripts/dev.sh 'cmake --preset clang-fuzz && cmake --build --preset clang-fuzz && build/clang-fuzz/tests/fuzz/fuzz_netlink -max_total_time=60 tests/fuzz/corpus/netlink'` |
| Benchmark | queue → writer → SQLite throughput and drops | `scripts/dev.sh 'cmake --preset release && cmake --build --preset release && bench/run_bench.sh 10 4 5000 5'` |

## Expected behaviour written before the tests

| Failure | Expected behaviour | Test |
|---|---|---|
| Same batch inserted twice | Row count unchanged; duplicates counted | `Store.InsertAssignsIdsAndReplayIsIdempotent` |
| Process killed mid-transaction | Committed batches present, in-flight batch absent, quick_check ok | `Store.CrashMidTransactionLeavesConsistentStore`, lab `agent_crash` |
| Store file corrupted | Set aside, recreated, `store.recovered` recorded | `Store.CorruptFileIsSetAsideAndRecreated` |
| Disk full | Batch fails atomically; pending bounded; high-priority kept; recovery commits a drop record | `Store.DiskFullFailsBatchAtomically`, `Writer.DiskFullShedsLowPriorityAndRecovers` |
| Queue saturation | Producers never block; every event popped once or counted dropped | `EventQueue.ConcurrentProducersAccountForEveryEvent` (run under TSan) |
| Malformed telemetry | Rejected with reason, counted, never crashes | `JsonCodec.RejectsMalformedInput`, fuzzers |
| Kernel log unreadable | OOM cannot be ruled out; conclusion unknown; gap listed | `Correlator.MissingKernelLogMeansNoConfidentVerdict`, lab `missing_kmsg` |
| Wall clock in 1970 | Ordering and retention unaffected | `Store.OrdersByBootOrdinalThenMonotonic`, `Store.AgeRetentionIgnoresUnsyncedWallTime` |

Failing tests are never deleted to get a green run. Two real bugs were
found by the scenario harness and the lab; see `ENGINEERING_LOG.md`.
