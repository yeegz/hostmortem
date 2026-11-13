#!/usr/bin/env bash
# Hostmortem fault-injection lab: REAL faults in network namespaces.
#
#   hm-client (192.0.2.2) ---veth--- hm-up (192.0.2.1: gateway, dnsmasq DNS, TCP :8080)
#
# hmd runs inside hm-client. Each scenario: fresh store, healthy baseline,
# inject a fault, recover, wait for the incident to close, then check
# `hm explain --json` against the expected diagnosis.
#
# Needs root + CAP_NET_ADMIN (run via scripts/lab.sh, which uses a
# privileged throwaway container). Never run this on a machine you care about.
#
# Usage: lab/run_lab.sh [scenario ...]     (default: all)
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
BIN="${BIN:-build/debug}"
CHECK="tests/integration/check_diagnosis.py"
mkdir -p lab/out
AGENT_PID=""
INJECT_UP=0
RESULTS=()

log() { printf '[lab %s] %s\n' "$(date +%T)" "$*"; }

# Removes one scenario's store and logs. Literal directory prefix on purpose.
clean_store() {
  local n="${1:?scenario name required}"
  rm -f -- lab/out/"$n".db lab/out/"$n".db-wal lab/out/"$n".db-shm lab/out/"$n".agent.log
}

teardown() {
  if [[ -n "$AGENT_PID" ]]; then
    kill -TERM "$AGENT_PID" 2>/dev/null
    wait "$AGENT_PID" 2>/dev/null
  fi
  AGENT_PID=""
  pkill -f "dnsmasq --conf-file=lab/out/dnsmasq.conf" 2>/dev/null
  pkill -f "http.server 8080" 2>/dev/null
  pkill -x victim 2>/dev/null
  ip netns del hm-client 2>/dev/null
  ip netns del hm-up 2>/dev/null
  rm -f -- /etc/netns/hm-client/resolv.conf
  true
}
trap teardown EXIT

setup_net() {
  teardown
  ip netns add hm-up
  ip netns add hm-client
  ip link add name veth-c type veth peer name veth-u
  ip link set veth-c netns hm-client
  ip link set veth-u netns hm-up
  ip -n hm-up addr add 192.0.2.1/24 dev veth-u
  ip -n hm-client addr add 192.0.2.2/24 dev veth-c
  ip -n hm-up link set lo up
  ip -n hm-client link set lo up
  ip -n hm-up link set veth-u up
  ip -n hm-client link set veth-c up
  ip -n hm-client route add default via 192.0.2.1
  printf 'no-resolv\nno-hosts\nlisten-address=192.0.2.1\nbind-interfaces\naddress=/example.com/192.0.2.80\n' \
    > lab/out/dnsmasq.conf
  ip netns exec hm-up dnsmasq --conf-file=lab/out/dnsmasq.conf --pid-file="$PWD/lab/out/dnsmasq.pid"
  ip netns exec hm-up python3 -m http.server 8080 --bind 192.0.2.1 >/dev/null 2>&1 &
  mkdir -p /etc/netns/hm-client
  echo "nameserver 192.0.2.1" > /etc/netns/hm-client/resolv.conf
  sleep 1
  # Pre-flight: the healthy network must really be healthy, or every
  # diagnosis below is measuring a broken lab.
  ip netns exec hm-client getent hosts example.com >/dev/null || { log "SETUP FAIL: DNS"; return 1; }
  ip netns exec hm-client ping -c1 -W1 192.0.2.1 >/dev/null || { log "SETUP FAIL: gateway"; return 1; }
  ip netns exec hm-client python3 -c 'import socket; socket.create_connection(("192.0.2.1", 8080), 1)' \
    || { log "SETUP FAIL: tcp"; return 1; }
}

write_conf() {  # $1 = scenario
  cat > "lab/out/$1.conf" <<CONF
[store]
path = $PWD/lab/out/$1.db
commit_interval = 5s
[sampler]
interval = 1s
[probe]
interval = 2s
timeout = 1s
dns_names = example.com
dns_servers = 192.0.2.1
icmp_targets = gateway
tcp_targets = 192.0.2.1:8080
[watch]
processes = victim
[privacy]
clear_names = example.com
[detector]
incident_quiet = 20s
CONF
}

