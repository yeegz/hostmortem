# Maintainer handoff: start here

For whoever picks this up next (a person or a coding agent). Read this, then
`ROADMAP.md` and `ENGINEERING_LOG.md`.

## What this is
Hostmortem: a local-first flight recorder for Linux edge boxes. C++20 agent
`hmd`, CLI `hm`, SQLite in WAL mode. It was planned from two research reports
(problem validation, competitor review, requirements); their conclusions are in
`docs/REQUIREMENTS.md` and the ADRs. The working name in the reports was
"FaultScope" (taken by other projects; see ADR-0001).

## How to work on it
- Everything builds and runs in Docker: `scripts/dev.sh '<cmd>'`. On macOS,
  Colima works; it only shares your home directory with its VM, so keep clones there.
- Build and test: `scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'`
- Real-fault lab: `scripts/lab.sh [scenario ...]` (privileged throwaway container; output in `lab/out/`).
- VM hard-reset test: `scripts/hard-reset-test.sh`.
- Everything else, with expected results: `docs/BUILDING.md`.

## Where things live
- Source, issues and releases: https://github.com/yeegz/hostmortem (Apache-2.0).
  Pushing a `v*` tag runs `.github/workflows/release.yml`.
- Project site: https://yousofselim.com/hostmortem-showcase/ (repo `yeegz/hostmortem-showcase`).
  It replays real lab incidents; refresh its data after a lab run with that
  repo's `tools/build_data.py`.

## State (14 Nov 2026)
v0.1.0 released: amd64 and arm64 `.deb`s with SHA-256 sums. Lab 11/11, VM
hard reset passes, 84 tests clean under sanitizers on amd64 and arm64.
Not yet done: a physical power-cut test, eBPF (v0.2), Raspberry Pi numbers.

## Next steps, in order
1. Physical power-cut test on a Raspberry Pi (the VM test can't test disk hardware).
3. Remaining blueprint scenarios (stress-ng CPU, cgroup MemoryMax OOM, restart loop, clock step).
4. Tag v0.1.0; then Phase 2 (eBPF).

## Rules carried from the research
- Never claim a number without the script that produced it.
- Zero confident wrong attributions is the release gate; "unknown" is a valid answer.
- No AI in the diagnosis path in the MVP.
