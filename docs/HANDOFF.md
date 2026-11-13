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
See `ROADMAP.md` "State" column; it is kept current. No release or tag exists.
- Private repo: github.com/yeegz/hostmortem (this tree).
- Public showcase: github.com/yeegz/hostmortem-showcase, local ~/Documents/hostmortem-showcase,
  site on GitHub Pages at https://yousofselim.com/hostmortem-showcase/ (served under the
  user site's custom domain). Refresh its data after a lab run:
  `python3 -I tools/build_data.py ~/Documents/hostmortem`, copy `lab/out/reports/*.html`
  to `site/reports/`, then verify with
  `NODE_PATH=~/Documents/yeegz.github.io/node_modules node tools/verify.mjs <url> docs/screens`.
- Agent footprint: run `build/release/hmd --config <conf> --run-for 65s` in the dev container
  and read /proc/<pid>/status and /proc/<pid>/stat after 60 s.

## Next steps, in order
1. Real reboot test in a VM (hard reset mid-incident).
2. `.deb` + `hostmortem` user + `systemd-analyze security`.
3. Remaining blueprint scenarios (stress-ng CPU, cgroup MemoryMax OOM, restart loop, clock step).
4. Tag v0.1.0; then Phase 2 (eBPF).

## Rules carried from the research
- Never claim a number without the script that produced it.
- Zero confident wrong attributions is the release gate; "unknown" is a valid answer.
- No AI in the diagnosis path in the MVP.
