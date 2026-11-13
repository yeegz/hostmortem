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
  local name="$1"
  shift
  if [[ $# -gt 0 ]]; then
    ip netns exec hm-client setpriv "$@" -- "$BIN/hmd" --config "lab/out/$name.conf" >>"lab/out/$name.agent.log" 2>&1 &
  else
    ip netns exec hm-client "$BIN/hmd" --config "lab/out/$name.conf" >>"lab/out/$name.agent.log" 2>&1 &
  fi
  AGENT_PID=$!
}

stop_agent() {
  kill -TERM "$AGENT_PID" 2>/dev/null
  wait "$AGENT_PID" 2>/dev/null
  AGENT_PID=""
}

finish() {  # $1 = scenario, $2 = expectation
  local name="$1" expect="$2" db="lab/out/$1.db" id why
  id="$("$BIN/hm" --db "$db" incidents --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[0]["id"] if d else "")')"
  "$BIN/hm" --db "$db" incidents > "lab/out/$name.incidents.txt"
  if [[ -z "$id" ]]; then
    log "FAIL $name: no incident opened"
    RESULTS+=("FAIL $name: no incident")
    return
  fi
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
  python3 - "lab/out/$name.db" "$kill_up" <<'PY' | tee "lab/out/$name.loss.txt"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
kill_ns = float(sys.argv[2]) * 1e9
print("quick_check:", db.execute("PRAGMA quick_check").fetchone()[0])
last = db.execute("SELECT MAX(mono_ns) FROM events WHERE mono_ns < ?", (kill_ns,)).fetchone()[0]
print(f"newest event committed before SIGKILL was captured {(kill_ns - last) / 1e9:.2f} s before the kill")
print("agent.start events:", db.execute("SELECT COUNT(*) FROM events WHERE kind='agent.start'").fetchone()[0])
print("incidents:", db.execute("SELECT COUNT(*) FROM incidents").fetchone()[0])
PY
  finish "$name" "leading=upstream_dns"
  if grep -q "quick_check: ok" "lab/out/$name.loss.txt" && grep -q "incidents: 1" "lab/out/$name.loss.txt"; then
    RESULTS+=("ok   agent_crash store-integrity")
  else
    RESULTS+=("FAIL agent_crash store-integrity")
  fi
}

ALL=(dns_upstream dns_local link_down kill9 missing_kmsg packet_loss latency agent_crash)
SEL=("$@")
[[ ${#SEL[@]} -eq 0 ]] && SEL=("${ALL[@]}")
log "kernel $(uname -r), $(nproc) CPUs; binaries from $BIN"
for s in "${SEL[@]}"; do "s_$s"; done
echo
printf '%s\n' "${RESULTS[@]}" | tee lab/out/summary.txt
! grep -q '^FAIL' lab/out/summary.txt
