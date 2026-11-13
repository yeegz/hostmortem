# Architecture

Hostmortem is a single-host flight recorder: one agent process (`hmd`) and one
CLI (`hm`) sharing a SQLite file. There are no services, brokers or cloud
components.

```
 procfs/sysfs ---------> [ProcfsSampler]  --+
 rtnetlink   ----------> [NetlinkCollector]--+
 /dev/kmsg   ----------> [KmsgCollector]   --+--> EventQueue (bounded, 2 lanes,
 /proc + cgroup v2 ----> [ProcessWatch]    --+     drop counters per source)
 network (active) -----> [Prober]          --+              |
                         (+ resolver worker)                 v
                                                      [Writer] -- batch commit --> events.db (SQLite WAL)
                                                         |                           ^      ^
                                                         v                           |      |
                                                     [Detector] -- open/close ------+       |
                                                                   incidents                |
                                         hm timeline | incidents | explain | doctor --------+
                                                          |
                                                    [Correlator + 12 playbooks] -> text / JSON / HTML
```

## Threads

| Thread | Owns | Blocks on | Stops via |
|---|---|---|---|
| main | signalfd, lifecycle | `poll(signalfd)` | SIGTERM/SIGINT/SIGHUP |
| procfs | sampler state | interruptible sleep | `std::stop_token` |
| netlink | rtnetlink socket | `poll` 500 ms | stop token |
| kmsg | `/dev/kmsg` fd | `poll` 500 ms | stop token |
| prober | probe sockets | `poll` per probe (≤ timeout) | stop token |
| resolver worker | nothing shared but a 1-slot handoff | `getaddrinfo()` | stop token (after the call returns) |
| process | `/proc` scan state | interruptible sleep (1 s) | stop token |
| writer | the only read-write `Store` | `EventQueue::pop_batch` | queue closed and drained |

The threads share nothing but the queue. Each collector has an `Emitter`,
which stamps clocks and a per-source sequence number and pushes into the
queue. Only the writer touches the store.

## Lifecycle

1. Block SIGTERM/SIGINT/SIGHUP in all threads; create a signalfd.
2. Read `boot_id`, open the store (quick_check → recover if needed → migrate),
   register the boot, load the highest seq per source for this boot (so a
   restart within the boot continues numbering), load the name-hash key.
3. Emit `agent.start` (with `previous_boot` when the boot changed), and
   `store.recovered` if the store was rebuilt.
4. Adopt an incident left open by a previous run (it may have started before a
   reboot).
5. Start the writer, then the collectors.
6. Wait on the signalfd. Shutdown order: request stop on collectors and join
   them, emit `agent.stop`, close the queue, and let the writer drain, commit
   and join.

## Data model

See `src/store/store.cpp` (migration 1). Key points:

- `events`: `UNIQUE(boot_id, source, seq)` makes ingest idempotent. `attrs` holds
  JSON scalars only (validated on ingest: 64 attrs maximum, 4 KiB strings,
  64 KiB per line).
- `boots`: an `AUTOINCREMENT` ordinal gives the cross-boot order.
- `incidents`, `hypotheses`, `evidence`: `evidence.event_id` is NULL for
  "missing" evidence; retention never deletes an event that evidence or an
  incident trigger references.

## Event vocabulary

`src/core/kinds.hpp` is the single list. Collectors emit these kinds;
playbooks match on them.

## Analysis

- **Detector** (`analysis/detector.cpp`): a pure function of the committed
  event stream. Symptoms are: a probe failure streak, partial ICMP loss, an
  RTT excursion over a learned baseline, link down or lost carrier, the default
  route removed, OOM (kernel or cgroup), NIC watchdog, conntrack table full, a
  clock step, a process exit or restart, a restart burst, a retransmission
  excursion over a learned baseline, and PSI over thresholds. One incident at a
  time; it closes after `incident_quiet` with no symptom and survives reboots.
  `hm ingest` replays the same detector, so lab fixtures and live runs produce
  identical incidents.
- **Correlator** (`analysis/correlator.cpp`): the window runs from 60 s before
  the incident opened to its close (possibly in a later boot). The baseline is
  the 10 minutes before that. Collector availability comes from
  `collector.status` events. It runs the 12 playbooks (`analysis/playbooks.cpp`,
  documented in `docs/playbooks.md`), ranks them, and applies the "unknown" rule.

## Dependency direction

`core` ← `pipeline` ← `store` ← `analysis` ← `collectors` ← `agent`/`cli`.
`hm_core` (core, store, analysis, pure parsers) builds on any POSIX host;
`hm_linux` adds the Linux-only collectors and agent.
