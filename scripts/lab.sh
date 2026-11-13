#!/usr/bin/env bash
# Runs the fault-injection lab in a privileged throwaway container.
# Privileges are confined to that container's own network namespaces.
set -euo pipefail
cd "$(dirname "$0")/.."
HM_DOCKER_FLAGS="--privileged" exec scripts/dev.sh "cmake --build --preset debug >/dev/null && lab/run_lab.sh $*"
