#!/bin/bash
# run_tests.sh — автотесты IDZ2.
#
# Каждый тест проверяет не только код возврата, но и содержимое вывода:
#   * сценарные ожидания (сколько выстрелов, попаданий, залпов, какая причина остановки);
#   * инварианты ТЗ по журналу событий (check_invariants) для КАЖДОГО прогона:
#       - после «ПРЕКРАЩАЕТ ОГОНЬ» сторона больше не стреляет;
#       - остаток снарядов, число целей и стоимость не отрицательны;
#       - выстрелов в журнале ровно столько, сколько в итоговой статистике,
#         а попадания + промахи = выстрелы.
# Тесты с tick_ms=0 детерминированы по числу выстрелов: координатор ждёт, пока оба
# огневых потока отработают такт, поэтому «пустых» прогонов быть не может.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="${BIN:-$PROJECT_DIR/build/IDZ2}"
CASES_DIR="$SCRIPT_DIR/cases"
OUT_DIR="$PROJECT_DIR/tests"
mkdir -p "$OUT_DIR"
# Лог-файлы сценариев (log=duel_*.log) создаются в текущем каталоге — пусть это tests/.
cd "$OUT_DIR" || exit 1

if [ ! -x "$BIN" ]; then
    echo "Бинарь не найден: $BIN"
    echo "Собери: cd $PROJECT_DIR && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

TESTS_RUN=0
TESTS_PASS=0
TESTS_FAIL=0
OK=1   # флаг текущего теста

# ---------- вспомогательные функции ----------

pass () { echo "    OK: $1"; }
fail () { echo "    FAIL: $1"; OK=0; }

check_contains () {
    if grep -q -- "$2" "$1"; then pass "$3"; else fail "$3 (не найдено '$2')"; fi
}
check_absent () {
    if grep -q -- "$2" "$1"; then fail "$3 (найдено '$2')"; else pass "$3"; fi
}
count_of () { grep -c -- "$2" "$1" || true; }
check_count_eq () {   # файл, шаблон, ожидание, подпись
    local n; n=$(count_of "$1" "$2")
    if [ "$n" -eq "$3" ]; then pass "$4: $n"; else fail "$4: ожидалось $3, найдено $n"; fi
}
check_count_ge () {
    local n; n=$(count_of "$1" "$2")
    if [ "$n" -ge "$3" ]; then pass "$4: $n"; else fail "$4: ожидалось не менее $3, найдено $n"; fi
}

