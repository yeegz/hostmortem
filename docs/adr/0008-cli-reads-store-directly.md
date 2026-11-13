# ADR-0008: The CLI reads SQLite directly in v0.1

**Context.** The blueprint puts a Unix-socket API in front of the store. In
v0.1 the only client is `hm` on the same host.

**Decision.** `hm` opens the store read-only (WAL allows concurrent readers
while `hmd` writes). `hm explain` opens it read-write only to persist
hypotheses, and falls back to read-only. Collector health is read from
`collector.status` events rather than queried live.

**Consequences.** No extra attack surface, thread or protocol in v0.1. A reader
needs filesystem access to the store, so the store is mode 0640, group
`hostmortem`. The Unix socket (0660) and the 127.0.0.1 HTTP API with a
per-install token arrive with the web UI in v1.0.
