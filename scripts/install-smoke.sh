#!/bin/bash
# Fresh-machine smoke test of the documented install path, on a VM with real
# systemd (GitHub's ubuntu-* runners in CI, or a throwaway Lima/cloud VM).
# Run as a normal user with passwordless sudo, from the repository root:
#
#   scripts/install-smoke.sh DEB_DIR [--upgrade-from 0.1.0] [--footprint SECONDS]
#
# DEB_DIR holds hostmortem_*_<arch>.deb and SHA256SUMS. Steps, each checked:
#   (optional) install an older release from GitHub and let it record;
#   install.sh from DEB_DIR -> hmd active under systemd;
#   upgrade keeps the old evidence and restarts hmd on the new binary;
#   hm drill as this user via the hostmortem group (no sudo) -> incident;
#   hm explain and hm report (plain and --redact) render it;
#   (optional) footprint: RSS and CPU of the running service;
#   apt purge removes the agent, store and user.
# Prints "SMOKE PASS" at the end; any failed check exits non-zero.
set -euo pipefail
deb_dir="$(cd "$1" && pwd)"
shift
upgrade_from=""
footprint=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --upgrade-from) upgrade_from="$2"; shift 2 ;;
    --footprint) footprint="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
repo="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mktemp -d)"
step() { printf '\n== %s\n' "$*"; }
check() { if "$@"; then echo "   ok: $*"; else echo "   FAIL: $*"; exit 1; fi; }
db=/var/lib/hostmortem/events.db

step "machine"
# shellcheck source=/dev/null
. /etc/os-release
echo "   $PRETTY_NAME, $(uname -m), kernel $(uname -r), glibc $(getconf GNU_LIBC_VERSION | awk '{print $2}'), systemd $(systemctl --version | awk 'NR==1{print $2}')"
check test -d /run/systemd/system

events_before=0
if [[ -n "$upgrade_from" ]]; then
  step "install v$upgrade_from from GitHub Releases (the documented 0.1.0 path)"
  arch="$(dpkg --print-architecture)"
  (cd "$out" && curl -fsSLO "https://github.com/yeegz/hostmortem/releases/download/v$upgrade_from/hostmortem_${upgrade_from}_${arch}.deb" \
     && curl -fsSL -o OLDSUMS "https://github.com/yeegz/hostmortem/releases/download/v$upgrade_from/SHA256SUMS" \
     && sha256sum --check --ignore-missing OLDSUMS)
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$out/hostmortem_${upgrade_from}_${arch}.deb" >/dev/null
  sudo systemctl enable --now hmd
  sleep 15
  events_before="$(sudo hm status | sed -n 's/.*events: \([0-9]*\).*/\1/p')"
  echo "   v$upgrade_from recorded $events_before events"
  check test "$events_before" -gt 0
fi
pid_before="$(systemctl show -p MainPID --value hmd 2>/dev/null || echo 0)"

step "install.sh"
sudo HM_DEB_DIR="$deb_dir" sh "$repo/install.sh" | tee "$out/install.log"
check grep -q "checksum OK" "$out/install.log"
check systemctl is-active --quiet hmd
new_version="$(dpkg-query -W -f='${Version}' hostmortem)"
check test "$(hmd --version)" = "hmd $new_version"
# Under the hardened unit hmd can still read the clock-sync state (adjtimex).
# The first commit lands within commit_interval (5 s).
start_line=""
for _ in $(seq 20); do
  start_line="$(sudo hm timeline --all --kind agent.start --json | tail -n1)"
  [[ "$start_line" == *"\"version\":\"$new_version\""* ]] && break
  sleep 1
done
check test -n "$start_line"
check test "${start_line/\"wall_quality\":\"unknown\"/}" = "$start_line"
check bash -c "id -nG $USER | tr ' ' '\\n' | grep -qx hostmortem"
if [[ -n "$upgrade_from" ]]; then
  check grep -q "upgraded $upgrade_from -> $new_version" "$out/install.log"
  pid_after="$(systemctl show -p MainPID --value hmd)"
  echo "   hmd pid $pid_before -> $pid_after"
  check test "$pid_before" != "$pid_after"
  sleep 3
  events_after="$(sudo hm status | sed -n 's/.*events: \([0-9]*\).*/\1/p')"
  echo "   events $events_before -> $events_after (store kept across the upgrade)"
  check test "$events_after" -ge "$events_before"
  check bash -c "sudo hm timeline --all --kind agent.start --json | grep -q '\"version\":\"$new_version\"'"
fi
sleep 3

step "first incident: hm drill as $USER through the hostmortem group, no sudo"
sg hostmortem -c "hm drill" | tee "$out/drill.log"
check grep -q "drill: recorded" "$out/drill.log"
id="$(sed -n 's/.*hm explain \([0-9]*\).*/\1/p' "$out/drill.log")"
sg hostmortem -c "hm explain $id" > "$out/explain.txt"
head -12 "$out/explain.txt" | sed 's/^/   | /'
check grep -q "process.exit \[hm-drill\]" "$out/explain.txt"
sg hostmortem -c "hm incidents"

step "hm report"
(cd "$out" && sg hostmortem -c "hm report -o report.html" && sg hostmortem -c "hm report --redact -o redacted.html")
check grep -q "hm-drill" "$out/report.html"
check grep -q "default-src 'none'" "$out/report.html"
check bash -c "! grep -qi '<script' '$out/report.html'"
gw="$(ip -4 route show default | awk '{print $3; exit}')"
if [[ -n "$gw" ]] && grep -q "$gw" "$out/report.html"; then
  check bash -c "! grep -q '$gw' '$out/redacted.html'"
fi
stat -c '   %s bytes  %n' "$out"/*.html
cp "$out/report.html" "${SMOKE_REPORT_OUT:-/tmp}/hostmortem-smoke-report.html" 2>/dev/null || true

step "hm doctor"
sg hostmortem -c "hm doctor" | sed 's/^/   | /'

if [[ "$footprint" -gt 0 ]]; then
  step "footprint over ${footprint} s (service as installed, default config)"
  pid="$(systemctl show -p MainPID --value hmd)"
  hz="$(getconf CLK_TCK)"
  t0="$(awk '{print $14+$15}' "/proc/$pid/stat")"
  sleep "$footprint"
  t1="$(awk '{print $14+$15}' "/proc/$pid/stat")"
  rss_kb="$(awk '/VmRSS/{print $2}' "/proc/$pid/status")"
  cpu="$(awk -v a="$t0" -v b="$t1" -v hz="$hz" -v s="$footprint" 'BEGIN{printf "%.2f", 100*(b-a)/hz/s}')"
  echo "   FOOTPRINT rss_mb=$(awk -v k="$rss_kb" 'BEGIN{printf "%.1f", k/1024}') cpu_pct_one_core=$cpu memory_current_mb=$(( $(systemctl show -p MemoryCurrent --value hmd) / 1048576 )) store_kb=$(sudo sh -c "du -ck $db*" | tail -n1 | cut -f1)"
fi

step "purge"
sudo DEBIAN_FRONTEND=noninteractive apt-get purge -y -qq hostmortem >/dev/null
check bash -c "! systemctl is-active --quiet hmd"
check test ! -e /var/lib/hostmortem
check bash -c "! getent passwd hostmortem"

printf '\nSMOKE PASS (%s, %s)\n' "$PRETTY_NAME" "$(dpkg --print-architecture)"
