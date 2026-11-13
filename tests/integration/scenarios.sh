#!/usr/bin/env bash
# Replays every lab fixture through `hm ingest` + `hm explain --json` and
# checks the expected diagnosis declared in the fixture's header:
#   # expect: leading=<playbook>|unknown  [supported=a,b] [ruled_out=c,d]
set -euo pipefail
HM="$1"
FIXTURES="$2"
work="$(mktemp -d)"
trap 'rm -rf -- "${work:?}"' EXIT
fail=0
for f in "$FIXTURES"/*.jsonl; do
  name="$(basename "$f" .jsonl)"
  db="$work/$name.db"
  expect="$(grep -m1 '^# expect:' "$f" | sed 's/^# expect: *//')"
  "$HM" --db "$db" ingest "$f" >/dev/null
  id="$("$HM" --db "$db" incidents --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[0]["id"] if d else "")')"
  if [[ -z "$id" ]]; then echo "FAIL $name: no incident opened"; fail=1; continue; fi
  "$HM" --db "$db" explain "$id" --json > "$work/$name.json"
  if python3 "$(dirname "$0")/check_diagnosis.py" "$work/$name.json" "$expect synthetic=1"
  then echo "ok   $name ($expect)"; else echo "FAIL $name"; fail=1; fi
done
exit $fail
