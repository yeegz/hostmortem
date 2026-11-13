# Handoff: RESUME HERE

For the next session (human, Claude Code or another agent). Read this, then
`ROADMAP.md`, then `ENGINEERING_LOG.md`.

## What this is
Hostmortem (formerly the FaultScope brief; renamed per research ADR-0001): a
local-first Linux edge flight recorder. C++20 agent `hmd`, CLI `hm`, SQLite WAL.
The research PDFs live in `~/Downloads/FaultScope Reassessed_ ... .pdf`
(authoritative) and `FaultScope Research Report and Build-Ready Engineering Blueprint.pdf`.

## How to work on it (macOS host)
- Everything builds and runs in Docker (Colima): `scripts/dev.sh '<cmd>'`.
- Build + test: `scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'`
- Real-fault lab: `scripts/lab.sh [dns_upstream dns_local link_down kill9 missing_kmsg packet_loss latency agent_crash]`
  (privileged throwaway container; outputs in `lab/out/`, git-ignored).
- Regenerate synthetic fixtures: `python3 -I lab/gen_fixtures.py`.

## State
See `ROADMAP.md` "State" column; it is kept current. Nothing is pushed to GitHub
and no release exists. The user wants a private build repo plus a public
showcase repo; neither has been created yet (needs the user's go-ahead).

## Next steps, in order
1. Real reboot test in a VM (hard reset mid-incident).
2. `.deb` + `hostmortem` user + `systemd-analyze security`.
3. Remaining blueprint scenarios (stress-ng CPU, cgroup MemoryMax OOM, restart loop, clock step).
4. Tag v0.1.0; then Phase 2 (eBPF).

## Rules carried from the research
- Never claim a number without the script that produced it.
- Zero confident wrong attributions is the release gate; "unknown" is a valid answer.
- No AI in the diagnosis path in the MVP.
