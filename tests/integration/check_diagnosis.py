#!/usr/bin/env python3
"""Checks an `hm explain --json` result against an expectation string:
   leading=<playbook>|unknown  supported=a,b  ruled_out=c  possible=d  synthetic=0|1
Exit 0 on match, 1 with reasons otherwise."""
import json
import sys

d = json.load(open(sys.argv[1]))
want = dict(kv.split("=", 1) for kv in sys.argv[2].split())
verdicts = {h["playbook"]: h["verdict"] for h in d["hypotheses"]}
errors = []
c = d["conclusion"]
leading = c.get("leading", "unknown") if c["verdict"] == "supported" else "unknown"
if "leading" in want and want["leading"] != leading:
    errors.append(f"leading={leading}, want {want['leading']} ({c['reason']})")
for key in ("supported", "ruled_out", "possible"):
    for pb in filter(None, want.get(key, "").split(",")):
        if verdicts.get(pb) != key:
            errors.append(f"{pb}={verdicts.get(pb)}, want {key}")
for h in d["hypotheses"]:
    if h["verdict"] != "ruled_out" and not any(e["role"] == "for" and e["event_id"] for e in h["evidence"]):
        errors.append(f"{h['playbook']} cites no event")
if "synthetic" in want and bool(int(want["synthetic"])) != bool(d.get("synthetic")):
    errors.append(f"synthetic={d.get('synthetic')}, want {want['synthetic']}")
if errors:
    print("; ".join(errors))
    sys.exit(1)
