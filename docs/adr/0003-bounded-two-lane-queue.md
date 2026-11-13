# ADR-0003: Bounded MPSC queue with two lanes; drops are evidence

**Context.** Five collector threads produce and one writer consumes. Under a
flood (retransmission storm, log spam) memory must stay bounded, and the
evidence an incident depends on must not be the thing that gets dropped.

**Decision.** `EventQueue`: a mutex + condition variable around two
`std::deque`s. Events with severity at or above warning go to the high lane
(capacity 4096), everything else to the low lane (16384). `push()` never
blocks. A full lane drops the incoming event and counts it per (source, lane).
At 80 % full `push()` returns `pressure` and the sampler halves its rate. The
writer turns drop counts into `pipeline.drop` events, so loss is visible in
the timeline (FR5).

**Alternatives.** Lock-free ring (rejected until profiling shows contention;
see PERFORMANCE.md); blocking producers (rejected: a stalled writer would
stall netlink reads, and the kernel then drops notifications we cannot count);
a single lane with drop-oldest (rejected: a sample flood evicts the OOM line).

**Invariant, tested under TSan:** for every pushed event, exactly one of
"popped once" or "counted as dropped" is true.
