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
  Install instructions first, then real lab incidents as examples; refresh
  its data after a lab run with that repo's `tools/build_data.py`.

## State (24 Nov 2026)
v0.2.0 released: one-line `install.sh`, `hm drill`, `hm report`, packages for
Ubuntu 22.04/24.04, Debian 12/13 and Raspberry Pi OS (amd64, arm64). The
install path is tested on fresh VMs with systemd (`scripts/install-smoke.sh`,
CI job `package / vm`). v0.1.0's unit crash-looped under systemd (SIGSYS);
fixed in v0.2.0. 92 tests, clean under ASan+UBSan and TSan.
No external users yet. Not yet done: a physical power-cut test, eBPF (v0.3),
Raspberry Pi numbers.

## Next steps, in order
See `ROADMAP.md`, "Next milestone": external testers first, then Pi numbers
and the power-cut test.

## Releasing
Bump `project(... VERSION x.y.z)` in `CMakeLists.txt`, add a `## [x.y.z]`
section to `CHANGELOG.md` (its text becomes the release notes), merge to main
with CI green, then `git tag vx.y.z && git push origin vx.y.z`.
`release.yml` rebuilds, re-runs every install test and publishes the `.deb`s,
`install.sh` and `SHA256SUMS`.

## Rules carried from the research
- Never claim a number without the script that produced it.
- Zero confident wrong attributions is the release gate; "unknown" is a valid answer.
- No AI in the diagnosis path in the MVP.