start_agent() {  # $1 = scenario, $2... = optional setpriv args
  # `ip netns exec` remounts /sys without the cgroup tree; mount cgroup2 back
  # inside that mount namespace so the agent sees what it would on a real host.
  local name="$1"
  shift
  ip netns exec hm-client sh -c 'mount -t cgroup2 none /sys/fs/cgroup 2>/dev/null; exec "$@"' sh \
    ${1:+setpriv "$@" --} "$BIN/hmd" --config "lab/out/$name.conf" >>"lab/out/$name.agent.log" 2>&1 &
  AGENT_PID=$!
}

stop_agent() {
  kill -TERM "$AGENT_PID" 2>/dev/null
  wait "$AGENT_PID" 2>/dev/null
  AGENT_PID=""
}

finish() {  # $1 = scenario, $2 = expectation
  local name="$1" expect="$2" db="lab/out/$1.db" id why
  # Grade the incident opened at or after the injection. The kernel log is read
  # from boot, so OOM lines from earlier scenarios can open an older (real but
  # unrelated) incident in a fresh store; grading the first one was wrong.
  id="$("$BIN/hm" --db "$db" incidents --json | python3 -c '
import json, sys
t = float(sys.argv[1]) * 1e9 - 5e9
d = json.load(sys.stdin)
later = [i for i in d if i["opened_mono_ns"] >= t]
print(later[0]["id"] if later else "")' "$INJECT_UP")"
  "$BIN/hm" --db "$db" incidents > "lab/out/$name.incidents.txt"
  if [[ -z "$id" ]]; then
    log "FAIL $name: no incident opened"
    RESULTS+=("FAIL $name: no incident")
    return
  fi
  log "grading incident $id (opened after the injection)"
  "$BIN/hm" --db "$db" explain "$id" > "lab/out/$name.explain.txt"
  "$BIN/hm" --db "$db" explain "$id" --json > "lab/out/$name.explain.json"
  "$BIN/hm" --db "$db" explain "$id" --html "lab/out/$name.html" 2>/dev/null
  if why="$(python3 "$CHECK" "lab/out/$name.explain.json" "$expect synthetic=0")"; then
    log "ok   $name ($expect)"
    RESULTS+=("ok   $name")
  else
    log "FAIL $name: $why"
    RESULTS+=("FAIL $name: $why")
  fi
}

scenario() {  # name healthy_s fault_s after_s expect inject recover [setpriv args...]
  local name="$1" healthy="$2" fault="$3" after="$4" expect="$5" inject="$6" recover="$7"
  shift 7
  log "---- $name"
  clean_store "$name"
  if ! setup_net; then
    RESULTS+=("FAIL $name: lab setup")
    return
  fi
  write_conf "$name"
  start_agent "$name" "$@"
  sleep "$healthy"
  INJECT_UP="$(cut -d' ' -f1 /proc/uptime)"
  log "inject: $inject"
  eval "$inject"
  sleep "$fault"
  log "recover: $recover"
  eval "$recover"
  sleep "$after"
  stop_agent
  finish "$name" "$expect"
}

s_dns_upstream() {
  scenario dns_upstream 12 20 30 "leading=upstream_dns ruled_out=local_resolver,link_or_gateway" \
    "ip netns exec hm-up iptables -I INPUT -p udp --dport 53 -j DROP" \
    "ip netns exec hm-up iptables -D INPUT -p udp --dport 53 -j DROP"
}

s_dns_local() {
  # Break only the local resolver path (resolv.conf points at a dead stub);
  # direct queries to 192.0.2.1 keep working. Write in place: the file is
  # bind-mounted into the namespace, so its inode must not change.
  scenario dns_local 12 20 30 "leading=local_resolver ruled_out=link_or_gateway" \
    "echo 'nameserver 127.0.0.1' > /etc/netns/hm-client/resolv.conf" \
    "echo 'nameserver 192.0.2.1' > /etc/netns/hm-client/resolv.conf"
}

s_link_down() {
  scenario link_down 12 20 30 "leading=link_or_gateway ruled_out=upstream_dns" \
    "ip -n hm-client link set veth-c down" \
    "ip -n hm-client link set veth-c up && ip -n hm-client route replace default via 192.0.2.1"
}

