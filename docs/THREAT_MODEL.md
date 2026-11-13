# Threat model (v0)

**Assets:** the evidence store (it reveals network topology, process names and
failure history), the agent's capabilities, and the integrity of diagnoses.

**Trust boundaries:** kernel → agent (trusted source, untrusted bytes: parsers
are bounds-checked and fuzzed); agent → store file; store → CLI user; `hm
ingest` input (fully untrusted); the network the prober talks to (responses
are untrusted).

**Attacker capabilities considered:** an unprivileged local user; anyone who
can send packets to the host; anyone who can hand the operator a JSONL file or
an HTML report; a compromised upstream DNS server.

| # | Threat | Mitigation in v0.1 | Verified by |
|---|---|---|---|
| T1 | Privilege escalation via the agent | Runs as dedicated user `hostmortem` with ambient CAP_SYSLOG and CAP_NET_RAW only; `NoNewPrivileges`, `ProtectSystem=strict`, syscall filter (`packaging/systemd/hmd.service`); no shell-outs; `hm doctor` shows granted vs needed | Unit file review; systemd-analyze security not yet run |
| T2 | Unauthorised read of history | Store dir 0750 via `StateDirectoryMode`, umask 0027; no network listener in v0.1 (ADR-0008) | Manual |
| T3 | Malicious telemetry / ingest | JSON line ≤ 64 KiB, ≤ 64 scalar attrs, ≤ 4 KiB strings, schema version checked, rejected lines counted; ingest refuses a live store and marks its store SYNTHETIC; the agent refuses to record into a SYNTHETIC store | `JsonCodec.RejectsMalformedInput`, fuzz targets |
| T4 | Parser memory safety (netlink, kmsg, procfs, DNS, config) | `NLMSG_OK`/`RTA_OK` bounds, alignment check, `from_chars`, explicit lengths; libFuzzer + ASan/UBSan targets | `tests/fuzz/*`, `NetlinkParse.TruncatedBufferIsMalformedNotACrash` |
| T5 | DNS response spoofing affecting diagnosis | Probe uses a connected UDP socket (kernel filters the source), random-ish id check, ignores non-responses; does not parse answers | Code review |
| T6 | Log/terminal/HTML injection via interface names, kernel messages, process names | Agent log escapes control bytes; CLI strips control bytes; HTML escapes all stored strings, strict CSP `default-src 'none'`, no scripts | `Report.HtmlEscapesStoredStringsAndHasNoScript`, `Report.TextStripsControlCharacters` |
| T7 | Sensitive metadata | DNS names keyed-hashed (SipHash-2-4, per-install key) unless allowlisted; no payload capture; `hm export --redact` masks IPv4/IPv6 literals | `SipHash.*` |
| T8 | Denial of service (event floods, slow consumer) | Bounded queue with reserved high lane; writer pending bounded; sampler backs off at 80 %; retention cap | `EventQueue.*`, `Writer.DiskFullShedsLowPriorityAndRecovers` |
| T9 | Evidence tampering | Out of scope for v0.1. Planned: SHA-256 of exports, hash-chained batches (research extension) | — |
| T10 | Clock manipulation hiding or reordering events | Ordering ignores wall time (ADR-0005); steps recorded as events; age retention ignores unsynced wall time | `Store.AgeRetentionIgnoresUnsyncedWallTime` |
| T11 | Supply chain | Distro packages only (SQLite, nlohmann-json, GoogleTest); no network fetch at build time. Planned: SBOM, signed artefacts, Dependabot/CodeQL | — |

**Known gaps:** no authentication on the store beyond file permissions; the
lab requires a privileged container and must never run on a production
host; `systemd-analyze security` has not been run on a real systemd host yet.
