#!/usr/bin/env bash
# run_tests.sh — автоматические проверки IDZ4 (сервер + клиенты по TCP на localhost).
#
# Использование:  ./scripts/run_tests.sh [имя_теста ...]     (без аргументов — все)
# Предварительно: make   (или cmake --build build)
# Каждый тест поднимает свой сервер на свободном порту (--port 0), поэтому
# тесты не мешают друг другу и работающим программам. Рабочие файлы — в mktemp-каталоге.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="${IDZ4_BIN:-$ROOT/build}"     # IDZ4_BIN=build-asan — прогнать тесты на другой сборке
CASES="$ROOT/scripts/cases"
WORK=$(mktemp -d /tmp/idz4_tests.XXXXXX)
PASS=0; FAIL=0; PIDS=()

for exe in idz4_server idz4_side idz4_artillery idz4_observer; do
    [ -x "$BIN/$exe" ] || { echo "Не найден $BIN/$exe — сначала соберите проект (make)"; exit 2; }
done

cleanup() { [ ${#PIDS[@]} -gt 0 ] && kill -9 "${PIDS[@]}" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

ok()  { echo "    [OK]   $1"; PASS=$((PASS+1)); }
bad() { echo "    [FAIL] $1"; FAIL=$((FAIL+1)); }
check() { local d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
has()  { grep -q -- "$2" "$1"; }                 # файл содержит текст
hasnt(){ ! grep -q -- "$2" "$1"; }

# ---- вспомогательные функции ------------------------------------------------

# start_server DIR CONF [ключи сервера...]  -> SP, PORT
start_server() {
    local dir=$1 conf=$2; shift 2
    mkdir -p "$dir"; rm -f "$dir/port"
    "$BIN/idz4_server" --port 0 --port-file "$dir/port" --log "$dir/srv.log" --quiet "$@" "$conf" >"$dir/srv.out" 2>&1 &
    SP=$!; PIDS+=("$SP")
    for _ in $(seq 1 50); do [ -s "$dir/port" ] && break; sleep 0.1; done
    PORT=$(cat "$dir/port" 2>/dev/null)
}
# start_client DIR ИМЯ ПРОГРАММА [ключи...]  -> CP
start_client() {
    local dir=$1 name=$2 prog=$3; shift 3
    "$BIN/$prog" -s 127.0.0.1 -p "$PORT" --quiet --log "$dir/$name.log" "$@" >"$dir/$name.out" 2>&1 &
    CP=$!; PIDS+=("$CP")
}
# start_all DIR -> запускает 2 государства, 2 подразделения (PID в SA SB GA GB)
start_all() {
    local d=$1
    start_client "$d" sideA idz4_side -r A;      SA=$CP
    start_client "$d" sideB idz4_side -r B;      SB=$CP
    start_client "$d" gunA  idz4_artillery -r A; GA=$CP
    start_client "$d" gunB  idz4_artillery -r B; GB=$CP
}
# wait_exit PID СЕКУНДЫ -> код завершения или 124 при таймауте
wait_exit() {
    local pid=$1 t=$2 i=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.1; i=$((i+1)); [ $i -ge $((t*10)) ] && return 124
    done
    wait "$pid" 2>/dev/null; return $?
}
alive() { kill -0 "$1" 2>/dev/null; }
# kill9 PID — убить без предупреждения и тихо «собрать» процесс (без сообщения bash «Killed»)
kill9() { kill -9 "$1" 2>/dev/null; { wait "$1"; } 2>/dev/null; }
# число тактов из итогового блока журнала сервера
ticks_of() { grep -m1 "Всего тактов:" "$1" | grep -oE "[0-9]+"; }
# значение поля из блока итогов: field FILE "Выстрелов всего" N-е вхождение
nth_num() { grep "$2" "$1" | sed -n "${3}p" | grep -oE "[0-9]+" | head -1; }
# сервер завершил моделирование штатно и все клиенты вышли с кодом 0
all_exit_ok() {
    local rc=0 p
    wait_exit "$SP" 60; [ $? -eq 0 ] || { echo "server rc != 0"; rc=1; }
    for p in "$@"; do wait_exit "$p" 10; [ $? -eq 0 ] || { echo "client $p rc != 0"; rc=1; }; done
    return $rc
}

# ---- тесты ------------------------------------------------------------------

# Режим tick_ms=0: полная итоговая сводка (выстрелы, попадания, промахи, уничтоженные цели,
# остатки и расход боезапаса, потери, вердикт, число тактов) сравнивается с эталоном,
# полученным на исходной реализации IDZ3 (tests/golden/*.sums, см. tests/make_golden.sh).
# Если сам IDZ3 даёт разные итоги (гонка процессов), в эталоне записаны все наблюдавшиеся
# варианты, и IDZ4 должен совпасть с одним из них.
golden_match() {   # golden_match КАНДИДАТ ЭТАЛОН -> номер совпавшего варианта (или код 1)
    awk -v cand="$1" 'BEGIN { while ((getline l < cand) > 0) c = c l "\n" }
        /^---$/ { n++; if (b == c) { print n; found = 1; exit } b = ""; next }
        { b = b $0 "\n" } END { exit found ? 0 : 1 }' "$2"
}
t_lockstep_cases() {
    echo "== T1. Режим tick_ms=0: полная сводка совпадает с эталоном IDZ3"
    local g c
    for g in "$ROOT"/tests/golden/*.sums; do
        c=$(basename "$g" .sums)
        local d="$WORK/lock_$c"; mkdir -p "$d"
        sed -E 's/^tick_ms=.*/tick_ms=0/; /^log=/d' "$CASES/$c.conf" > "$d/c.conf"
        grep -q '^tick_ms' "$d/c.conf" || echo "tick_ms=0" >> "$d/c.conf"
        start_server "$d" "$d/c.conf"; start_all "$d"
        if all_exit_ok $SA $SB $GA $GB >/dev/null 2>&1; then
            "$ROOT/tests/summary.sh" "$d/srv.log" > "$d/new.sum"
            local variants; variants=$(grep -c '^---$' "$g")
            if v=$(golden_match "$d/new.sum" "$g"); then
                ok "$c: совпал вариант $v из $variants (тактов $(grep ТАКТОВ "$d/new.sum" | cut -d' ' -f2))"
            else bad "$c: сводка не совпала ни с одним эталонным вариантом ($variants)"; diff "$d/new.sum" <(sed '/^---$/q' "$g") | head -6; fi
        else bad "$c: участники завершились с ошибкой"; fi
    done
}

# Инварианты из ТЗ, проверяемые по итоговой статистике сервера.
t_invariants() {
    echo "== T2. Инварианты: выстрелы = попадания + промахи, нет отрицательных значений"
    local d="$WORK/inv"; mkdir -p "$d"
    sed -E 's/^tick_ms=.*/tick_ms=0/; /^log=/d' "$CASES/case1.conf" > "$d/c.conf"
    start_server "$d" "$d/c.conf"; start_all "$d"; all_exit_ok $SA $SB $GA $GB >/dev/null 2>&1
    local i
    for i in 1 2; do
        local shots hits miss
        shots=$(grep "Выстрелов всего" "$d/srv.log" | sed -n "${i}p" | grep -oE "[0-9]+" | head -1)
        hits=$(grep "Попаданий:" "$d/srv.log" | sed -n "${i}p" | grep -oE "[0-9]+" | sed -n 1p)
        miss=$(grep "Попаданий:" "$d/srv.log" | sed -n "${i}p" | grep -oE "[0-9]+" | sed -n 2p)
        [ "$shots" -eq $((hits+miss)) ] && ok "сторона $i: $shots = $hits + $miss" || bad "сторона $i: $shots != $hits + $miss"
    done
    check "нет отрицательных чисел в итогах" hasnt "$d/srv.log" ": -[0-9]"
    check "не более одного выстрела за такт (нет отклонений dup)" hasnt "$d/srv.log" "второй выстрел"
}

t_realtime_observer() {
    echo "== T3. Бой в реальном времени, наблюдатель видит полную картину"
    local d="$WORK/rt"; mkdir -p "$d"
    start_server "$d" "$CASES/case1.conf"
    start_client "$d" obs idz4_observer; local OB=$CP
    start_all "$d"
    all_exit_ok $SA $SB $GA $GB $OB && ok "все завершились с кодом 0" || bad "ненулевой код завершения"
    check "наблюдатель получил итоги" has "$d/obs.log" "ИТОГИ IDZ4"
    check "наблюдатель видит выстрелы обеих сторон" has "$d/obs.log" "АРТИЛЛЕРИЯ Южная"
    check "наблюдатель видит карты территорий" has "$d/obs.log" "Территория A"
    check "государство видит только свою территорию" hasnt "$d/sideA.log" "Территория B"
    check "подразделение не видит карт" hasnt "$d/gunA.log" "Территория"
}

t_observers_dynamic() {
    echo "== T4. Наблюдатели подключаются и отключаются во время боя"
    local d="$WORK/obsdyn"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf"; start_all "$d"
    start_client "$d" o1 idz4_observer; local O1=$CP
    sleep 1.5; kill9 "$O1"                                  # внезапное отключение
    start_client "$d" o2 idz4_observer; local O2=$CP          # поздний наблюдатель
    start_client "$d" o3 idz4_observer; local O3=$CP
    sleep 1.5; kill -TERM "$O3"                               # штатное отключение по сигналу
    all_exit_ok $SA $SB $GA $GB $O2 && ok "бой завершился штатно, все участники и o2 вышли с кодом 0" || bad "сбой"
    check "сервер сообщил о 3 подключениях наблюдателей" test "$(grep -c 'Подключён observer' "$d/srv.log")" -eq 3
    check "поздний наблюдатель получил заголовок журнала" has "$d/o2.log" "СТАРТ IDZ4"
    check "поздний наблюдатель получил историю уже прошедших событий" has "$d/o2.log" "ранее (последние"
    check "в истории есть выстрелы, сделанные до его подключения" has "$d/o2.log" "\[такт [0-9]\]"
    check "поздний наблюдатель получил текущее состояние" has "$d/o2.log" "Текущее состояние"
    check "бой дошёл до лимита тактов" test "$(ticks_of "$d/srv.log")" = 80
}

t_reconnect() {
    echo "== T5. Отключение и возврат участников (kill -9): сервер на паузе и продолжает"
    local d="$WORK/recon"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf" --grace 20; start_all "$d"
    start_client "$d" obs idz4_observer; local OB=$CP
    sleep 2; kill9 "$GA"; sleep 1.5
    check "сервер жив и на паузе" alive "$SP"
    check "пауза записана в журнале" has "$d/srv.log" "gun-A отключился"
    start_client "$d" gunA2 idz4_artillery -r A; local GA2=$CP
    sleep 1.5; kill9 "$SB"; sleep 1
    start_client "$d" sideB2 idz4_side -r B; local SB2=$CP
    all_exit_ok $SA $GB $GA2 $SB2 $OB && ok "бой завершён, оставшиеся и вернувшиеся вышли с кодом 0" || bad "сбой"
    check "возобновление после gun-A" has "$d/srv.log" "Участник gun-A вернулся"
    check "возобновление после side-B" has "$d/srv.log" "Участник side-B вернулся"
    check "все 80 тактов отыграны" test "$(ticks_of "$d/srv.log")" = 80
    check "вернувшееся подразделение получило своё состояние с сервера" has "$d/gunA2.log" "состояние генератора восстановлено"
    check "наблюдатель видел паузу" has "$d/obs.log" "приостановлено"
}

t_server_sigint() {
    echo "== T6. Остановка сервера (SIGINT): все клиенты завершаются корректно"
    local d="$WORK/sigint"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf"; start_all "$d"
    start_client "$d" obs idz4_observer; local OB=$CP
    sleep 2; kill -INT "$SP"
    local bad_rc=0 p
    for p in $SA $SB $GA $GB $OB; do wait_exit "$p" 10; [ $? -eq 0 ] || bad_rc=1; done
    [ $bad_rc -eq 0 ] && ok "все 5 клиентов вышли с кодом 0" || bad "клиент завис или вышел с ошибкой"
    wait_exit "$SP" 10; [ $? -eq 0 ] && ok "сервер вышел с кодом 0" || bad "сервер вышел с ошибкой"
    check "клиенты получили причину завершения" has "$d/gunA.log" "прерывание оператором"
}

t_server_kill9() {
    echo "== T7. Сервер убит без предупреждения (SIGKILL): клиенты не зависают"
    local d="$WORK/kill9"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf"; start_all "$d"
    sleep 1.5; kill9 "$SP"
    local p ok_all=1 rc
    for p in $SA $SB $GA $GB; do wait_exit "$p" 10; rc=$?; [ $rc -eq 2 ] || ok_all=0; done
    [ $ok_all -eq 1 ] && ok "клиенты вышли с кодом 2 («сервер недоступен»)" || bad "клиент завис или код не 2"
}

t_duplicate_role() {
    echo "== T8. Занятая роль не принимается, бой не нарушается"
    local d="$WORK/dup"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf"; start_all "$d"; sleep 1
    start_client "$d" dup idz4_artillery -r A; local DP=$CP
    wait_exit "$DP" 5; [ $? -eq 1 ] && ok "дубликат gun-A получил отказ (код 1)" || bad "дубликат не отклонён"
    check "сообщение «роль уже занята»" has "$d/dup.log" "уже занята"
    all_exit_ok $SA $SB $GA $GB && ok "основной бой завершился штатно" || bad "бой нарушен"
}

t_garbage() {
    echo "== T9. Мусор и некорректные сообщения не ломают сервер"
    local d="$WORK/garbage"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf"; start_all "$d"; sleep 0.8
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT; printf 'SHOT 1 R 0 0 0\n' >&3; sleep 0.2 ) 2>/dev/null          # до HELLO
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT; printf 'HELLO martian\n' >&3; sleep 0.2 ) 2>/dev/null           # неизвестная роль
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT; head -c 20000 /dev/urandom >&3; sleep 0.2 ) 2>/dev/null          # двоичный мусор
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT; head -c 10000 /dev/zero | tr '\0' 'A' >&3; sleep 0.2 ) 2>/dev/null  # строка без \n
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT; printf 'HELLO observer\nSHOT\nACK x\nBOGUS 1 2 3\n' >&3; sleep 0.3 ) 2>/dev/null
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT; sleep 0.2 ) 2>/dev/null                                           # молчащий клиент
    check "сервер жив после мусора" alive "$SP"
    all_exit_ok $SA $SB $GA $GB && ok "бой завершён штатно" || bad "бой нарушен"
    check "неизвестная роль отклонена" has "$d/srv.log" "нет HELLO\|Отключён"
    check "ticks = 80" test "$(ticks_of "$d/srv.log")" = 80
}

