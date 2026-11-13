<img src="docs/brand/icon.svg" width="56" height="56" alt="">

# Hostmortem

[![ci](https://github.com/yeegz/hostmortem/actions/workflows/ci.yml/badge.svg)](https://github.com/yeegz/hostmortem/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/yeegz/hostmortem)](https://github.com/yeegz/hostmortem/releases/latest)
[![licence](https://img.shields.io/badge/licence-Apache--2.0-blue)](LICENSE)

**A flight recorder for Linux edge boxes.** It records what happened, in order,
across the kernel, network and services, keeps it through the reboot that
usually destroys it, and separates what it *knows* from what it *suspects*
and what it *cannot see*.

> "My Linux edge box lost connectivity, DNS or a service last night and a reboot
> fixed it. What happened before, which layer failed first, and what can I rule out?"

Status: **v0.1.0 released** for amd64 and arm64. Early, honest software: see
the [status table](#status) for what it can't do yet. Project site with
replayable real incidents: https://yousofselim.com/hostmortem-showcase/

## Install (Ubuntu 24.04 or any distribution with glibc ≥ 2.38)

```bash
arch=$(dpkg --print-architecture)   # amd64 or arm64
curl -LO "https://github.com/yeegz/hostmortem/releases/download/v0.1.0/hostmortem_0.1.0_${arch}.deb"
curl -LO https://github.com/yeegz/hostmortem/releases/download/v0.1.0/SHA256SUMS
sha256sum --check --ignore-missing SHA256SUMS
sudo apt install "./hostmortem_0.1.0_${arch}.deb"
sudo systemctl enable --now hmd
sudo usermod -aG hostmortem "$USER"   # log in again, then:
hm doctor
```

The agent runs as its own `hostmortem` user with two capabilities, keeps at most
14 days or 256 MB of evidence in `/var/lib/hostmortem`, and has no network
listener. Configuration: `/etc/hostmortem/hmd.conf`.

## What it can and cannot see

| Sees (v0.1) | Cannot see yet |
|---|---|
| CPU, memory, PSI pressure, TCP/UDP counters, conntrack stats, clock sync and steps (procfs) | Which TCP connection retransmitted (needs eBPF, v0.2) |
| Link, address and default-route changes (rtnetlink, with ENOBUFS resync) | Process exit codes/signals (eBPF `sched_process_exit`, v0.2) |
| OOM kills, NIC watchdog, conntrack table full, link down (`/dev/kmsg`) | Packet payloads: never captured, by design |
| `getaddrinfo()` vs direct upstream DNS, ICMP to gateway, TCP connect (active probes) | Anything on other hosts (no fleet server) |
| Exits, restarts and cgroup OOM kills of watched processes | |

When a collector cannot run (missing capability, module or kernel feature) it
says so with a reason, and every diagnosis lists those gaps.

## Build from source (any OS with Docker)

```bash
git clone https://github.com/yeegz/hostmortem && cd hostmortem
scripts/dev.sh 'cmake --preset debug && cmake --build --preset debug && ctest --preset debug'
```

Replay a synthetic scenario (clearly marked SYNTHETIC in every output):

```bash
scripts/dev.sh 'build/debug/hm --db /tmp/demo.db ingest lab/fixtures/reboot_mid_incident.jsonl && build/debug/hm --db /tmp/demo.db incidents && build/debug/hm --db /tmp/demo.db explain 1'
```

Reproduce real faults (network namespaces, dnsmasq, iptables, tc netem, `kill -9`,
dropped capabilities, agent SIGKILL) in a privileged throwaway container:

```bash
scripts/lab.sh
```

On a Linux host: `sudo build/debug/hmd --config packaging/hmd.conf.example`,
then `hm timeline`, `hm incidents`, `hm explain <id> [--html report.html]`, `hm doctor`.

## Example: a real upstream DNS outage (lab)

`iptables -j DROP` on UDP 53 at the upstream, inside the lab (excerpt of real `hm explain` output):

```
CONCLUSION: SUPPORTED - Leading explanation: Upstream DNS server or the path to it.

Observed facts (ordered by boot, then CLOCK_BOOTTIME; offsets from incident open):
  #101      +0.0s  probe.dns.resolver [resolver]  err=timeout latency_ms=1000.0 name=example.com ok=false
  #112      +1.0s  probe.dns.direct [192.0.2.1]  err=timeout latency_ms=1005.3 ... ok=false server=192.0.2.1
  #120      +3.0s  probe.dns.resolver [resolver]  err=resolver_blocked ... ok=false
  ...
Hypotheses (score = evidence points, not a probability):
  [SUPPORTED] Upstream DNS server or the path to it  (upstream_dns, score 6)
      Direct queries to the upstream DNS server failed 6 time(s) (#112, #126, #145, #169 +2 more)
      while the gateway stayed reachable (#113, #130, #146, #170 +2 more).
  [RULED OUT] Local resolver path (nsswitch, stub resolver, systemd-resolved)  (local_resolver, score 2)
      The system resolver failed 6 time(s) (#101, #120, ...), but direct queries failed at the same time.
  [RULED OUT] Local link or default gateway lost  (link_or_gateway)
      The gateway answered throughout (...) and no interface or route change was seen.
```

`resolver_blocked` means glibc's `getaddrinfo()` was still stuck from the previous
round. The prober runs it on a worker thread, so a hung resolver becomes
evidence instead of stalling the other probes.

## Architecture

Collectors (procfs, rtnetlink, kmsg, prober, process watch), one thread each →
a bounded two-lane MPSC queue (drops are counted and recorded as events) → one
writer committing batches to SQLite in WAL mode → a deterministic detector that
opens and closes incidents → twelve playbooks that the CLI runs on demand.
Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), decisions:
[docs/adr/](docs/adr/README.md).

## Results so far

Measured on an Ubuntu 24.04 arm64 container (Colima VM on an Apple M1 Pro, 4 vCPU), 13–14 Nov 2026.
Scripts and raw data: [docs/PERFORMANCE.md](docs/PERFORMANCE.md), [lab/run_lab.sh](lab/run_lab.sh).

| What | Result |
|---|---|
| Real-fault lab | **11 of 11** scenarios diagnosed as expected (upstream DNS, local resolver, link down, kill -9, kill -9 with the kernel log hidden, 50 % loss, 300 ms latency, agent SIGKILL mid-incident, real cgroup OOM kill, CPU saturation, restart loop); 14 Nov |
| VM hard reset mid-incident | PASS in 2 runs: ≤ 1.4 s of evidence lost; the incident spans both boots (`scripts/hard-reset-test.sh`; a VM reset, not a physical power cut) |
| Agent SIGKILL mid-incident | store passed quick_check; newest committed event 1.94 s before the kill (bound 5 s); the same incident continued |
| Pipeline throughput (every commit fsynced) | 20,000 events/s offered: median 19,996 achieved, 0 dropped (10 runs); 50,000/s: drops, all recorded |
| Agent footprint, 60 s idle | 6.4 MB RSS, 0.07–0.27 % of one core (3 runs, final build) |
| Package | `.deb` with a `hostmortem` service user; `systemd-analyze security` exposure 2.2 |
| Tests | 84 (unit, integration, 13 synthetic scenario fixtures), clean under ASan+UBSan and TSan; CI green on amd64 and arm64 |

Public showcase with replayable lab incidents: https://github.com/yeegz/hostmortem-showcase

## Security model

Dedicated user, two ambient capabilities (CAP_SYSLOG, CAP_NET_RAW), hardened
systemd unit, no network listener, keyed-hashed DNS names, no payload capture,
bounded parsers with fuzz targets, escaped reports with a strict CSP. See
[docs/THREAT_MODEL.md](docs/THREAT_MODEL.md).

## Status

| Area | State |
|---|---|
| Engine: queue, store, 5 collectors, detector, 12 playbooks, CLI | Working; 84 tests, clean under ASan/UBSan and TSan, CI on amd64 and arm64 |
| Real-fault lab | 11 of 11 scenarios pass (14 Nov 2026) |
| Hard reset mid-incident | Passes in a VM (≤ 1.4 s lost); **no physical power-cut test yet** |
| Packages | v0.1.0 `.deb` for amd64 and arm64, install-tested in CI |
| eBPF probes | Not started; planned for v0.2 |
| Evaluation harness (randomised runs, accuracy) | Not started |
| Raspberry Pi measurements | Not measured |
| Web UI | Not started; planned for v1.0 |

More: [ROADMAP](docs/ROADMAP.md) · [REQUIREMENTS](docs/REQUIREMENTS.md) ·
[TESTING](docs/TESTING.md) · [PERFORMANCE](docs/PERFORMANCE.md) ·
[COMPATIBILITY](docs/COMPATIBILITY.md) · [PLAYBOOKS](docs/playbooks.md) ·
[LEARNING MAP](docs/LEARNING_MAP.md) · [ENGINEERING LOG](docs/ENGINEERING_LOG.md)

## Contributing and licence

Contributions are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md), the
[code of conduct](CODE_OF_CONDUCT.md), and [SECURITY.md](SECURITY.md) for
private vulnerability reports. Licensed under [Apache-2.0](LICENSE).
