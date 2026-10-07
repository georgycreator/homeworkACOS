#!/bin/bash
# run_tests.sh — автотесты IDZ1: проверка exit code и содержимого.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$PROJECT_DIR/build/IDZ1"
CASES_DIR="$SCRIPT_DIR/cases"
OUT_DIR="$PROJECT_DIR/tests"
mkdir -p "$OUT_DIR"

if [ ! -x "$BIN" ]; then
    echo "Бинарь не найден: $BIN"
    echo "Собери: cd $PROJECT_DIR && mkdir -p build && cd build && cmake .. -G Ninja && ninja"
    exit 1
fi

TESTS_RUN=0
TESTS_PASS=0
TESTS_FAIL=0

check_contains () {
    local file="$1" pattern="$2" label="$3"
    if grep -q "$pattern" "$file"; then
        echo "    OK: $label"; return 0
    fi
    echo "    FAIL: $label (не найдено '$pattern')"; return 1
}

run_case () {
    local name="$1"; shift
    local outfile="$OUT_DIR/${name}.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1))
    echo "--- Тест: $name ---"
    "$@" > "$outfile" 2>&1
    local rc=$?
    echo "  Код возврата: $rc, вывод: $outfile"
    local ok=1
    if [ "$rc" -ne 0 ]; then
        echo "    FAIL: ожидался код 0, получен $rc"; ok=0
    fi
    check_contains "$outfile" "ЗАВЕРШЕНИЕ" "программа завершилась" || ok=0
    if [ "$ok" = "1" ]; then
        TESTS_PASS=$((TESTS_PASS + 1)); echo "  РЕЗУЛЬТАТ: PASS"
    else
        TESTS_FAIL=$((TESTS_FAIL + 1)); echo "  РЕЗУЛЬТАТ: FAIL"
    fi
    echo
}

run_signal_case () {
    local name="$1" case_file="$2" sig="$3" wait_sec="$4"
    local outfile="$OUT_DIR/${name}.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1))
    echo "--- Тест: $name (SIG$sig через ${wait_sec}s) ---"
    if [ -z "$case_file" ]; then
        "$BIN" > "$outfile" 2>&1 &
    else
        "$BIN" "$case_file" > "$outfile" 2>&1 &
    fi
    local pid=$!
    sleep "$wait_sec"
    kill -"$sig" "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null
    local rc=$?
    echo "  Код возврата: $rc, вывод: $outfile"
    local ok=1
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 130 ] && [ "$rc" -ne 143 ]; then
        echo "    FAIL: неожиданный код $rc"; ok=0
    fi
    check_contains "$outfile" "ЗАВЕРШЕНИЕ" "программа завершилась" || ok=0
    check_contains "$outfile" "Прерывание (сигнал" "сигнал обработан" || ok=0
    if [ "$ok" = "1" ]; then
        TESTS_PASS=$((TESTS_PASS + 1)); echo "  РЕЗУЛЬТАТ: PASS"
    else
        TESTS_FAIL=$((TESTS_FAIL + 1)); echo "  РЕЗУЛЬТАТ: FAIL"
    fi
    echo
}

echo "===== Тесты IDZ1 ====="
echo "Бинарь: $BIN"
echo

run_case "default" "$BIN"

for case in "$CASES_DIR"/*.conf; do
    [ -f "$case" ] || continue
    base="$(basename "$case" .conf)"
    [ "$base" = "case2" ] && continue
    run_case "case_$base" "$BIN" "$case"
done

echo "--- Тест: case2 (timeout 15 + SIGINT) ---"
TESTS_RUN=$((TESTS_RUN + 1))
timeout --signal=INT --kill-after=2 15 "$BIN" "$CASES_DIR/case2.conf" \
    > "$OUT_DIR/case_case2.stdout.txt" 2>&1
rc=$?
echo "  Код возврата: $rc (124 = timeout с SIGINT, ожидаемо)"
ok=1
if [ "$rc" -ne 124 ] && [ "$rc" -ne 0 ]; then
    echo "    FAIL: ожидался 124 или 0, получен $rc"; ok=0
fi
check_contains "$OUT_DIR/case_case2.stdout.txt" "ЗАВЕРШЕНИЕ" "программа завершилась" || ok=0
check_contains "$OUT_DIR/case_case2.stdout.txt" "Прерывание (сигнал" "сигнал обработан" || ok=0
if [ "$ok" = "1" ]; then
    TESTS_PASS=$((TESTS_PASS + 1)); echo "  РЕЗУЛЬТАТ: PASS"
else
    TESTS_FAIL=$((TESTS_FAIL + 1)); echo "  РЕЗУЛЬТАТ: FAIL"
fi
echo

run_signal_case "sigint_default"   ""                    "INT"  "1"
run_signal_case "sigint_infinite"  "$CASES_DIR/case2.conf" "INT"  "3"
run_signal_case "sigterm_infinite" "$CASES_DIR/case2.conf" "TERM" "3"

echo "===== Итого: $TESTS_PASS / $TESTS_RUN успешно ====="
[ "$TESTS_FAIL" -eq 0 ]
