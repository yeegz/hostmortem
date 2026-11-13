# ADR-0001: Narrow single-host flight recorder named Hostmortem

**Context.** The original FaultScope brief combined Linux, eBPF, an ESP32
fleet, a correlation engine and a dashboard. The research reassessment
(13 Nov 2026) found three problems with that brief: it could not be finished
in the time available, the ESP32 fleet half overlaps funded vendors (Memfault,
Golioth), and the name is taken (an MIT-licensed GitHub project
oakb1ack/FaultScope and a PyPI package). The gap it did confirm: no free
tool keeps cross-layer evidence from an intermittent edge failure through the
reboot that usually destroys it, and says what it can and cannot see.

**Decision.** Build "Option A, narrowed": a local-first recorder for single
Linux hosts and gateways. Agent `hmd`, CLI `hm`. Working name Hostmortem
(GitHub search for "hostmortem" returned no repositories on 13 Nov 2026; PyPI,
Debian, domain and trademark checks are still to do).

**Non-goals (v1).** APM, Kubernetes, payload capture, enforcement, OTA, MCU
crash analytics, AI verdicts, a fleet server, MQTT.

**Consequences.** The ESP32 peer is optional and comes after v1.0. The web
UI is limited to five screens and comes after the engine. Every phase
gate needs the previous tag.