# Инварианты ТЗ по журналу событий (до блока итогов).
check_invariants () {
    local file="$1" report
    report=$(awk '
        /^========== ИТОГИ/ { in_stats = 1 }
        !in_stats && /ПРЕКРАЩАЕТ ОГОНЬ/ {
            s = $0; sub(/^\[/, "", s); sub(/\].*$/, "", s); stopped[s] = 1
        }
        !in_stats && /АРТИЛЛЕРИЯ .* \| pthread=/ && (/Выстрел по/ || /ТЯЖЁЛЫЙ ЗАЛП/) {
            s = $0; sub(/^.*АРТИЛЛЕРИЯ /, "", s); sub(/ \| pthread=.*$/, "", s)
            if (stopped[s]) { print "сторона " s " стреляет после прекращения огня: " $0; bad = 1 }
            log_shots++
        }
        /Осталось снарядов: -|осталось целей: -|\(стоимость -|потери противника: -/ {
            print "отрицательная величина: " $0; bad = 1
        }
        in_stats && /Выстрелов всего:/ { stat_shots += $3; cur = $3 }
        in_stats && /Попаданий:/ {
            h = $2; sub(/,/, "", h); m = $4
            if (h + m != cur) { print "попадания+промахи != выстрелы: " $0; bad = 1 }
        }
        END {
            if (in_stats && log_shots != stat_shots) {
                print "выстрелов в журнале " log_shots " != в статистике " stat_shots; bad = 1
            }
            exit bad
        }' "$file")
    if [ $? -eq 0 ]; then pass "инварианты ТЗ соблюдены"; else fail "нарушены инварианты: $report"; fi
}

finish () {
    if [ "$OK" = "1" ]; then
        TESTS_PASS=$((TESTS_PASS + 1)); echo "  РЕЗУЛЬТАТ: PASS"
    else
        TESTS_FAIL=$((TESTS_FAIL + 1)); echo "  РЕЗУЛЬТАТ: FAIL"
    fi
    echo
}

# Сценарные проверки для конкретного конфигурационного файла.
assert_case () {
    local name="$1" f="$2"
    case "$name" in
        case_case_tiny)
            # 1x1, по одному снаряду: первый выстрел уничтожает единственную цель,
            # второй стороне стрелять уже незачем.
            check_count_eq "$f" 'Выстрел по' 1 "ровно один выстрел"
            check_count_eq "$f" 'ПОПАДАНИЕ' 1 "ровно одно попадание"
            check_contains "$f" 'Всего тактов: 1$' "один такт"
            check_contains "$f" 'Причина: обе стороны прекратили огонь' "причина остановки"
            ;;
        case_case_budget)
            # Цена 3, бюджет 5: возможен один выстрел, второй (6 > 5) нет.
            check_count_eq "$f" 'Выстрел по' 1 "ровно один выстрел"
            check_contains "$f" 'бюджет исчерпан (израсходовано 3 из 5)' "причина: бюджет"
            ;;
        case_case_area_only)
            check_count_ge "$f" 'ТЯЖЁЛЫЙ ЗАЛП' 1 "тяжёлые залпы выполнены"
            check_count_eq "$f" 'Выстрел по' 0 "обычных выстрелов нет"
            ;;
        case_case_replenish)
            # Нулевой запас пополняется на 1 снаряд за такт: 5 тактов x 2 стороны.
            check_count_eq "$f" 'Выстрел по' 10 "10 выстрелов на пополненном запасе"
            check_count_eq "$f" 'Пополнение: +1 снарядов, всего' 10 "10 пополнений"
            check_contains "$f" 'Причина: лимит тактов' "причина остановки"
            ;;
        case_case_destroyed_cell)
            # Две цели (5 и 7): стоимость потерь ровно 12, не больше; каждая цель
            # уничтожена один раз, а повторные выстрелы по разрушенной клетке не дают результата.
            check_count_ge "$f" 'уже разрушена' 1 "повторное попадание в разрушенную клетку"
            check_count_eq "$f" 'ПОПАДАНИЕ' 2 "каждая цель уничтожена один раз"
            check_contains "$f" 'Потери: A — 0, B — 12' "потери B = 12"
            ;;
        case_case_overflow)
            # 30 000 снарядов по 100 000: сумма 3 000 000 000 > INT_MAX, int64 без переполнения.
            check_contains "$f" 'Всего тактов: 30000$' "30000 тактов"
            if awk '/Всего израсходовано:/ { if (($3+0) > 2147483647) found=1 } END { exit !found }' "$f"; then
                pass "расходы превысили INT_MAX без переполнения"
            else
                fail "расходы не превысили INT_MAX"
            fi
            ;;
        case_case_types)
            check_contains "$f" '(0,0) штаб, стоимость 5' "тип цели в списке целей"
            check_contains "$f" '(1,1) цель, стоимость 7' "тип по умолчанию"
            check_contains "$f" 'Уничтожена цель «штаб»' "тип цели в событии попадания"
            check_contains "$f" 'Уничтожена цель «радар»' "тип цели в событии попадания (B)"
            ;;
        case_case_area_zero)
            # Регрессия: радиус 0 и цена 0 не должны давать ложное «бюджет исчерпан».
            check_count_ge "$f" 'ТЯЖЁЛЫЙ ЗАЛП' 1 "тяжёлые залпы с радиусом 0 выполняются"
            check_absent "$f" 'бюджет исчерпан' "нет ложного сообщения о бюджете"
            check_absent "$f" 'Возобновляет' "завершение окончательное"
            ;;
        case_t_tick0|case_t_tick500)
            check_count_ge "$f" 'Выстрел по' 1 "выстрелы выполнялись"
            ;;
        case_case1|case_case3_area|default)
            check_count_ge "$f" 'Выстрел по' 1 "выстрелы выполнялись"
            ;;
        case_case_area_delayed)
            # При tick_ms=10 и reaction_ms=100 поток обычно пропускает точный такт 3, 6, 9...;
            # залп должен выполниться при ближайшей возможности.
            check_count_ge "$f" 'ТЯЖЁЛЫЙ ЗАЛП' 2 "при медленной реакции залпы не пропадают"
            ;;
    esac
}

run_case () {
    local name="$1"; shift
    local outfile="$OUT_DIR/${name}.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name ---"
    "$@" > "$outfile" 2>&1
    local rc=$?
    echo "  Код возврата: $rc, вывод: tests/${name}.stdout.txt"
    [ "$rc" -eq 0 ] || fail "ожидался код 0, получен $rc"
    check_contains "$outfile" "ЗАВЕРШЕНИЕ" "программа завершилась"
    check_invariants "$outfile"
    assert_case "$name" "$outfile"
    finish
}

# ---------- специальные тесты ----------

