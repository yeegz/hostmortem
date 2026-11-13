# Roadmap

Phases follow the research plan (section 18). A phase is done only when its
acceptance line is met and recorded here with evidence.

| Phase | Scope | Acceptance | State (13 Nov 2026) |
|---|---|---|---|
| 0 Research, ADRs | name check, ADRs, threat model v0 | ADRs merged | Done: 8 ADRs, threat model v0. Name check: GitHub only; PyPI/Debian/domain/USPTO/EUIPO/MyIPO still to do |
| 1 Native MVP (v0.1) | core, queue, store, collectors, probes, CLI, scenarios | sanitisers clean; cross-boot demo; scenarios emit expected events; hard reset loses ≤ 5 s | Engine, CLI, 13 fixtures, 8-scenario real-fault lab: 8/8 pass; ASan+UBSan and TSan clean. Real **reboot** (VM hard reset) not yet run; agent SIGKILL is covered |
| 2 eBPF (v0.2) | 4 tracepoints, ring/perf buffer, counter fallbacks, probe self-test, `hm doctor` | degraded mode on 5.4; kernel matrix | Not started |
| 3 Incident intelligence (v0.3) | detector, playbooks, `hm explain`, HTML report, evaluation harness | 0 confident misattributions over 20 randomised runs per scenario | Detector, 12 playbooks, explain/HTML done early; evaluation harness not started |
| 4 Accessible UI (v1.0) | 5 screens, tokens, axe CI | WCAG 2.2 AA checklist | Not started (frontend comes after the engine, by design) |
| 6 Reliability/perf/security | benchmarks, 72 h soak, fuzzing, TSan, signing | bench report meets or explains targets | Fuzz targets, TSan/ASan presets, pipeline bench exist; soak, Pi numbers, signing not done |
| 7 Release | .deb amd64/arm64, docs, video, 5+ external testers | v1.0 | Not started. No release has been published |
| 5 ESP32 peer (optional) | reset reasons, NVS boot count, heap/stack, buffered HTTPS | HIL watchdog scenario passes | Not started; needs hardware |

## Next milestone (v0.1 exit)

1. A real reboot test: a QEMU or Lima VM, hard reset mid-incident; verify ≤ 5 s
   loss and an incident that spans the reboot.
2. `.deb` packaging with the `hostmortem` user, the unit file and `systemd-analyze security`.
3. Remaining blueprint scenarios: CPU saturation and memory pressure (stress-ng,
   cgroup MemoryMax), restart loop (systemd unit), clock step in a VM.
4. Tag v0.1.0 once the above pass and are recorded.
