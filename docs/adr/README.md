# Architecture decision records

One file per decision: context, decision, alternatives, consequences. Never
rewrite an accepted ADR; supersede it with a new one.

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-narrow-scope-and-name.md) | Narrow single-host flight recorder named Hostmortem | Accepted |
| [0002](0002-cpp20-own-result-type.md) | C++20 with a small `Result<T>` instead of exceptions or C++23 | Accepted |
| [0003](0003-bounded-two-lane-queue.md) | Bounded MPSC queue, two priority lanes, drops recorded as events | Accepted |
| [0004](0004-sqlite-wal-durability.md) | SQLite in WAL mode, `synchronous=NORMAL`, batched commits | Accepted |
| [0005](0005-time-and-ordering.md) | Order by boot ordinal then CLOCK_BOOTTIME; wall time is display-only | Accepted |
| [0006](0006-deterministic-playbooks.md) | Deterministic playbooks with integer evidence points; no ML or LLM | Accepted |
| [0007](0007-procfs-and-probes-before-ebpf.md) | procfs, netlink, kmsg and active probes first; eBPF in v0.2 | Accepted |
| [0008](0008-cli-reads-store-directly.md) | CLI reads SQLite directly; no API socket in v0.1 | Accepted |
| [0009](0009-synchronous-full.md) | synchronous=FULL after a VM hard reset lost a boot of evidence | Accepted (supersedes part of 0004) |