t_bad_shots() {
    echo "== T10. Сервер отклоняет некорректные выстрелы и ничего не списывает"
    local d="$WORK/shots"; mkdir -p "$d"
    sed -E 's/^tick_ms=.*/tick_ms=0/; /^log=/d' "$CASES/case1.conf" > "$d/c.conf"
    start_server "$d" "$d/c.conf"
    start_client "$d" sideA idz4_side -r A; local SA_=$CP
    start_client "$d" sideB idz4_side -r B; local SB_=$CP
    start_client "$d" gunB idz4_artillery -r B; local GB_=$CP
    sleep 0.3
    # «Ручное» огневое подразделение A: шлёт заведомо плохие запросы, затем один хороший.
    ( exec 3<>/dev/tcp/127.0.0.1/$PORT
      printf 'HELLO gun-A\n' >&3
      while IFS= read -r -t 5 line <&3; do
          case "$line" in
              GUNTICK\ 0\ *)
                  printf 'SHOT 0 H 1 1 0 1 1\nSHOT 0 X 0 0 0 1 1\nSHOT 0 R 99 99 0 1 1\nSHOT 0 R 0 0 500 1 1\nSHOT 0 R 1 1 0 1 1\nSHOT 0 R 1 1 0 1 1\nSHOT 7 R 1 1 0 1 1\nSHOT abc\n' >&3 ;;
              GUNTICK\ *) printf 'PASS %s\n' "$(echo "$line" | cut -d' ' -f2)" >&3 ;;
              END*) break ;;
          esac
      done ) > "$d/manual.out" 2>&1 &
    local MP=$!; PIDS+=("$MP")
    wait_exit "$SP" 30; [ $? -eq 0 ] && ok "сервер завершился штатно" || bad "сервер завершился с ошибкой"
    check "тяжёлый залп раньше срока отклонён сервером" has "$d/srv.log" "раньше срока"
    check "неверный вид снаряда отклонён" has "$d/srv.log" "неверные аргументы"
    check "координаты вне поля отклонены" has "$d/srv.log" "координаты вне территории"
    # дубликат: либо «второй выстрел», либо «опоздание» (если такт успел закрыться) — важно, что он не исполнен
    check "второй выстрел в такте отклонён" has "$d/srv.log" "второй выстрел за один такт\\|опоздание"
    check "выстрел «из будущего» отклонён" has "$d/srv.log" "опоздание\|вне фазы"
    check "на такте 0 принят ровно один выстрел A" test "$(grep -c '^\[АРТИЛЛЕРИЯ Северная\] \[такт 0\]' "$d/srv.log")" -eq 1
    kill -9 $SA_ $SB_ $GB_ $MP 2>/dev/null
}

