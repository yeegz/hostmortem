# Requirements

Source: *FaultScope Reassessed* (13 Nov 2026), sections 4–9. Status reflects
the code in this repository, verified by the tests named. "Target" means
not yet measured.

## Problem

> "My Linux edge box lost connectivity, DNS or a service last night and a reboot
> fixed it. What happened before, which layer failed first, and what can I rule out?"

Users: edge/IoT engineers with 1–100 Debian/Ubuntu/Raspberry Pi OS gateways on
Wi-Fi or LTE; homelab and Home Assistant users; lab engineers who must separate
network faults from device-under-test faults. Non-users: Kubernetes-native
teams, MCU-only fleets, APM buyers.

## Functional

| ID | Requirement | Status | Evidence |
|---|---|---|---|
| FR1 | Every event carries boot_id, sequence, CLOCK_BOOTTIME, CLOCK_REALTIME and a wall-clock quality flag | Done | `core/event.hpp`, `Store.OrdersByBootOrdinalThenMonotonic` |
| FR2 | Power loss loses at most the last commit interval (5 s) and never corrupts the store | Done at process level; power-cut on real hardware unverified | `Store.CrashMidTransactionLeavesConsistentStore`, lab `agent_crash` |
| FR3 | Incidents open on configurable triggers | Done | `Detector.*`, lab scenarios |
| FR4 | Each incident lists facts, hypotheses with for/against/missing evidence, and "unknown" when nothing qualifies | Done | `Correlator.*`, 13 fixtures, lab |
| FR5 | Data loss (queue, ring buffer, collector failure) is recorded as events | Done for queue, writer shedding, kmsg ring overwrite, netlink ENOBUFS | `EventQueue.*`, `Writer.CommitsDrainsAndRecordsDropsAsEvents` |
| FR6 | Each collector reports active, degraded or unavailable, with a reason | Done | `ProcfsSampler.EmitsRatesAndReportsMissingFeatures`, `hm doctor` |
| FR7 | Incidents export as self-contained HTML and JSON | Done | `hm explain --html/--json`, `Report.*` |
| FR8 | DNS names keyed-hashed by default unless allowlisted | Done (SipHash-2-4, per-install key) | `SipHash.*`, `Store.NameHashKeyIsStable` |

## Non-functional targets (to be measured; see PERFORMANCE.md)

| Target | Status |
|---|---|
| Idle CPU < 1 % of one core on a Raspberry Pi 4 | Not measured (no Pi in this environment) |
| RSS < 30 MB | Measured in container, see PERFORMANCE.md |
| 20,000 events/s sustained on an x86 VM without silent loss | Measured on arm64 VM, see PERFORMANCE.md |
| p99 event-to-commit < 2 s | By construction for warning+ (immediate commit); not yet measured |
| 256 MB default disk cap | Implemented (`store.max_mb`), retention tested |
| 24-hour timeline query < 200 ms | Not measured |
| Start-up < 1 s, clean shutdown < 2 s | Not measured; shutdown is bounded by 500 ms polls plus the probe timeout |

## Excluded from v0.x

Fleet server, MQTT, Kubernetes enrichment, payload capture, ML/LLM, Windows
and macOS agents, OTA.
