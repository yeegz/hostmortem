# Performance

Rule: no number without the script that produced it and the environment it ran
in. Targets come from the research (section 9) and are **targets**, not results.

## Method

- `bench/bench_pipeline.cpp`: N producer threads push events at a fixed offered
  rate through the real `EventQueue` → `Writer` → SQLite (WAL, production commit
  policy) on disk. Reports achieved events/s, drops (all counted, never silent),
  commits and drain time. `bench/run_bench.sh` repeats it and summarises
  median/min/max; raw CSV rows are kept in `bench/results-*.csv`.
- Agent footprint: `hmd` run for a fixed time with the lab config; RSS from
  `/proc/<pid>/status` (VmRSS, VmHWM), CPU from `/proc/<pid>/stat` utime+stime.

## Environment for every result below

Apple M1 Pro host → Colima VM (Virtualization.Framework, 4 vCPU, 8 GiB) →
Ubuntu 24.04 container, kernel 6.8.0-117-generic aarch64, GCC 13.3,
RelWithDebInfo build. **This is not a Raspberry Pi and not x86.** Treat the
numbers as an upper bound on what a Pi would do.

## Results

> **Stale since 13 Nov 2026 (ADR-0009).** The pipeline figures below were measured
> with synchronous=NORMAL. The store now fsyncs every commit, and partial
> re-runs showed drops at 20,000 events/s with the old 500-event batching.
> Commit spacing (200 ms) has been added; re-run `bench/run_bench.sh` and
> replace this table before quoting any throughput number.

### Pipeline (bench/run_bench.sh, raw rows in bench/results-aarch64-*.csv)

| Offered | Runs | Achieved events/s: median (min–max) | Dropped | Commits/run |
|---|---|---|---|---|
| 4 × 5,000 = 20,000/s for 5 s | 10 | 19,998 (19,749–19,999) | 0 in every run | 200 |
| 4 × 12,500 = 50,000/s for 5 s | 5 | 49,996 (48,135–49,997) | 0 in every run | 497 |

Not yet measured: event-to-commit latency percentiles, the 24-hour query time,
a 72-hour soak, anything on a Raspberry Pi or x86.

### Agent footprint (release build, packaging/hmd.conf.example defaults, 60 s idle)

| Metric | Value | Target |
|---|---|---|
| VmRSS / VmHWM | 6,068 kB / 6,068 kB | < 30 MB |
| CPU (utime+stime) | 5 ticks at 100 Hz in 60 s = 0.08 % of one core | < 1 % on a Pi 4 |
| Threads | 7 | — |

Single measurement, not repeated. Command: see the "Agent footprint" step in docs/HANDOFF.md.

### Durability (lab/run_lab.sh agent_crash)

The newest committed event was captured 1.97 s before SIGKILL (bound: the 5 s
commit interval; warnings commit immediately). quick_check ok; one incident
across the restart.
