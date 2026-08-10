#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-8080}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_SERVER="${SCRIPT_DIR}/gitboard-server"

if [[ -x "${DIST_SERVER}" ]]; then
  SERVER="${DIST_SERVER}"
else
  cd "${REPO_ROOT}"
  echo "Building Barnaby server..."
  bazel build //src:gitboard-server
  SERVER="${REPO_ROOT}/bazel-bin/src/gitboard-server"
fi

PIDS="$(lsof -tiTCP:"${PORT}" -sTCP:LISTEN || true)"
if [[ -n "${PIDS}" ]]; then
  echo "Stopping existing Barnaby instance on port ${PORT}: ${PIDS}"
  kill ${PIDS}
  for _ in {1..30}; do
    if ! lsof -tiTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done
  REMAINING="$(lsof -tiTCP:"${PORT}" -sTCP:LISTEN || true)"
  if [[ -n "${REMAINING}" ]]; then
    echo "Existing instance did not stop cleanly; forcing stop: ${REMAINING}"
    kill -9 ${REMAINING}
  fi
else
  echo "No Barnaby instance is listening on port ${PORT}."
fi

echo "Starting Barnaby at http://127.0.0.1:${PORT}/"
exec "${SERVER}" --port "${PORT}"
