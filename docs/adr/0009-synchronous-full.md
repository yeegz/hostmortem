# ADR-0009: synchronous=FULL (supersedes the synchronous choice in ADR-0004)

**Context.** ADR-0004 chose WAL with `synchronous=NORMAL` and claimed a power
cut loses at most one commit interval. The nested-VM hard-reset test
(`scripts/hard-reset-test.sh`, 13 Nov 2026) disproved that: after a QEMU
`system_reset` 7 s into the agent's first run, **all of boot 1's evidence was
gone**, including an already committed incident. In WAL mode, NORMAL never
fsyncs a commit, only checkpoints, so committed transactions can sit in the
kernel page cache far longer than 5 s. The earlier SIGKILL lab test could not
catch this, because the kernel keeps its page cache when a process dies.

**Decision.** `synchronous=FULL`: every commit fsyncs the WAL. The same test
then passed: 2 boots recorded, 0.59 s of evidence lost (bound 5 s), and one
incident spanning both boots.

**Cost.** An fsync per commit. With 500-event batches that cut throughput
sharply in this VM (partial runs: about 7,000 events/s achieved at 20,000
offered, with drops). Mitigation in progress: commits spaced at least 200 ms
apart so each fsync covers everything queued (`WriterConfig::min_commit_spacing`).
**Not yet re-measured**; see docs/HANDOFF.md.

**Rejected.** Staying on NORMAL: it fails the project's own durability
requirement. `PRAGMA wal_checkpoint` on a timer: still loses up to the timer
period, and is more complex than FULL.
