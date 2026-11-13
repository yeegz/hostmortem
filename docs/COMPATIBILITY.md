# Compatibility

## Platforms

| Platform | Status | Notes |
|---|---|---|
| Ubuntu 24.04, kernel 6.8, arm64 (Colima VM on Apple silicon, Docker container) | **Verified**: builds with GCC 13.3 and Clang 18; all tests and the real-fault lab run here | The only verified environment so far |
| Ubuntu 24.04 amd64 | CI configured (GitHub Actions), not yet run | |
| Ubuntu 22.04 / Debian 12 | Intended (C++20, GCC 11/12; no C++23 used) | Not built yet |
| Raspberry Pi OS (arm64), Pi 4/5 | Intended primary edge target | **Unverified**: no hardware in this environment |
| Kernels 5.4–5.x | Intended "degraded" support | Unverified; QEMU kernel matrix planned for v0.2 |
| macOS, Windows, WSL2 | Not supported for the agent | `hm_core` (store, analysis, parsers) is portable C++ and builds without Linux headers; the agent and lab need Linux |

## What each kernel feature costs when absent

`hm doctor` reports these live. The diagnosis lists the same gaps under "Could not see".

| Feature | Needed for | If missing |
|---|---|---|
| `/proc/pressure` (CONFIG_PSI, 4.20+) | `resource_pressure` | CPU/memory/IO starvation invisible |
| `/proc/net/stat/nf_conntrack` | `conntrack_pressure` | conntrack races and table-full drops invisible (common in containers: the module is not loaded in that netns) |
| `/dev/kmsg` readable (CAP_SYSLOG when `dmesg_restrict=1`) | `oom_kill`, `nic_stall`, link-down messages | OOM can never be ruled out; the conclusion stays "unknown" for process exits |
| ping socket (`net.ipv4.ping_group_range`) or CAP_NET_RAW | gateway reachability | upstream DNS cannot be separated from link faults |
| cgroup v2 | cgroup OOM kills of watched services | only kernel-log OOM evidence |
| BTF + 5.8 ring buffer | v0.2 eBPF probes | v0.2 falls back to the counters above |

## Known limits in v0.1 (by design, reported as missing evidence)

- Retransmissions are host-wide; per-connection attribution needs eBPF (v0.2).
- Process exit codes and signals are not visible; a crash, `kill -9` and a clean
  stop look the same. A process that dies and restarts within the 1 s poll is
  seen only as a pid change.
- ICMP probes are IPv4 only. TCP and DNS probes accept IPv4/IPv6 literals; TCP
  targets must be IP literals (resolving them would couple TCP probes to DNS).
- `getaddrinfo()` probes go through caches (systemd-resolved, nscd): a cached
  name can still resolve during a short upstream outage. The direct probe is
  not cached.
- kmsg timestamps are printk (MONOTONIC) based and drift from BOOTTIME after suspend.
