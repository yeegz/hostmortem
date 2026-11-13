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

### Pipeline (bench/run_bench.sh, raw rows in bench/results-aarch64-*.csv), 14 Nov 2026

Measured with the current durability setting (synchronous=FULL: every commit
fsynced; commits spaced ≥ 200 ms apart under load; ADR-0009). The host was also
running other work (load average about 3.7–5.4 on 4 vCPUs).

| Offered | Runs | Achieved events/s: median (min–max) | Dropped (all counted as events) | Commits/run |
|---|---|---|---|---|
| 4 × 5,000 = 20,000/s for 5 s | 10 | 19,996 (19,923–19,999) | 0 in every run | 22 |
| 4 × 12,500 = 50,000/s for 5 s | 5 | 46,616 (36,210–49,985) | median 16,609 of ~250,000 offered | 15 |

Before the durability fix (synchronous=NORMAL) the 50,000/s case dropped
nothing. That was faster because commits weren't really durable; the 20,000/s
research target is met either way.

### Agent footprint (release build, packaging/hmd.conf.example defaults, 60 s idle, measured 13 Nov before ADR-0009)

| Metric | Value | Target |
|---|---|---|
| VmRSS / VmHWM | 6,068 kB / 6,068 kB | < 30 MB |
| CPU (utime+stime) | 5 ticks at 100 Hz in 60 s = 0.08 % of one core | < 1 % on a Pi 4 |
| Threads | 7 | — |

Single measurement, not repeated. Command: see the "Agent footprint" step in docs/HANDOFF.md.

### Durability (lab/run_lab.sh agent_crash)

Hard reset of a VM (scripts/hard-reset-test.sh, synchronous=FULL): 0.59 s and
1.39 s of evidence lost in two runs (+ ≤ 1 s tick resolution); incident spans
both boots; quick_check ok.

The newest committed event was captured 1.97 s before SIGKILL (bound: the 5 s
commit interval; warnings commit immediately). quick_check ok; one incident
across the restart.
