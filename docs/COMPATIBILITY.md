# Compatibility

## Platforms

| Platform | Status | Notes |
|---|---|---|
| Ubuntu 24.04, kernel 6.8, arm64 (Colima VM on Apple silicon, Docker container) | **Verified**: builds with GCC 13.3 and Clang 18; all tests and the real-fault lab run here | The only verified environment so far |
| Ubuntu 24.04 amd64 and arm64 (GitHub-hosted runners) | **Verified in CI**: debug, ASan+UBSan and TSan builds and all tests pass (13 Nov 2026) | No real-fault lab run on amd64 yet |
| Packages (v0.2.0): built on Ubuntu 22.04, GCC 11.4, CMake 3.22 | `Depends: libc6 (>= 2.34), libstdc++6 (>= 12), libsqlite3-0, adduser` | All 92 tests pass in that build |
| Debian 12 arm64, fresh VM (Lima on Apple silicon), systemd 252, kernel 6.1 | **Verified** 24 Nov 2026: `scripts/install-smoke.sh` passes (install.sh, service, drill as a group member, report, purge) | Found and fixed the unit's SIGSYS kill |
| Ubuntu 24.04 arm64, fresh VM (Lima on Apple silicon), systemd 255, kernel 6.8 | **Verified** 24 Nov 2026: smoke test passes, including the upgrade from the published 0.1.0 | |
| Ubuntu 22.04 and 24.04, amd64 and arm64, fresh GitHub-hosted VMs | **Verified in CI** on every push (`package.yml`, job `vm`) | |
| Debian 12/13, Ubuntu 22.04/24.04 containers, amd64 and arm64 | **Verified in CI**: install.sh, agent as the service user, remove, purge (no systemd in containers) | |
| Raspberry Pi OS bookworm (arm64), Pi 4/5 | Same Debian 12 arm64 base as the verified VM | **Not run on Pi hardware yet** |
| 32-bit (armhf), Debian 11 / Ubuntu 20.04 and older | Not supported by the packages | Build from source may work; untested |
| Kernels 5.4–5.x | Intended "degraded" support | Unverified; QEMU kernel matrix planned with eBPF (v0.3) |
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
| BTF + 5.8 ring buffer | eBPF probes (planned, v0.3) | would fall back to the counters above |

## Known limits in v0.2 (by design, reported as missing evidence)

- Retransmissions are host-wide; per-connection attribution needs eBPF (planned for v0.3).
- Process exit codes and signals are not visible; a crash, `kill -9` and a clean
  stop look the same. A process that dies and restarts within the 1 s poll is
  seen only as a pid change.
- ICMP probes are IPv4 only. TCP and DNS probes accept IPv4/IPv6 literals; TCP
  targets must be IP literals (resolving them would couple TCP probes to DNS).
- `getaddrinfo()` probes go through caches (systemd-resolved, nscd): a cached
  name can still resolve during a short upstream outage. The direct probe is
  not cached.
- kmsg timestamps are printk (MONOTONIC) based and drift from BOOTTIME after suspend.
