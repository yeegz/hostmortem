# Changelog

All notable changes. Versions follow semantic versioning.

## [0.2.1] - 2026-11-24

### Changed
- `hm report` and `hm explain --html` have a new look that matches the
  project site: navy surfaces in layers, each incident and hypothesis on a
  raised card, tables in recessed panels, verdicts as pills (cyan
  supported, violet possible, struck-through ruled out), status lights in
  the coverage table, event citations as chips, and a hatched panel for what
  could not be seen. The Hostmortem mark is inline SVG, so the strict CSP
  (`default-src 'none'`) is unchanged. Printing switches to dark ink on white.

## [0.2.0] - 2026-11-24

The usability release: install with one command, record a first real
incident in about ten seconds, and share the result as one HTML file.
(eBPF, previously planned as v0.2, moves to v0.3.)

### Added
- `install.sh`, published with each release:
  `curl -fsSL https://github.com/yeegz/hostmortem/releases/latest/download/install.sh | sudo sh`.
  It picks the `.deb` for this architecture, checks it against `SHA256SUMS`
  and stops on a mismatch, installs it with apt, enables `hmd`, and adds the
  user who ran sudo to the `hostmortem` group. Re-running it upgrades in
  place. `HM_VERSION`, `HM_DEB_DIR` (offline) and `HM_NO_START` adjust it.
- `hm drill`: starts a harmless process named `hm-drill`, which `hmd` always
  watches, then SIGKILLs it. The running agent records the exit through its
  normal path, so a fresh install produces a real incident to explain.
- `hm report [-o FILE] [--incident ID] [--redact]`: one self-contained HTML
  file with the host's coverage, its boots and every incident's diagnosis.
  No scripts, no network requests (strict CSP); `--redact` replaces IP and MAC
  addresses so the file can be sent to someone else (ADR-0011).
- Packages for more systems: built on Ubuntu 22.04, so they install on
  Ubuntu 22.04 and 24.04, Debian 12 and 13, and Raspberry Pi OS (bookworm,
  arm64). 0.1.0 needed glibc 2.38.
- CI tests the documented install path on every push: the `.deb` is
  installed in clean Debian 12/13 and Ubuntu 22.04/24.04 containers, and on
  fresh Ubuntu 22.04 and 24.04 VMs (amd64 and arm64) with systemd, where
  `scripts/install-smoke.sh` installs, runs `hm drill` as a normal user,
  renders `hm report`, upgrades from the published 0.1.0, and purges.
- 7 new tests (92 in all), including a live-agent drill end-to-end test.

### Fixed
- **In 0.1.0 the systemd service never stayed up.** The unit's seccomp
  filter denied the read-only `adjtimex()` hmd calls to read clock-sync state,
  so systemd killed hmd with SIGSYS at start and restarted it in a loop. Seen
  on fresh Ubuntu 24.04 (systemd 255) and Debian 12 (systemd 252) VMs. The
  0.1.0 package test ran hmd by hand as the service user, not through the
  unit, so it missed this; the new fresh-VM test starts the real service. The
  unit now allows that one call, drops `ProtectClock=` (setting the clock was
  already impossible: no CAP_SYS_TIME in the bounding set), and makes any
  other filtered call fail with EPERM instead of killing the recorder.
  If you installed 0.1.0, upgrade: re-run the installer.
- An upgrade left the old `hmd` running until the next reboot; `postinst` now
  restarts a running agent.
- `sudo hm explain` opened the agent's store read-write as root; it now opens
  it read-only, so it can never leave root-owned WAL files behind.
- `hm` says how to get read access (group or sudo) when the store exists but
  is unreadable, instead of "no store".
- An incident now commits in the same transaction as the events that opened
  it. Before, a crash between the batch commit and the incident write kept the
  evidence but lost the incident (ADR-0010).

### Packages
`hostmortem_0.2.0_amd64.deb`, `hostmortem_0.2.0_arm64.deb`, `install.sh` and
`SHA256SUMS` covering all three.

## [0.1.0] - 2026-11-14

### Added
- `hmd` agent: procfs/PSI sampler, rtnetlink collector (initial dump, ENOBUFS
  resync), `/dev/kmsg` reader, active probes (getaddrinfo vs direct UDP DNS,
  ICMP, TCP connect), process and cgroup-OOM watch.
- Bounded two-lane MPSC queue; every drop is counted and recorded as a
  `pipeline.drop` event.
- SQLite store in WAL mode with idempotent ingest, startup quick_check and
  recovery, numbered migrations, retention, and backup via VACUUM INTO.
- Deterministic detector (incidents survive reboots) and 12 playbooks with
  for/against/missing evidence, a citation gate, and an explicit "unknown".
- `hm` CLI: `status`, `timeline`, `incidents`, `explain` (text, JSON,
  self-contained HTML), `doctor`, `export --redact`, `ingest` (synthetic stores
  only), `backup`.
- Real-fault lab (11 scenarios in network namespaces, including a real cgroup
  OOM kill, CPU saturation and a restart loop), nested-QEMU hard-reset
  test, 13 synthetic scenario fixtures, 6 libFuzzer targets, sanitizer presets,
  CI on amd64 and arm64.
- Release workflow: on a `v*` tag, CI builds, tests, packages and install-tests
  amd64 and arm64 `.deb`s and publishes them with SHA-256 sums.
- Debian package: `hostmortem` system user, hardened systemd unit
  (`systemd-analyze security` exposure 2.2), conffile, and a purge that removes
  the store.

### Fixed (found by our own tests)
- **Durability:** `synchronous=NORMAL` lost a whole boot of evidence on a VM
  hard reset; now `synchronous=FULL` with batched, spaced commits (ADR-0009).
- kmsg parser threw on a line ending at `NETDEV WATCHDOG`, which would have
  stopped kernel-log collection (found by libFuzzer).
- An upstream DNS outage could look "local" (probes now paired by nearest time).
- A latency spike could be hidden by window medians.
- Boot ordinals skipped numbers (`INSERT OR IGNORE` on AUTOINCREMENT).
- The store could be created world-readable when hmd was started by hand
  (hmd now sets umask 027).
- CPU starvation only opened an incident at ≥ 80 % PSI in one sample; a real
  saturation peaked at 77.7 %. Now 3 samples in a row at ≥ 50 %, matching the
  playbook.
- The process watch said nothing when the cgroup v2 tree was invisible; it now
  reports `process.cgroup` unavailable with a reason.

### Packages
`hostmortem_0.1.0_amd64.deb` and `hostmortem_0.1.0_arm64.deb` for Ubuntu 24.04
and other distributions with glibc ≥ 2.38, plus `SHA256SUMS`. Install with
`sudo apt install ./hostmortem_0.1.0_<arch>.deb`, then
`sudo systemctl enable --now hmd`.

### Known limits
- No eBPF yet (v0.2): retransmissions are host-wide; exit codes and signals
  aren't visible.
- Hard reset tested in a VM, not as a physical power cut; no Raspberry Pi or
  x86 hardware measurements yet.
- At 50,000 events/s offered, this arm64 VM drops events (all counted), because
  every commit is fsynced. 20,000 events/s: 0 dropped.