s_latency() {
  # The detector needs healthy probes to learn the baseline RTT, and the
  # playbook compares against the 10 minutes before the incident window
  # (which starts 60 s before the incident opens), hence the long warm-up.
  scenario latency 75 20 30 "leading=path_degradation" \
    "ip netns exec hm-up tc qdisc add dev veth-u root netem delay 300ms 50ms" \
    "ip netns exec hm-up tc qdisc del dev veth-u root"
}

s_packet_loss() {
  scenario packet_loss 12 30 30 "supported=path_degradation" \
    "ip netns exec hm-up tc qdisc add dev veth-u root netem loss 50%" \
    "ip netns exec hm-up tc qdisc del dev veth-u root"
}

s_kill9() {
  scenario kill9 8 5 30 "leading=unknown ruled_out=oom_kill possible=process_exit" \
    "cp /bin/sleep /tmp/victim; ip netns exec hm-client /tmp/victim 1000 & sleep 3; pkill -9 -x victim" \
    "true"
}

s_missing_kmsg() {
  # Same kill, but hmd runs without CAP_SYSLOG *and* CAP_SYS_ADMIN (the kernel's
  # check_syslog_permissions() still accepts CAP_SYS_ADMIN "for historical
  # reasons"; dropping only CAP_SYSLOG left /dev/kmsg readable, found in this
  # lab). With dmesg_restrict=1 the log is then unreadable, so OOM must NOT be
  # ruled out.
  scenario missing_kmsg 8 5 30 "leading=unknown possible=oom_kill" \
    "cp /bin/sleep /tmp/victim; ip netns exec hm-client /tmp/victim 1000 & sleep 3; pkill -9 -x victim" \
    "true" \
    --inh-caps=-syslog,-sys_admin --bounding-set=-syslog,-sys_admin
}

s_agent_crash() {
  # SIGKILL the agent mid-incident and restart it in the same boot. Checks:
  # store passes quick_check, the incident continues, and the evidence lost is
  # bounded by the commit interval.
  local name=agent_crash kill_up
  log "---- $name"
  clean_store "$name"
  if ! setup_net; then
    RESULTS+=("FAIL $name: lab setup")
    return
  fi
  write_conf "$name"
  start_agent "$name"
  sleep 12
  INJECT_UP="$(cut -d' ' -f1 /proc/uptime)"
  ip netns exec hm-up iptables -I INPUT -p udp --dport 53 -j DROP
  sleep 10
  kill_up="$(cut -d' ' -f1 /proc/uptime)"
  kill -KILL "$AGENT_PID"
  wait "$AGENT_PID" 2>/dev/null
  AGENT_PID=""
  log "hmd SIGKILLed at uptime ${kill_up}s"
  sleep 3
  start_agent "$name"
  sleep 12
  ip netns exec hm-up iptables -D INPUT -p udp --dport 53 -j DROP
  sleep 30
  stop_agent
  python3 - "lab/out/$name.db" "$kill_up" "$INJECT_UP" <<'PY' | tee "lab/out/$name.loss.txt"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
kill_ns = float(sys.argv[2]) * 1e9
print("quick_check:", db.execute("PRAGMA quick_check").fetchone()[0])
last = db.execute("SELECT MAX(mono_ns) FROM events WHERE mono_ns < ?", (kill_ns,)).fetchone()[0]
print(f"newest event committed before SIGKILL was captured {(kill_ns - last) / 1e9:.2f} s before the kill")
print("agent.start events:", db.execute("SELECT COUNT(*) FROM events WHERE kind='agent.start'").fetchone()[0])
# Older kernel-log lines (from earlier scenarios in this boot) can open their
# own incidents; the fault must yield exactly one, across the agent restart.
inject_ns = float(sys.argv[3]) * 1e9 - 5e9
print("incidents:", db.execute("SELECT COUNT(*) FROM incidents WHERE opened_mono >= ?", (inject_ns,)).fetchone()[0])
PY
  finish "$name" "leading=upstream_dns"
  if grep -q "quick_check: ok" "lab/out/$name.loss.txt" && grep -q "incidents: 1" "lab/out/$name.loss.txt"; then
    RESULTS+=("ok   agent_crash store-integrity")
  else
    RESULTS+=("FAIL agent_crash store-integrity")
  fi
}

