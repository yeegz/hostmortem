// The event vocabulary. Collectors emit these kinds; playbooks match on them.
// Adding a kind is additive within schema v1; renaming one is a breaking change.
#pragma once

namespace hm::kind {

// Agent self-observation (source "hmd")
inline constexpr const char* agent_start = "agent.start";
inline constexpr const char* agent_stop = "agent.stop";
inline constexpr const char* collector_status = "collector.status";  // subject=collector, state, reason
inline constexpr const char* pipeline_drop = "pipeline.drop";        // origin, lane, count
inline constexpr const char* store_recovered = "store.recovered";
inline constexpr const char* store_degraded = "store.degraded";

// procfs/sysfs sampler (source "procfs")
inline constexpr const char* sys_cpu = "sys.cpu";          // busy_pct
inline constexpr const char* sys_mem = "sys.mem";          // mem_total_kb, mem_available_kb, swap_free_kb
inline constexpr const char* sys_psi = "sys.psi";          // cpu_some10, mem_some10, mem_full10, io_some10
inline constexpr const char* net_tcp = "net.tcp";          // out_segs, retrans_segs, retrans_ppm, attempt_fails, estab_resets, timeouts
inline constexpr const char* net_udp = "net.udp";          // in_errors, rcvbuf_errors
inline constexpr const char* net_conntrack = "net.conntrack";  // insert_failed, drop, early_drop (deltas)
inline constexpr const char* clock_state = "clock.state";  // synced, max_error_us
inline constexpr const char* clock_step = "clock.step";    // step_ms

// rtnetlink (source "netlink")
inline constexpr const char* link_state = "link.state";    // ifname, oper, carrier, up
inline constexpr const char* addr_add = "addr.add";
inline constexpr const char* addr_del = "addr.del";
inline constexpr const char* route_add = "route.add";      // dst, gateway, default
inline constexpr const char* route_del = "route.del";

// kernel log (source "kmsg")
inline constexpr const char* kernel_oom = "kernel.oom";    // pid, comm
inline constexpr const char* kernel_netdev_watchdog = "kernel.netdev_watchdog";  // ifname
inline constexpr const char* kernel_conntrack_full = "kernel.conntrack_full";
inline constexpr const char* kernel_link_down = "kernel.link_down";  // ifname
inline constexpr const char* kernel_log = "kernel.log";    // prio, msg (error and worse)

// Active probes (source "prober"); every probe carries ok and latency_ms
inline constexpr const char* probe_dns_resolver = "probe.dns.resolver";  // name, err
inline constexpr const char* probe_dns_direct = "probe.dns.direct";      // server, name, rcode, err
inline constexpr const char* probe_icmp = "probe.icmp";                  // target, role(gateway|target), rtt_ms
inline constexpr const char* probe_tcp = "probe.tcp";                    // target, err

// Process watch (source "process")
inline constexpr const char* process_start = "process.start";    // name, pid
inline constexpr const char* process_exit = "process.exit";      // name, pid
inline constexpr const char* process_restart = "process.restart";  // name, old_pid, new_pid
inline constexpr const char* cgroup_oom_kill = "cgroup.oom_kill";  // name, delta

}  // namespace hm::kind