t_absent_timeout() {
    echo "== T11. Участник не вернулся: сервер завершается по --grace с кодом 1"
    local d="$WORK/absent"; mkdir -p "$d"
    start_server "$d" "$CASES/net_long.conf" --grace 2; start_all "$d"
    sleep 1.5; kill9 "$GA"
    wait_exit "$SP" 15; local rc=$?
    [ $rc -eq 1 ] && ok "сервер вышел с кодом 1" || bad "код сервера $rc (ожидался 1)"
    check "причина записана в журнале" has "$d/srv.log" "не вернулся"
    local p okc=1; for p in $SA $SB $GB; do wait_exit "$p" 10; [ $? -eq 0 ] || okc=0; done
    [ $okc -eq 1 ] && ok "оставшиеся клиенты завершились корректно" || bad "клиент завис"
}

t_hung_client() {
    echo "== T12. Зависший клиент (SIGSTOP): отключение по --reply-timeout в обоих режимах"
    local mode conf
    for mode in lockstep realtime; do
        conf="$CASES/net_lockstep_long.conf"; [ $mode = realtime ] && conf="$CASES/net_long.conf"
        local d="$WORK/hung_$mode"; mkdir -p "$d"
        start_server "$d" "$conf" --reply-timeout 800 --grace 20; start_all "$d"
        sleep 0.8; kill -STOP "$GA"
        sleep 2.5
        check "[$mode] сервер отключил зависшего gun-A" has "$d/srv.log" "gun-A не отве"
        check "[$mode] сервер жив и на паузе" alive "$SP"
        kill9 "$GA"
        start_client "$d" gunA2 idz4_artillery -r A; local GA2=$CP
        sleep 1.5
        check "[$mode] бой возобновлён после замены подразделения" has "$d/srv.log" "ВОЗОБНОВЛЕНИЕ"
        kill -INT "$SP"
        all_exit_ok $SA $SB $GB $GA2 && ok "[$mode] после остановки сервера все завершились с кодом 0" || bad "[$mode] сбой"
    done
}

