# Changelog

All notable changes. Versions follow semantic versioning. **v0.1.0 is prepared
but not tagged or published**: releasing and opening the source are the
owner's decisions.

## [0.1.0] - unreleased

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
- Real-fault lab (8 scenarios in network namespaces), nested-QEMU hard-reset
  test, 13 synthetic scenario fixtures, 6 libFuzzer targets, sanitizer presets,
  CI on amd64 and arm64.
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

### Known limits
- No eBPF yet (v0.2): retransmissions are host-wide; exit codes and signals
  aren't visible.
- Hard reset tested in a VM, not as a physical power cut; no Raspberry Pi or
  x86 hardware measurements yet.
- At 50,000 events/s offered, this arm64 VM drops events (all counted), because
  every commit is fsynced. 20,000 events/s: 0 dropped.
