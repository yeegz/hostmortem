#!/usr/bin/env bash
# End to end on a real kernel: a live hmd records `hm drill`, the incident is
# diagnosed, and `hm report` writes a self-contained HTML file with it.
set -euo pipefail
HMD="$1"
HM="$2"
work="$(mktemp -d)"
pid=""
cleanup() { if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; rm -rf -- "${work:?}"; }
trap cleanup EXIT
cat > "$work/hmd.conf" <<CONF
[store]
path = $work/events.db
commit_interval = 1s
[probe]
interval = 60s
icmp_targets = 127.0.0.1
CONF
"$HMD" --config "$work/hmd.conf" --no-kmsg --run-for 60s 2> "$work/hmd.log" &
pid=$!
for _ in $(seq 50); do [[ -f "$work/events.db" ]] && break; sleep 0.1; done
sleep 1
"$HM" --db "$work/events.db" drill | tee "$work/drill.out"
grep -q "drill: recorded" "$work/drill.out"
id="$(sed -n 's/.*hm explain \([0-9]*\).*/\1/p' "$work/drill.out")"
"$HM" --db "$work/events.db" explain "$id" | tee "$work/explain.out" | head -5
grep -q "process.exit \[hm-drill\]" "$work/explain.out"
"$HM" --db "$work/events.db" report -o "$work/report.html"
grep -q "hm-drill" "$work/report.html"
grep -q "default-src 'none'" "$work/report.html"
if grep -qi "<script" "$work/report.html"; then echo "FAIL: script in report"; exit 1; fi
"$HM" --db "$work/events.db" report --redact -o "$work/redacted.html"
if grep -q "127\.0\.0\.1" "$work/redacted.html"; then echo "FAIL: address in redacted report"; exit 1; fi
echo "ok drill_e2e (incident $id)"