# Точность возобновления: бой с отключениями подразделений даёт ТОЧНО ту же последовательность
# выстрелов и итог, что и бой без отключений (tick_ms=0, фиксированный seed).
t_resume_exact() {
    echo "== T15. Возобновление подразделений после kill -9 не меняет ход боя (точное состояние ГПСЧ)"
    local d1="$WORK/det_plain" d2="$WORK/det_kill"; mkdir -p "$d1" "$d2"
    start_server "$d1" "$CASES/net_det.conf"; start_all "$d1"
    all_exit_ok $SA $SB $GA $GB >/dev/null 2>&1 && ok "контрольный бой без отключений завершён" || bad "контрольный бой не завершён"

    start_server "$d2" "$CASES/net_det.conf" --grace 30; start_all "$d2"
    sleep 0.8; kill9 "$GA"; sleep 0.4
    start_client "$d2" gunA2 idz4_artillery -r A; local GA2=$CP
    sleep 0.8; kill9 "$GB"; sleep 0.4
    start_client "$d2" gunB2 idz4_artillery -r B; local GB2=$CP
    sleep 0.8; kill9 "$GA2"; sleep 0.3
    start_client "$d2" gunA3 idz4_artillery -r A; local GA3=$CP
    all_exit_ok $SA $SB $GA3 $GB2 >/dev/null 2>&1 && ok "бой с тремя отключениями завершён" || bad "бой с отключениями не завершён"
    check "отключения действительно были" test "$(grep -c 'ВОЗОБНОВЛЕНИЕ' "$d2/srv.log")" -ge 3
    local h1 h2
    h1=$(grep -E '^\[АРТИЛЛЕРИЯ|^  ->' "$d1/srv.log" | md5sum | cut -d' ' -f1)
    h2=$(grep -E '^\[АРТИЛЛЕРИЯ|^  ->' "$d2/srv.log" | md5sum | cut -d' ' -f1)
    [ "$h1" = "$h2" ] && ok "последовательность всех выстрелов и исходов побитово совпадает" || bad "последовательности выстрелов различаются"
    "$ROOT/tests/summary.sh" "$d1/srv.log" > "$d1/s.txt"; "$ROOT/tests/summary.sh" "$d2/srv.log" > "$d2/s.txt"
    check "итоговые сводки совпадают" diff -q "$d1/s.txt" "$d2/s.txt"
}

