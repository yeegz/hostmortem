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

RESULTS_TBD
