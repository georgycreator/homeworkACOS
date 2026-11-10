#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
SESSION="${1:-demo_$(date +%s)}"
CONFIG="${2:-$ROOT/scripts/cases/case1.conf}"

for binary in IDZ3_init IDZ3_coordinator IDZ3_side IDZ3_artillery; do
  [[ -x "$BUILD/$binary" ]] || { echo "Не найден $BUILD/$binary — сначала выполните сборку" >&2; exit 1; }
done

"$BUILD/IDZ3_init" "$SESSION" "$CONFIG"
"$BUILD/IDZ3_coordinator" "$SESSION" &
COORDINATOR_PID=$!
WORKER_PIDS=()
cleanup() {
  # Координатор штатно обрабатывает SIGINT/SIGTERM и будит рабочие процессы.
  if kill -0 "$COORDINATOR_PID" 2>/dev/null; then kill -INT "$COORDINATOR_PID" 2>/dev/null || true; fi
  for pid in "${WORKER_PIDS[@]:-}"; do kill -0 "$pid" 2>/dev/null && wait "$pid" || true; done
  wait "$COORDINATOR_PID" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

# Отдельные исполняемые файлы, каждый со своим PID и собственным подключением к IPC.
"$BUILD/IDZ3_side" "$SESSION" A & WORKER_PIDS+=("$!")
"$BUILD/IDZ3_side" "$SESSION" B & WORKER_PIDS+=("$!")
"$BUILD/IDZ3_artillery" "$SESSION" A & WORKER_PIDS+=("$!")
"$BUILD/IDZ3_artillery" "$SESSION" B & WORKER_PIDS+=("$!")

set +e
wait "$COORDINATOR_PID"
COORDINATOR_STATUS=$?
set -e
for pid in "${WORKER_PIDS[@]}"; do wait "$pid" || true; done
trap - INT TERM EXIT
exit "$COORDINATOR_STATUS"
