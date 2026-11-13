# Playbooks

Each playbook encodes a documented edge failure mode. Points: 4 or more is
supported, 1–3 possible; a decisive contradiction rules it out. Every
hypothesis lists missing telemetry when a collector could not see something.

| Playbook | Supported when | Ruled out when | Public case behind it |
|---|---|---|---|
| `local_resolver` | `getaddrinfo()` fails while the *nearest-in-time* direct upstream query succeeds | the paired direct queries fail too | systemd #29069 (~90 % success under systemd-resolved, 2023) |
| `upstream_dns` | direct upstream queries fail while the gateway answers ICMP | the gateway is unreachable at the same time (DNS is a consequence) | Red Hat BZ 2047182 (5–10 % of queries failing) |
| `link_or_gateway` | link down / carrier lost, default route removed, gateway unreachable | gateway answered every probe and no link/route change | balenaFin LTE gateway offline until power-cycled (2021) |
| `path_degradation` | ICMP loss with replies in between, or ≥ 2 probes far above the 10-minute baseline RTT | — | HAOS 18.3 14,000 ms ping spikes (2026) |
| `retrans_storm` | host-wide retransmitted share above max(2 %, 3× baseline) | samples stayed at baseline | — (per-flow attribution needs eBPF, v0.2) |
| `conntrack_pressure` | conntrack `insert_failed`/`drop` rise, or "table full" logged | counters flat | Weaveworks conntrack DNS race (2018) |
| `nic_stall` | `NETDEV WATCHDOG` / transmit timeout logged | kernel log readable and silent (noted as absence-based) | HAOS NIC watchdog regression (2026) |
| `oom_kill` | kernel OOM-killer line or cgroup `memory.events` `oom_kill` rise | a watched process exited, kernel log readable, no OOM record | Home Assistant core #181120: restarts wrongly blamed on OOM |
| `restart_loop` | ≥ 3 restarts of one process in the window | — | — |
| `process_exit` | a watched process exited without an OOM record (crash, external kill or clean stop are indistinguishable without eBPF) | an OOM record explains it | — |
| `clock_step` | REALTIME stepped > 1 s vs BOOTTIME, or kernel reports unsynchronised | — | ntpsec list: TLS "certificate is not yet valid" on RTC-less Pis (2022) |
| `resource_pressure` | PSI: CPU some ≥ 50 %, memory some ≥ 20 %, IO some ≥ 50 % | PSI low throughout | — |

Conclusion rule: **unknown** when no hypothesis is supported, or when the top two
supported scores are within one point; the output then lists what could not be
seen.
