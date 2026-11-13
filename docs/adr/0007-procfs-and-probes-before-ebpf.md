# ADR-0007: procfs, rtnetlink, kmsg and active probes before eBPF

**Context.** eBPF probes are commoditised (Inspektor Gadget ships trace_dns and
trace_tcpretrans). The differentiator sits above the probes. eBPF also needs
BTF, a 5.8+ kernel for ring buffers, and CAP_BPF/CAP_PERFMON.

**Decision.** v0.1 uses only stable kernel interfaces: procfs/sysfs counters
(CPU, memory, PSI, `/proc/net/snmp`, `/proc/net/netstat`, conntrack stats),
rtnetlink link/addr/route notifications (with initial dump and ENOBUFS
resync), `/dev/kmsg` (OOM, NETDEV WATCHDOG, conntrack table full, link down),
cgroup v2 `memory.events`, `/proc` process polling, and active probes:
`getaddrinfo()` vs a direct UDP DNS query, ICMP to the gateway, TCP connect.
v0.2 adds four tracepoints (tcp_retransmit_skb, inet_sock_set_state,
oom:mark_victim, sched_process_exit) with fallbacks to these counters.

**Consequences.** v0.1 cannot attribute retransmissions to a connection or see
exit codes, and it misses processes that crash and restart within one poll
second. Each of these limits is reported as missing evidence in the diagnosis,
not hidden.
