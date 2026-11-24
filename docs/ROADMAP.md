# Roadmap

Phases follow the research plan (section 18). A phase is done only when its
acceptance line is met and recorded here with evidence.

| Phase | Scope | Acceptance | State (24 Nov 2026) |
|---|---|---|---|
| 0 Research, ADRs | name check, ADRs, threat model v0 | ADRs merged | Done: 8 ADRs, threat model v0. Name check: GitHub only; PyPI/Debian/domain/USPTO/EUIPO/MyIPO still to do |
| 1 Native MVP (v0.1) | core, queue, store, collectors, probes, CLI, scenarios | sanitisers clean; cross-boot demo; scenarios emit expected events; hard reset loses ≤ 5 s | **Done.** Engine, CLI, 13 fixtures, 11-scenario real-fault lab: 11/11 pass; 84 tests clean under ASan+UBSan and TSan. VM hard reset PASS ×2 after the ADR-0009 fix; throughput re-measured; .deb built and install-tested for amd64 and arm64. v0.1.0 tagged and published on GitHub Releases; source open under Apache-2.0 |
| 1b Usable product (v0.2) | one-line install, first incident in minutes, shareable report, wider distro support, upgrade path | install → drill → report passes on fresh VMs with systemd, amd64 and arm64, incl. upgrade from 0.1.0 | **Done (v0.2.0, 24 Nov 2026).** `install.sh`, `hm drill`, `hm report` (ADR-0011); packages for Ubuntu 22.04/24.04, Debian 12/13, Raspberry Pi OS; `scripts/install-smoke.sh` passes on fresh Debian 12 and Ubuntu 24.04 VMs (Lima, arm64) and in CI on fresh Ubuntu 22.04/24.04 runner VMs (amd64, arm64). Fixed 0.1.0's unit, which crash-looped under systemd |
| 2 eBPF (v0.3) | 4 tracepoints, ring/perf buffer, counter fallbacks, probe self-test, `hm doctor` | degraded mode on 5.4; kernel matrix | **Not started; moved from v0.2 to v0.3** when v0.2 became the usability release. Until then: procfs, netlink and kmsg only (ADR-0007) |
| 3 Incident intelligence (v0.3) | detector, playbooks, `hm explain`, HTML report, evaluation harness | 0 confident misattributions over 20 randomised runs per scenario | Detector, 12 playbooks, explain/HTML done early; evaluation harness not started |
| 4 Accessible UI (v1.0) | 5 screens, tokens, axe CI | WCAG 2.2 AA checklist | Not started as a live UI. v0.2 added `hm report`, a static self-contained HTML report (ADR-0011) |
| 6 Reliability/perf/security | benchmarks, 72 h soak, fuzzing, TSan, signing | bench report meets or explains targets | Fuzz targets, TSan/ASan presets, pipeline bench exist; soak, Pi numbers, signing not done |
| 7 Release | .deb amd64/arm64, docs, video, 5+ external testers | v1.0 | Partly: v0.1.0 and v0.2.0 published (amd64 and arm64 `.deb`s, `install.sh`, SHA-256 sums). No external testers yet. Still to do for v1.0: video, 5+ external testers |
| 5 ESP32 peer (optional) | reset reasons, NVS boot count, heap/stack, buffered HTTPS | HIL watchdog scenario passes | Not started; needs hardware |

## Next milestone (after v0.2.0)

1. External testers: v0.2.0 is the first version a stranger can install and
   use without help. Get 5 people running it on their own boxes and record
   what they hit (Phase 7).
2. Raspberry Pi numbers in `docs/PERFORMANCE.md` and a physical power-cut test.
3. Clock-step scenario in a VM (the one blueprint scenario still missing).
4. Phase 3 evaluation harness: 20 randomised runs per scenario, 0 confident
   misattributions.
5. Phase 2 (v0.3): eBPF tracepoints with counter fallbacks and a probe self-test.
6. A signed apt repository, so `apt upgrade` picks up new versions.
