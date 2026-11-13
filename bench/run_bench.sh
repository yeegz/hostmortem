#!/usr/bin/env bash
# Repeats bench_pipeline and reports median and min/max of achieved events/s
# and drop counts. Usage: bench/run_bench.sh [reps] [producers] [eps_each] [secs]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${BIN:-build/release}"
REPS="${1:-10}" P="${2:-4}" R="${3:-5000}" S="${4:-5}"
out="bench/results-$(uname -m)-p${P}-r${R}.csv"
echo "# $(uname -srm), $(nproc) CPUs, $(date -u +%FT%TZ), reps=$REPS" > "$out"
for i in $(seq "$REPS"); do
  "$BIN/bench_pipeline" "$P" "$R" "$S" /tmp | tail -1 >> "$out"
done
python3 - "$out" <<'PY'
import statistics, sys
rows = [l.strip().split(",") for l in open(sys.argv[1]) if l[0].isdigit()]
eps = sorted(float(r[2]) for r in rows)
drops = [int(r[4]) for r in rows]
commits = [int(r[6]) for r in rows]
print(f"runs={len(rows)} offered={rows[0][1]} eps  achieved median={statistics.median(eps):.0f} min={eps[0]:.0f} max={eps[-1]:.0f} eps")
print(f"dropped: median={statistics.median(drops)} max={max(drops)}  commits/run median={statistics.median(commits)}")
PY
echo "raw: $out"