# Много наблюдателей одновременно.
t_many_observers() {
    echo "== T16. 100 наблюдателей одновременно"
    local d="$WORK/many"; mkdir -p "$d"
    start_server "$d" "$CASES/case1.conf"
    local obs=() i
    for i in $(seq 1 100); do start_client "$d" o$i idz4_observer; obs+=("$CP"); done
    start_all "$d"
    all_exit_ok $SA $SB $GA $GB "${obs[@]}" && ok "бой завершён, все 100 наблюдателей вышли с кодом 0" || bad "сбой"
    check "сервер видел 100 наблюдателей" test "$(grep -c 'Подключён observer' "$d/srv.log")" -eq 100
}

t_startup_order() {
    echo "== T13. Клиенты можно запускать раньше сервера; --min-observers ждёт наблюдателя"
    local d="$WORK/order"; mkdir -p "$d"
    # Порт заранее неизвестен — берём фиксированный из диапазона и проверяем занятость.
    local port=$((30000 + RANDOM % 20000))
    for p in A B; do
        "$BIN/idz4_side" -s 127.0.0.1 -p $port -r $p --quiet --log "$d/side$p.log" --retries 100 --retry-ms 100 >/dev/null 2>&1 & PIDS+=($!); eval "S$p=$!"
        "$BIN/idz4_artillery" -s 127.0.0.1 -p $port -r $p --quiet --log "$d/gun$p.log" --retries 100 --retry-ms 100 >/dev/null 2>&1 & PIDS+=($!); eval "G$p=$!"
    done
    sleep 1
    "$BIN/idz4_server" --port $port --log "$d/srv.log" --quiet --min-observers 1 "$CASES/t_tick0.conf" >/dev/null 2>&1 & SP=$!; PIDS+=("$SP")
    sleep 1.5
    check "бой не начат без наблюдателя" hasnt "$d/srv.log" "СТАРТ\] Все участники"
    PORT=$port; start_client "$d" obs idz4_observer; local OB=$CP
    all_exit_ok $SA $SB $GA $GB $OB && ok "после подключения наблюдателя бой прошёл штатно" || bad "сбой"
}

