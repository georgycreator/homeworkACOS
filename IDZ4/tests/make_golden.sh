#!/usr/bin/env bash
# make_golden.sh ПУТЬ_К_БИНАРНИКУ_IDZ3 [число_прогонов]
# Строит эталонные сводки tests/golden/<кейс>.sums по исходной реализации IDZ3.
# Каждый кейс запускается несколько раз: если IDZ3 даёт разные итоги (его процессы
# гоняются за порядок выстрелов в такте), в файл попадают ВСЕ наблюдавшиеся варианты,
# разделённые строкой «---». IDZ4 обязан давать один из них.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OLD=${1:?укажите путь к IDZ3_named}
RUNS=${2:-8}
mkdir -p "$ROOT/tests/golden"
for conf in "$ROOT"/scripts/cases/*.conf; do
    n=$(basename "$conf" .conf)
    case $n in bad_cost|case2|net_long|net_lockstep_long) continue;; esac
    d=$(mktemp -d); cd "$d"
    sed -E 's/^tick_ms=.*/tick_ms=0/; /^log=/d' "$conf" > c.conf
    grep -q '^tick_ms' c.conf || echo "tick_ms=0" >> c.conf
    for i in $(seq 1 "$RUNS"); do
        timeout 30 "$OLD" c.conf > out.$i 2>&1 || { echo "ПРОПУСК $n (IDZ3 не завершился)"; continue 2; }
        "$ROOT/tests/summary.sh" out.$i > sum.$i
    done
    # уникальные варианты и сколько раз каждый встретился
    sort sum.* 2>/dev/null >/dev/null
    declare -A seen=(); : > "$ROOT/tests/golden/$n.sums"
    for i in $(seq 1 "$RUNS"); do
        k=$(md5sum < sum.$i | cut -d' ' -f1)
        if [ -z "${seen[$k]:-}" ]; then seen[$k]=1; cat sum.$i >> "$ROOT/tests/golden/$n.sums"; echo "---" >> "$ROOT/tests/golden/$n.sums"; fi
    done
    echo "$n: вариантов итога у IDZ3: ${#seen[@]}"
    unset seen; cd /; rm -rf "$d"
done