run_thread_structure_case () {
    local name="thread_structure" outfile="$OUT_DIR/thread_structure.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (проверка настоящих потоков) ---"
    "$BIN" "$CASES_DIR/case1.conf" > "$outfile" 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || fail "неожиданный код возврата $rc"
    check_contains "$outfile" "роль=coordinator" "есть поток-координатор"
    check_count_eq "$outfile" 'роль=side,.*: запущен' 2 "потоки сторон"
    check_count_eq "$outfile" 'роль=artillery,.*: запущен' 2 "огневые потоки"
    local worker_ids all_ids
    worker_ids=$(grep -E '\[ПОТОК\] роль=(side|artillery),.*: запущен' "$outfile" \
        | sed -E 's/.*pthread=([0-9]+).*/\1/' | sort -u | wc -l)
    [ "$worker_ids" -eq 4 ] && pass "у четырёх рабочих потоков разные pthread ID" \
                            || fail "ожидалось 4 разных ID рабочих потоков, найдено $worker_ids"
    all_ids=$(grep -E '\[ПОТОК\] роль=(coordinator|side|artillery)' "$outfile" \
        | grep -E 'управление моделью|: запущен' \
        | sed -E 's/.*pthread=([0-9]+).*/\1/' | sort -u | wc -l)
    [ "$all_ids" -eq 5 ] && pass "координатор отделён от рабочих потоков (5 уникальных ID)" \
                         || fail "ожидалось 5 уникальных ID потоков, найдено $all_ids"
    check_contains "$outfile" '\[АРТИЛЛЕРИЯ .*pthread=' "каждый выстрел маркируется артиллерийским потоком"
    check_invariants "$outfile"
    finish
}

run_signal_case () {
    local name="$1" case_file="$2" sig="$3" wait_sec="$4"
    local outfile="$OUT_DIR/${name}.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (SIG$sig через ${wait_sec}s) ---"
    if [ -z "$case_file" ]; then "$BIN" > "$outfile" 2>&1 &
    else "$BIN" "$case_file" > "$outfile" 2>&1 &
    fi
    local pid=$!
    sleep "$wait_sec"
    kill -"$sig" "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null
    local rc=$?
    echo "  Код возврата: $rc, вывод: tests/${name}.stdout.txt"
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 130 ] && [ "$rc" -ne 143 ]; then fail "неожиданный код $rc"; fi
    check_contains "$outfile" "ЗАВЕРШЕНИЕ" "программа завершилась"
    check_contains "$outfile" "Прерывание (сигнал" "сигнал обработан"
    check_invariants "$outfile"
    finish
}

# Конфигурация с недопустимой стоимостью цели должна отклоняться.
run_bad_config_case () {
    local name="bad_cost" outfile="$OUT_DIR/bad_cost.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (отклонение стоимости > 10^9) ---"
    "$BIN" "$CASES_DIR/bad_cost.conf" > "$outfile" 2>&1
    local rc=$?
    echo "  Код возврата: $rc (ожидается 1)"
    [ "$rc" -eq 1 ] && pass "код возврата 1" || fail "ожидался код 1, получен $rc"
    check_contains "$outfile" "стоимость должна быть в диапазоне" "понятное сообщение об ошибке"
    check_contains "$outfile" "неверный формат target" "число за пределами int64_t отклонено безопасно"
    check_absent "$outfile" "ТАКТ 0" "моделирование не запускалось"
    finish
}

# Закрытый канал вывода (prog | head) не должен убивать программу и обрывать лог.
run_sigpipe_case () {
    local name="sigpipe" logf="$OUT_DIR/sigpipe_case1.log"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (вывод в закрытый канал) ---"
    sed "s|^log=.*|log=$logf|" "$CASES_DIR/case1.conf" > "$OUT_DIR/sigpipe.conf"
    rm -f "$logf"
    bash -c "'$BIN' '$OUT_DIR/sigpipe.conf' 2>/dev/null | head -n 1 > /dev/null; exit \${PIPESTATUS[0]}"
    local rc=$?
    echo "  Код возврата программы: $rc (141 = убита SIGPIPE)"
    [ "$rc" -eq 0 ] && pass "программа завершилась штатно" || fail "код $rc вместо 0"
    check_contains "$logf" "ЗАВЕРШЕНИЕ" "лог-файл дописан до конца"
    finish
}

