# ADR-0005: Order by boot ordinal, then CLOCK_BOOTTIME

**Context.** Edge boxes often have no RTC. They boot with a wrong wall clock and
step it when NTP arrives, sometimes by years. Ordering by wall time puts events
in the wrong order exactly when a timeline matters most.

**Decision.** Every event carries `boot_id` (from
`/proc/sys/kernel/random/boot_id`), a per-source sequence number,
`CLOCK_BOOTTIME` (`mono_ns`), `CLOCK_REALTIME` (`wall_ns`) and a wall-clock
quality flag from `adjtimex()`. Within a boot, `mono_ns` orders events.
Across boots, the boot ordinal (insertion order of `boot_id`s) orders them.
Wall time is shown for humans only and flagged when unsynchronised. The sampler
records clock steps (`clock.step`: REALTIME moved more than 1 s relative to
BOOTTIME between samples), which makes the TLS "certificate not yet valid"
failure mode diagnosable.

**Why BOOTTIME and not MONOTONIC.** BOOTTIME keeps counting through suspend;
MONOTONIC does not. Kernel printk timestamps are MONOTONIC-based, so kmsg
events carry `ts_source=printk` and are slightly misplaced only after a
suspend.

**Consequences.** Mono offsets are not comparable across boots. The detector
restarts its quiet timer at the first event of a new boot instead of
subtracting across boots.
