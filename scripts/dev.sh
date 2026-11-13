#!/usr/bin/env bash
# Run a command inside the Linux dev container with the repo mounted at /src.
# Usage: scripts/dev.sh <command...>     e.g. scripts/dev.sh cmake --build --preset debug
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE="${HM_DEV_IMAGE:-hostmortem-dev:0}"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" -f docker/dev.Dockerfile docker
fi
# shellcheck disable=SC2086 # HM_DOCKER_FLAGS is a deliberately word-split flag list
exec docker run --rm ${HM_DOCKER_FLAGS:-} -v "$PWD":/src -w /src "$IMAGE" bash -c "$*"
