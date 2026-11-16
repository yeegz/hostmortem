# ADR-0010: incidents commit in the same transaction as the events that open them

**Context.** The writer committed a batch (`Store::insert_batch`) and only then
ran the detector and wrote `open_incident` / `close_incident` as separate
autocommit statements. A crash or power cut in that gap kept the evidence but
lost the incident row. On restart the agent re-adopts only incidents already
marked open, and nothing replays committed events through the detector, so a
symptom that had stopped by then was never recorded as an incident. Found
while writing interview answers ("what if we crash between these two writes?"),
17 Nov 2026.

**Decision.** `insert_batch` takes an optional `before_commit` hook that runs
inside the batch transaction after ids are assigned. The writer runs detection
in that hook, so incident writes commit atomically with their events. The
detector is a value type: the writer copies it before the batch and restores
it if the batch rolls back, because the same events are offered again next
cycle and must not be counted twice.

A failing incident statement does not abort the batch: SQLite rolls back only
that statement, the error is logged, and the evidence still commits. Evidence
has priority over the incident row, which can be rebuilt from it.

**Rejected.** Replaying recent events through the detector at startup. It also
restores detector state such as RTT baselines, but needs a replay window per
rule and can double-open incidents; the transaction closes the gap with less code.

**Test.** `Store.BeforeCommitWritesShareTheBatchTransaction`: an incident written
in the hook commits with the batch, and a hook failure rolls back both.
