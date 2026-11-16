# Roadmap

Phases follow the research plan (section 18). A phase is done only when its
acceptance line is met and recorded here with evidence.

| Phase | Scope | Acceptance | State (17 Nov 2026) |
|---|---|---|---|
| 0 Research, ADRs | name check, ADRs, threat model v0 | ADRs merged | Done: 8 ADRs, threat model v0. Name check: GitHub only; PyPI/Debian/domain/USPTO/EUIPO/MyIPO still to do |
| 1 Native MVP (v0.1) | core, queue, store, collectors, probes, CLI, scenarios | sanitisers clean; cross-boot demo; scenarios emit expected events; hard reset loses ≤ 5 s | **Done.** Engine, CLI, 13 fixtures, 11-scenario real-fault lab: 11/11 pass; 84 tests clean under ASan+UBSan and TSan. VM hard reset PASS ×2 after the ADR-0009 fix; throughput re-measured; .deb built and install-tested for amd64 and arm64. v0.1.0 tagged and published on GitHub Releases; source open under Apache-2.0 |
| 2 eBPF (v0.2) | 4 tracepoints, ring/perf buffer, counter fallbacks, probe self-test, `hm doctor` | degraded mode on 5.4; kernel matrix | **Not started; deliberately v0.2.** v0.1 uses procfs, netlink and kmsg only (ADR-0007) |
| 3 Incident intelligence (v0.3) | detector, playbooks, `hm explain`, HTML report, evaluation harness | 0 confident misattributions over 20 randomised runs per scenario | Detector, 12 playbooks, explain/HTML done early; evaluation harness not started |
| 4 Accessible UI (v1.0) | 5 screens, tokens, axe CI | WCAG 2.2 AA checklist | Not started (frontend comes after the engine, by design) |
| 6 Reliability/perf/security | benchmarks, 72 h soak, fuzzing, TSan, signing | bench report meets or explains targets | Fuzz targets, TSan/ASan presets, pipeline bench exist; soak, Pi numbers, signing not done |
| 7 Release | .deb amd64/arm64, docs, video, 5+ external testers | v1.0 | Partly: v0.1.0 is published (amd64 and arm64 `.deb`s with SHA-256 sums). Still to do for v1.0: video, 5+ external testers |
| 5 ESP32 peer (optional) | reset reasons, NVS boot count, heap/stack, buffered HTTPS | HIL watchdog scenario passes | Not started; needs hardware |

## Next milestone (after v0.1.0)

v0.1.0 met its exit line: the VM hard-reset test, `.deb` packaging with the
`hostmortem` user and hardened unit, the CPU-saturation, cgroup-OOM and
restart-loop scenarios, and the tag are all done (see `CHANGELOG.md`).

1. Clock-step scenario in a VM (the one blueprint scenario still missing).
2. Phase 3 evaluation harness: 20 randomised runs per scenario, 0 confident
   misattributions.
3. Phase 2 (v0.2): eBPF tracepoints with counter fallbacks and a probe self-test.
4. 72 h soak and Raspberry Pi numbers in `docs/PERFORMANCE.md`.