t_cli_errors() {
    echo "== T14. Ошибки командной строки и конфигурации"
    local d="$WORK/cli"; mkdir -p "$d"
    "$BIN/idz4_server" --port 99999 --quiet >/dev/null 2>&1; [ $? -eq 1 ] && ok "неверный порт → код 1" || bad "неверный порт"
    "$BIN/idz4_server" --port 0 --log "$d/x.log" --quiet "$CASES/bad_cost.conf" >/dev/null 2>&1; [ $? -eq 1 ] && ok "неверная конфигурация → код 1" || bad "неверная конфигурация"
    "$BIN/idz4_server" --port 0 --log "$d/x.log" --quiet /nonexistent.conf >/dev/null 2>&1; [ $? -eq 1 ] && ok "нет файла конфигурации → код 1" || bad "нет файла"
    "$BIN/idz4_side" -s 127.0.0.1 -p 1 -r A --retries 1 --retry-ms 50 --quiet >/dev/null 2>&1; [ $? -eq 2 ] && ok "клиент без сервера → код 2" || bad "клиент без сервера"
    "$BIN/idz4_side" -s 127.0.0.1 -p 1 --quiet >/dev/null 2>&1; [ $? -eq 1 ] && ok "клиент без роли → код 1" || bad "клиент без роли"
    "$BIN/idz4_artillery" -s no.such.host.invalid -p 5000 -r A --retries 0 --quiet >/dev/null 2>&1; [ $? -eq 2 ] && ok "неразрешимое имя хоста → код 2" || bad "имя хоста"
    # порт уже занят другим сервером
    start_server "$d/a" "$CASES/t_tick0.conf"
    "$BIN/idz4_server" --port "$PORT" --log "$d/y.log" --quiet "$CASES/t_tick0.conf" >/dev/null 2>&1; [ $? -eq 1 ] && ok "занятый порт → код 1" || bad "занятый порт"
    kill -INT "$SP"; wait_exit "$SP" 5
}

ALL="lockstep_cases invariants realtime_observer observers_dynamic reconnect server_sigint server_kill9 duplicate_role garbage bad_shots absent_timeout hung_client startup_order cli_errors resume_exact many_observers"
RUN=${*:-$ALL}
for t in $RUN; do
    if declare -F "t_$t" >/dev/null; then "t_$t"; else echo "Неизвестный тест: $t (доступны: $ALL)"; fi
done
echo
echo "Итого: успешно $PASS, провалено $FAIL"
[ $FAIL -eq 0 ]
