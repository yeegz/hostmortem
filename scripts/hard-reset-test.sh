#!/usr/bin/env bash
# Builds static hmd/hm, then boots them in a nested QEMU VM and hard-resets it
# mid-incident (lab/vm/run_hard_reset.sh). Results land in lab/out/vm/.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
scripts/dev.sh 'cmake --preset static >/dev/null && cmake --build --preset static >/dev/null'
if ! docker image inspect hostmortem-vm:0 >/dev/null 2>&1; then
  docker build -t hostmortem-vm:0 -f lab/vm/vm.Dockerfile lab/vm
fi
exec docker run --rm -v "$PWD":/src -w /src hostmortem-vm:0 bash lab/vm/run_hard_reset.sh
