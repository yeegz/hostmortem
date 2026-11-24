# ADR-0011: The shareable report is a static HTML file, not a local web server

Status: Accepted (24 Nov 2026)

## Context
v0.2 needs a way for someone to see an incident without reading terminal
output, and to send it to someone who has no access to the box. The obvious
option is a local web UI served by `hmd` or `hm`. The threat model (T1, T6)
keeps the agent free of network listeners, and the target hosts are edge
boxes that are often reached over SSH only.

## Decision
`hm report` writes one self-contained HTML file: coverage, boots and every
incident's diagnosis, with the same escaping and strict CSP
(`default-src 'none'`) as `hm explain --html`. No scripts, no fonts, no
remote resources; incident sections expand with `<details>`. `--redact`
passes every stored string through the same IP/MAC redaction as
`hm export --redact`. Event rows get ids unique per incident, so windows
that share events can share one page.

## Consequences
- No listener, no auth, no port to firewall; the file works offline, can be
  copied off the box with `scp`, attached to a ticket, or opened on a phone.
- It is a snapshot, not live. A live UI (Phase 4, v1.0) is still planned and
  would follow ADR-0008's per-install token.
- Redaction is pattern based and deliberately over-eager; DNS names are
  already keyed-hashed unless listed in `[privacy] clear_names`.