# Puts a 64 MiB memory limit on cgroup "hmlab", or fails. Cgroup v2 refuses to
# enable a controller for children while the parent holds processes, and new
# processes keep appearing, so this retries and then verifies the limit.
make_oom_cgroup() {
  mkdir -p /sys/fs/cgroup/lab-init /sys/fs/cgroup/hmlab
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    while read -r pid; do echo "$pid" > /sys/fs/cgroup/lab-init/cgroup.procs 2>/dev/null; done < /sys/fs/cgroup/cgroup.procs
    if echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null; then break; fi
    sleep 0.2
  done
  echo 64M > /sys/fs/cgroup/hmlab/memory.max 2>/dev/null || return 1
  echo 0 > /sys/fs/cgroup/hmlab/memory.swap.max 2>/dev/null || true
  [[ "$(cat /sys/fs/cgroup/hmlab/memory.max 2>/dev/null)" == "67108864" ]]
}

s_oom_kill() {
  # A real cgroup OOM kill: the victim (a renamed python3, so its comm is
  # "victim") allocates inside a cgroup capped at 64 MiB. It is only started
  # if the limit is verified; the victim also caps itself at 512 MiB.
  log "---- oom_kill"
  if ! make_oom_cgroup; then
    log "FAIL oom_kill: could not apply the 64 MiB cgroup limit; victim NOT started"
    RESULTS+=("FAIL oom_kill: cgroup limit not applied (safe abort)")
    return
  fi
  # The victim is moved into the cgroup from outside its network namespace:
  # `ip netns exec` remounts /sys, so a process inside cannot see the cgroup
  # tree (the first version tried that, silently failed, and only the victim's
  # own 512 MiB cap stopped a system-wide OOM). The victim sleeps 3 s first.
  scenario oom_kill 10 15 30 "leading=oom_kill supported=oom_kill" \
    "cp \$(command -v python3) /tmp/victim;
     ip netns exec hm-client /tmp/victim lab/oom_victim.py &
     echo \$! > /sys/fs/cgroup/hmlab/cgroup.procs;
     log \"victim \$! moved into hmlab: \$(grep hmlab /proc/\$!/cgroup)\"" \
    "true"
  # The kernel log is read from boot, so an OOM line from an earlier run could
  # satisfy the diagnosis on its own (it did once: a false pass). Require the
  # live cgroup counter, which only this kill can raise.
  local fresh
  fresh="$("$BIN/hm" --db lab/out/oom_kill.db timeline --all --kind cgroup.oom_kill | grep -c 'cgroup.oom_kill')"
  if [[ "$fresh" -lt 1 ]]; then
    log "FAIL oom_kill: no cgroup.oom_kill event, so the diagnosis may rest on an old kernel line"
    RESULTS+=("FAIL oom_kill: no fresh cgroup OOM evidence")
  fi
}

s_cpu_saturation() {
  # Twelve busy loops on four vCPUs: run-queue pressure shows in PSI.
  scenario cpu_saturation 12 30 30 "leading=resource_pressure" \
    "for i in \$(seq 12); do ip netns exec hm-client sh -c 'while :; do :; done' & done" \
    "pkill -f \"while :; do :; done\" || true"
}

s_restart_loop() {
  # A service that keeps dying: kill -9 and restart four times in ~20 s.
  scenario restart_loop 10 25 30 "leading=restart_loop" \
    "cp /bin/sleep /tmp/victim;
     for i in 1 2 3 4; do ip netns exec hm-client /tmp/victim 1000 & sleep 3; pkill -9 -x victim; sleep 2; done;
     ip netns exec hm-client /tmp/victim 1000 &" \
    "pkill -9 -x victim || true"
}

ALL=(dns_upstream dns_local link_down kill9 missing_kmsg packet_loss latency agent_crash oom_kill cpu_saturation restart_loop)
SEL=("$@")
[[ ${#SEL[@]} -eq 0 ]] && SEL=("${ALL[@]}")
log "kernel $(uname -r), $(nproc) CPUs; binaries from $BIN"
for s in "${SEL[@]}"; do "s_$s"; done
echo
printf '%s\n' "${RESULTS[@]}" | tee lab/out/summary.txt
! grep -q '^FAIL' lab/out/summary.txt
