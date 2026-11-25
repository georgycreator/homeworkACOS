#!/usr/bin/env bash
# run_local.sh — запуск всей системы на одной машине: сервер, 2 государства,
# 2 огневых подразделения и один наблюдатель (его вывод — на экран).
# Журналы каждого участника сохраняются в каталоге logs/.
#
# Использование: ./scripts/run_local.sh [конфиг.conf] [порт]
#   по умолчанию: scripts/cases/case1.conf, порт 5000
# Остановка: Ctrl+C (сервер штатно завершит всех клиентов).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/build"
CONF=${1:-$ROOT/scripts/cases/case1.conf}
PORT=${2:-5000}
LOGS="$ROOT/logs"
mkdir -p "$LOGS"

"$BIN/idz4_server" -p "$PORT" --log "$LOGS/server.log" --quiet "$CONF" &
SP=$!
trap 'kill -INT $SP 2>/dev/null; wait $SP 2>/dev/null' INT TERM

for r in A B; do
    "$BIN/idz4_side"      -s 127.0.0.1 -p "$PORT" -r $r --quiet --log "$LOGS/side$r.log" &
    "$BIN/idz4_artillery" -s 127.0.0.1 -p "$PORT" -r $r --quiet --log "$LOGS/gun$r.log" &
done
# Наблюдатель — на экран (и в свой журнал).
"$BIN/idz4_observer" -s 127.0.0.1 -p "$PORT" --log "$LOGS/observer.log"
wait
echo "Журналы участников: $LOGS"