# Интерактивный ввод: все параметры, включая расширенные.
run_interactive_case () {
    local name="interactive" outfile="$OUT_DIR/interactive.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (интерактивный режим, расширенные параметры) ---"
    # 3 такта, tick_ms=0, точность 100/100, реакции по умолчанию; поле A 4x4 с целью-штабом,
    # поле B 4x4 с целью-радаром; «д» — расширенные: seed=5, A: 3 снаряда, B: 2 снаряда.
    printf '3\n0\n100\n100\n\n\n\n\n4\n4\n1 1 10 штаб\n\n4\n4\n2 2 10 радар\n\nд\n5\n3\n\n\n\n\n\n\n\n\n\n2\n\n\n\n\n\n\n\n\n\n' \
        | "$BIN" -i > "$outfile" 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || fail "ожидался код 0, получен $rc"
    check_contains "$outfile" "ЗАВЕРШЕНИЕ" "программа завершилась"
    check_contains "$outfile" "seed: 5" "seed задан интерактивно"
    check_contains "$outfile" "Обычные снаряды: 3, цена выстрела" "боезапас A задан интерактивно"
    check_contains "$outfile" "Обычные снаряды: 2, цена выстрела" "боезапас B задан интерактивно"
    check_contains "$outfile" "(1,1) штаб, стоимость 10" "цель с типом введена интерактивно"
    check_invariants "$outfile"
    finish
}

# Конец ввода до завершения ввода параметров — ошибка запуска, а не «сигнал».
run_interactive_eof_case () {
    local name="interactive_eof" outfile="$OUT_DIR/interactive_eof.stdout.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (EOF во время ввода) ---"
    "$BIN" -i < /dev/null > "$outfile" 2>&1
    local rc=$?
    [ "$rc" -eq 1 ] && pass "код возврата 1" || fail "ожидался код 1, получен $rc"
    check_contains "$outfile" "достигнут конец входного потока" "причина указана верно (не «сигнал»)"
    finish
}

# Один и тот же seed даёт идентичный результат (в сценарии стреляет только A).
run_repro_case () {
    local name="reproducibility" a="$OUT_DIR/repro_1.txt" b="$OUT_DIR/repro_2.txt"
    TESTS_RUN=$((TESTS_RUN + 1)); OK=1
    echo "--- Тест: $name (одинаковый seed — одинаковый ход боя) ---"
    # Служебные сообщения о запуске/остановке потоков идут в порядке, который выбирает
    # планировщик, поэтому сравнивается только ход боя.
    "$BIN" "$CASES_DIR/case_destroyed_cell.conf" 2>&1 | grep -v '\[ПОТОК\]' | sed -E 's/pthread=[0-9]+/pthread=N/g' > "$a"
    "$BIN" "$CASES_DIR/case_destroyed_cell.conf" 2>&1 | grep -v '\[ПОТОК\]' | sed -E 's/pthread=[0-9]+/pthread=N/g' > "$b"
    if cmp -s "$a" "$b"; then pass "два прогона побайтно совпадают"; else fail "прогоны различаются"; fi
    finish
}

# ---------- запуск ----------

echo "===== Тесты IDZ2 ====="
echo "Бинарь: $BIN"
echo

run_case "default" "$BIN"
run_thread_structure_case

for case in "$CASES_DIR"/*.conf; do
    [ -f "$case" ] || continue
    base="$(basename "$case" .conf)"
    case "$base" in case2|bad_cost) continue ;; esac
    run_case "case_$base" "$BIN" "$case"
done

echo "--- Тест: case2 (timeout 3 + SIGINT) ---"
TESTS_RUN=$((TESTS_RUN + 1)); OK=1
timeout --signal=INT --kill-after=2 3 "$BIN" "$CASES_DIR/case2.conf" \
    > "$OUT_DIR/case_case2.stdout.txt" 2>&1
rc=$?
echo "  Код возврата: $rc (124 = timeout с SIGINT, ожидаемо)"
if [ "$rc" -ne 124 ] && [ "$rc" -ne 0 ]; then fail "ожидался 124 или 0, получен $rc"; fi
check_contains "$OUT_DIR/case_case2.stdout.txt" "ЗАВЕРШЕНИЕ" "программа завершилась"
check_contains "$OUT_DIR/case_case2.stdout.txt" "Прерывание (сигнал" "сигнал обработан"
check_invariants "$OUT_DIR/case_case2.stdout.txt"
finish

run_signal_case "sigint_default"   ""                     "INT"  "1"
run_signal_case "sigint_infinite"  "$CASES_DIR/case2.conf" "INT"  "1"
run_signal_case "sigterm_infinite" "$CASES_DIR/case2.conf" "TERM" "1"

run_bad_config_case
run_sigpipe_case
run_interactive_case
run_interactive_eof_case
run_repro_case

echo "===== Итого: $TESTS_PASS / $TESTS_RUN успешно ====="
[ "$TESTS_FAIL" -eq 0 ]
