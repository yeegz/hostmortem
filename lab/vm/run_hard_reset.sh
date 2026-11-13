#!/usr/bin/env bash
# Hard-reset test: a real Linux VM (QEMU, Ubuntu 6.8 kernel, ext4 on a virtio
# disk) is reset mid-incident with QEMU's system_reset: the equivalent of the
# reset button. The guest gets no shutdown, no sync, and loses its page cache.
#
# What this is NOT: a physical power cut. QEMU's host-side cache keeps what the
# guest had already written to its disk, so this tests guest-level durability
# (what hmd and SQLite actually flushed), not the disk hardware.
#
# Run inside the hostmortem-vm image (scripts/hard-reset-test.sh does that).
set -euo pipefail
cd "$(dirname "$0")/../.." || exit 1
W=lab/out/vm
mkdir -p "$W" "$W/root"
LOG="$W/serial.log"
: > "$LOG"

# --- 1. initramfs: busybox, static hmd/hm, init, two configs ----------------
R="$W/root"
mkdir -p "$R/bin" "$R/etc" "$R/proc" "$R/sys" "$R/dev" "$R/data"
cp /bin/busybox "$R/bin/busybox"
cp build/static/hmd build/static/hm "$R/bin/"
cp lab/vm/init "$R/init"
chmod +x "$R/init"
for phase in 1 2; do
  target=10.0.2.2
  [[ $phase == 2 ]] && target=127.0.0.1
  cat > "$R/etc/hm-phase$phase.conf" <<CONF
[store]
path = /data/events.db
commit_interval = 5s
[sampler]
interval = 1s
[probe]
interval = 2s
timeout = 1s
dns_names =
icmp_targets = $target
[detector]
incident_quiet = 20s
CONF
done
(cd "$R" && find . | cpio -o -H newc --quiet | gzip -1) > "$W/initramfs.cpio.gz"

# --- 2. a fresh ext4 disk ---------------------------------------------------
truncate -s 0 "$W/disk.img"
truncate -s 256M "$W/disk.img"
mkfs.ext4 -F -q "$W/disk.img"

# --- 3. boot; reset mid-incident; let boot 2 finish -------------------------
KERNEL=$(ls /boot/vmlinuz-* | head -1)
MON="$W/monitor.sock"
qemu-system-aarch64 -M virt -cpu max -smp 2 -m 768 -nographic -no-user-config -nodefaults \
  -serial stdio -kernel "$KERNEL" -initrd "$W/initramfs.cpio.gz" \
  -append "console=ttyAMA0 panic=-1 quiet" \
  -drive file="$W/disk.img",format=raw,if=virtio,cache=writeback \
  -monitor unix:"$MON",server,nowait >> "$LOG" 2>&1 &
QEMU=$!

wait_for() {  # pattern, timeout seconds
  local i
  for ((i = 0; i < $2; i++)); do
    grep -q "$1" "$LOG" && return 0
    sleep 1
  done
  return 1
}

wait_for "HM-PHASE 1" 240 || { echo "FAIL: boot 1 never started"; kill $QEMU; exit 1; }
wait_for "incident 1 opened" 180 || { echo "FAIL: no incident in boot 1"; kill $QEMU; exit 1; }
sleep 3   # let a few more symptoms accumulate past the incident-open commit
last_tick=$(grep -o 'HM-TICK [0-9.]*' "$LOG" | tail -1 | cut -d' ' -f2)
echo "system_reset" | socat - UNIX-CONNECT:"$MON" >/dev/null
echo "reset sent; last uptime tick seen before reset: ${last_tick}s" | tee "$W/reset.txt"

wait_for "HM-DONE" 400 || { echo "FAIL: boot 2 did not finish"; kill $QEMU; exit 1; }
wait $QEMU || true

# --- 4. extract and judge ---------------------------------------------------
sed -n '/HM-BEGIN-EXPORT/,/HM-END-EXPORT/p' "$LOG" | grep '^{' > "$W/events.jsonl"
sed -n '/HM-BEGIN-INCIDENTS/,/HM-END-INCIDENTS/p' "$LOG" | sed '1d;$d' > "$W/incidents.json"
sed -n '/HM-BEGIN-EXPLAIN/,/HM-END-EXPLAIN/p' "$LOG" > "$W/explain.txt"
sed -n '/HM-BEGIN-DOCTOR/,/HM-END-DOCTOR/p' "$LOG" > "$W/doctor.txt"
python3 - "$W" "$last_tick" <<'PY' | tee "$W/result.txt"
import json, sys
w, tick = sys.argv[1], float(sys.argv[2])
events = [json.loads(l) for l in open(f"{w}/events.jsonl")]
boots = []
for e in events:
    if e["boot_id"] not in boots:
        boots.append(e["boot_id"])
incidents = json.load(open(f"{w}/incidents.json"))
b1 = [e for e in events if e["boot_id"] == boots[0]]
last = max(e["mono_ns"] for e in b1) / 1e9
lost = tick - last
ok = True
print(f"boots recorded: {len(boots)} (distinct kernel boot_ids)")
print(f"boot 1 events that survived the reset: {len(b1)}")
print(f"newest surviving boot-1 event: uptime {last:.2f}s; last tick before reset: {tick:.2f}s")
print(f"evidence lost to the reset: {lost:.2f}s (+ up to 1 s tick resolution); FR2 bound 5 s")
inc = incidents[0] if incidents else None
spans = bool(inc and inc.get("spans_boots"))
print(f"incidents: {len(incidents)}; first: trigger={inc and inc['trigger']} status={inc and inc['status']} spans_boots={spans}")
doctor = open(f"{w}/doctor.txt").read()
qc = "quick_check ok" in doctor
print(f"store quick_check after reset: {'ok' if qc else 'FAILED'}")
starts = [e for e in events if e["kind"] == "agent.start"]
prev = [e for e in starts if "previous_boot" in e.get("attrs", {})]
print(f"agent.start events: {len(starts)}; boot 2 start names the previous boot: {bool(prev)}")
ok = len(boots) == 2 and qc and spans and lost <= 6.0 and bool(prev)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
