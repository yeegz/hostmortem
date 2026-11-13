#!/usr/bin/env bash
# Replays every lab fixture through `hm ingest` + `hm explain --json` and
# checks the expected diagnosis declared in the fixture's header:
#   # expect: leading=<playbook>|unknown  [supported=a,b] [ruled_out=c,d]
set -euo pipefail
HM="$1"
FIXTURES="$2"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail=0
for f in "$FIXTURES"/*.jsonl; do
  name="$(basename "$f" .jsonl)"
  db="$work/$name.db"
  expect="$(grep -m1 '^# expect:' "$f" | sed 's/^# expect: *//')"
  "$HM" --db "$db" ingest "$f" >/dev/null
  id="$("$HM" --db "$db" incidents --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[0]["id"] if d else "")')"
  if [[ -z "$id" ]]; then echo "FAIL $name: no incident opened"; fail=1; continue; fi
  "$HM" --db "$db" explain "$id" --json > "$work/$name.json"
  if python3 - "$work/$name.json" "$expect" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
want = dict(kv.split("=", 1) for kv in sys.argv[2].split())
verdicts = {h["playbook"]: h["verdict"] for h in d["hypotheses"]}
errors = []
leading = d["conclusion"].get("leading", "unknown") if d["conclusion"]["verdict"] == "supported" else "unknown"
if "leading" in want and want["leading"] != leading:
    errors.append(f"leading={leading}, want {want['leading']}")
for key, verdict in (("supported", "supported"), ("ruled_out", "ruled_out"), ("possible", "possible")):
    for pb in filter(None, want.get(key, "").split(",")):
        if verdicts.get(pb) != verdict:
            errors.append(f"{pb}={verdicts.get(pb)}, want {verdict}")
for h in d["hypotheses"]:
    if h["verdict"] != "ruled_out" and not any(e["role"] == "for" and e["event_id"] for e in h["evidence"]):
        errors.append(f"{h['playbook']} cites no event")
if not d.get("synthetic"):
    errors.append("synthetic flag missing")
if errors:
    print("; ".join(errors)); sys.exit(1)
PY
  then echo "ok   $name ($expect)"; else echo "FAIL $name"; fail=1; fi
done
exit $fail
