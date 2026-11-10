#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="$ROOT/build"
TMPDIR_TEST="$(mktemp -d)"
PASS=0
TOTAL=36
SESSION="test_$$_idz3"
COORD_PID=""
WORKER_PIDS=()
DUP_WORKERS=()
CRASH_WORKERS=()
OWNER_WORKERS=()
SIGINT_WORKERS=()

cleanup() {
  if [[ -n "$COORD_PID" ]] && kill -0 "$COORD_PID" 2>/dev/null; then
    kill -INT "$COORD_PID" 2>/dev/null || true
  fi
  for pid in "${WORKER_PIDS[@]:-}" "${DUP_WORKERS[@]:-}" "${CRASH_WORKERS[@]:-}" "${OWNER_WORKERS[@]:-}" "${SIGINT_WORKERS[@]:-}"; do
    kill -0 "$pid" 2>/dev/null && kill -TERM "$pid" 2>/dev/null || true
  done
  rm -rf "$TMPDIR_TEST"
}
trap cleanup EXIT INT TERM

ok() { PASS=$((PASS + 1)); printf '[%02d/%02d] %s\n' "$PASS" "$TOTAL" "$1"; }
fail() { echo "[FAIL] $1" >&2; exit 1; }

[[ -x "$BUILD/IDZ3_named" && -x "$BUILD/IDZ3_unnamed" ]] || fail "Сначала соберите проект: cmake --build build"

CASES=(case1.conf case3_area.conf case_area_only.conf case_area_zero.conf
       case_budget.conf case_destroyed_cell.conf case_replenish.conf
       case_tiny.conf case_types.conf t_tick0.conf t_tick500.conf)

for binary in IDZ3_named IDZ3_unnamed; do
  for case_file in "${CASES[@]}"; do
    output="$TMPDIR_TEST/${binary}_${case_file}.out"
    if ! timeout 30s "$BUILD/$binary" "scripts/cases/$case_file" >"$output" 2>&1; then
      tail -80 "$output" >&2 || true
      fail "$binary / $case_file завершился с ошибкой"
    fi
    grep -q '========== ИТОГИ IDZ3 ==========' "$output" || fail "$binary / $case_file: нет итоговой статистики"
    grep -q '========== ЗАВЕРШЕНИЕ ==========' "$output" || fail "$binary / $case_file: нет штатного завершения"
    ok "$binary: $case_file"
  done
done

# Регрессия: лимит в один такт не должен обрывать временной интервал,
# в течение которого обе артиллерии выполняют по одному выстрелу.
for binary in IDZ3_named IDZ3_unnamed; do
  output="$TMPDIR_TEST/${binary}_tick1_delayed.out"
  timeout 10s "$BUILD/$binary" scripts/cases/t_tick1_delayed.conf >"$output" 2>&1 || {
    tail -80 "$output" >&2 || true
    fail "$binary: последний такт не завершился корректно"
  }
  shots=$(grep -c 'Выстрелов всего: 1' "$output" || true)
  [[ "$shots" -eq 2 ]] || fail "$binary: ожидалось по одному выстрелу каждой артиллерии за последний такт, найдено статистик: $shots"
  grep -q 'Лимит тактов: 1' "$output" || fail "$binary: причина остановки не соответствует лимиту"
  ok "$binary: полный последний такт при max_ticks=1"
done

for binary in IDZ3_named IDZ3_unnamed; do
  output="$TMPDIR_TEST/${binary}_signal.out"
  "$BUILD/$binary" scripts/cases/case2.conf >"$output" 2>&1 &
  pid=$!
  sleep 0.6
  kill -INT "$pid" 2>/dev/null || true
  set +e
  wait "$pid"
  status=$?
  set -e
  [[ $status -eq 0 ]] || fail "$binary: SIGINT вернул код $status"
  grep -q 'Прерывание' "$output" || fail "$binary: SIGINT не отражён в отчёте"
  ok "$binary: корректное завершение по SIGINT"
done

if "$BUILD/IDZ3_named" scripts/cases/bad_cost.conf >"$TMPDIR_TEST/bad_config.out" 2>&1; then
  fail "Некорректная конфигурация неожиданно принята"
fi
ok "Отклонение некорректной конфигурации"

# Передача каталога вместо файла вызывает EISDIR при read(). Это должно
# считаться ошибкой ввода, а не штатным EOF и не пустой конфигурацией.
if "$BUILD/IDZ3_named" "$ROOT" >"$TMPDIR_TEST/read_error.out" 2>&1; then
  fail "Ошибка read() неожиданно принята как конец конфигурации"
fi
grep -q 'Ошибка чтения файла' "$TMPDIR_TEST/read_error.out" || fail "Системная ошибка read() не диагностирована"
ok "Отдельная обработка ошибки read() (EISDIR)"

"$BUILD/IDZ3_init" "$SESSION" scripts/cases/t_tick0.conf >"$TMPDIR_TEST/init.out" 2>&1 || fail "IDZ3_init"
"$BUILD/IDZ3_coordinator" "$SESSION" >"$TMPDIR_TEST/coordinator.out" 2>&1 &
COORD_PID=$!
sleep 0.15
"$BUILD/IDZ3_side" "$SESSION" A >"$TMPDIR_TEST/sideA.out" 2>&1 & WORKER_PIDS+=("$!")
"$BUILD/IDZ3_side" "$SESSION" B >"$TMPDIR_TEST/sideB.out" 2>&1 & WORKER_PIDS+=("$!")
"$BUILD/IDZ3_artillery" "$SESSION" A >"$TMPDIR_TEST/artA.out" 2>&1 & WORKER_PIDS+=("$!")
"$BUILD/IDZ3_artillery" "$SESSION" B >"$TMPDIR_TEST/artB.out" 2>&1 & WORKER_PIDS+=("$!")
if ! wait "$COORD_PID"; then tail -80 "$TMPDIR_TEST/coordinator.out" >&2; fail "Независимый координатор"; fi
COORD_PID=""
for pid in "${WORKER_PIDS[@]}"; do wait "$pid" || fail "Независимый рабочий процесс"; done
WORKER_PIDS=()
grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/coordinator.out" || fail "Независимые процессы не зарегистрировались"
grep -q '========== ЗАВЕРШЕНИЕ ==========' "$TMPDIR_TEST/coordinator.out" || fail "Независимая модель не завершилась"
ok "Четыре независимо запускаемых участника + координатор"

# Конфигурация для длительного такта, чтобы можно было проверить регистрацию
# дубликата и потерю процесса во время моделирования.
cat >"$TMPDIR_TEST/long_tick.conf" <<EOF
max_ticks=5
tick_ms=2500
log=$TMPDIR_TEST/independent.log
A.rows=3
A.cols=3
A.ammo=20
A.accuracy=0
A.reaction_ms=0
A.reaction_jitter_ms=0
A.target=0 0 10
B.rows=3
B.cols=3
B.ammo=20
B.accuracy=0
B.reaction_ms=0
B.reaction_jitter_ms=0
B.target=0 0 10
EOF

# SIGINT в независимом режиме должен штатно остановить координатор и всех
# отдельно запущенных участников, после чего владелец удаляет IPC-ресурсы.
SIG_SESSION="sig_$$_idz3"
"$BUILD/IDZ3_init" "$SIG_SESSION" "$TMPDIR_TEST/long_tick.conf" >"$TMPDIR_TEST/sig_init.out" 2>&1 || fail "Инициализация independent-SIGINT сессии"
"$BUILD/IDZ3_coordinator" "$SIG_SESSION" >"$TMPDIR_TEST/sig_coord.out" 2>&1 & COORD_PID=$!
sleep 0.05
"$BUILD/IDZ3_side" "$SIG_SESSION" A >"$TMPDIR_TEST/sig_side_a.out" 2>&1 & SIGINT_WORKERS+=("$!")
"$BUILD/IDZ3_side" "$SIG_SESSION" B >"$TMPDIR_TEST/sig_side_b.out" 2>&1 & SIGINT_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$SIG_SESSION" A >"$TMPDIR_TEST/sig_art_a.out" 2>&1 & SIGINT_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$SIG_SESSION" B >"$TMPDIR_TEST/sig_art_b.out" 2>&1 & SIGINT_WORKERS+=("$!")
for _ in $(seq 1 100); do
  grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/sig_coord.out" && break
  sleep 0.05
done
grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/sig_coord.out" || fail "Независимые участники не зарегистрировались перед SIGINT"
kill -INT "$COORD_PID" 2>/dev/null || true
set +e
wait "$COORD_PID"
SIG_COORD_STATUS=$?
set -e
COORD_PID=""
[[ "$SIG_COORD_STATUS" -eq 0 ]] || fail "Независимый координатор: SIGINT вернул код $SIG_COORD_STATUS"
for pid in "${SIGINT_WORKERS[@]}"; do wait "$pid" || fail "Участник не завершился после SIGINT координатора"; done
SIGINT_WORKERS=()
grep -q 'Причина: прерывание пользователем' "$TMPDIR_TEST/sig_coord.out" || fail "Независимый координатор не отразил SIGINT"
if compgen -G "/dev/shm/sem.idz3_${SIG_SESSION}_*" >/dev/null || [[ -e "/dev/shm/idz3_shm_${SIG_SESSION}" ]]; then
  fail "После независимого SIGINT остались IPC-ресурсы"
fi
ok "SIGINT независимой модели и очистка IPC"

# Дубликат side-A не должен засчитываться как недостающий side-B.
DUP_SESSION="dup_$$_idz3"
"$BUILD/IDZ3_init" "$DUP_SESSION" "$TMPDIR_TEST/long_tick.conf" >"$TMPDIR_TEST/dup_init.out" 2>&1 || fail "Инициализация duplicate-сессии"
"$BUILD/IDZ3_coordinator" "$DUP_SESSION" >"$TMPDIR_TEST/dup_coord.out" 2>&1 & COORD_PID=$!
sleep 0.05
"$BUILD/IDZ3_side" "$DUP_SESSION" A >"$TMPDIR_TEST/dup_side_a1.out" 2>&1 & DUP_WORKERS+=("$!")
"$BUILD/IDZ3_side" "$DUP_SESSION" B >"$TMPDIR_TEST/dup_side_b.out" 2>&1 & DUP_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$DUP_SESSION" A >"$TMPDIR_TEST/dup_art_a.out" 2>&1 & DUP_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$DUP_SESSION" B >"$TMPDIR_TEST/dup_art_b.out" 2>&1 & DUP_WORKERS+=("$!")
for _ in $(seq 1 100); do
  grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/dup_coord.out" && break
  sleep 0.05
done
grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/dup_coord.out" || fail "Участники не зарегистрировались перед duplicate-тестом"
if "$BUILD/IDZ3_side" "$DUP_SESSION" A >"$TMPDIR_TEST/dup_side_a2.out" 2>&1; then
  fail "Повторная регистрация side-A неожиданно принята"
fi
set +e
wait "$COORD_PID"
DUP_COORD_STATUS=$?
set -e
COORD_PID=""
for pid in "${DUP_WORKERS[@]}"; do wait "$pid" || fail "Рабочий процесс duplicate-теста"; done
DUP_WORKERS=()
[[ "$DUP_COORD_STATUS" -ne 0 ]] || fail "Координатор не распознал дублирующую роль"
grep -q 'Повторная/недопустимая регистрация' "$TMPDIR_TEST/dup_side_a2.out" || fail "Нет диагностики повторной регистрации"
ok "Отклонение повторной регистрации одной роли"

# Убийство обязательного независимого процесса во время длительного такта
# должно обнаруживаться координатором и останавливать всю сессию.
CRASH_SESSION="crash_$$_idz3"
"$BUILD/IDZ3_init" "$CRASH_SESSION" "$TMPDIR_TEST/long_tick.conf" >"$TMPDIR_TEST/crash_init.out" 2>&1 || fail "Инициализация crash-сессии"
"$BUILD/IDZ3_coordinator" "$CRASH_SESSION" >"$TMPDIR_TEST/crash_coord.out" 2>&1 & COORD_PID=$!
sleep 0.05
"$BUILD/IDZ3_side" "$CRASH_SESSION" A >"$TMPDIR_TEST/crash_side_a.out" 2>&1 & CRASH_WORKERS+=("$!")
"$BUILD/IDZ3_side" "$CRASH_SESSION" B >"$TMPDIR_TEST/crash_side_b.out" 2>&1 & CRASH_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$CRASH_SESSION" A >"$TMPDIR_TEST/crash_art_a.out" 2>&1 & CRASH_KILL_PID=$!; CRASH_WORKERS+=("$CRASH_KILL_PID")
"$BUILD/IDZ3_artillery" "$CRASH_SESSION" B >"$TMPDIR_TEST/crash_art_b.out" 2>&1 & CRASH_WORKERS+=("$!")
for _ in $(seq 1 100); do
  grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/crash_coord.out" && break
  sleep 0.05
done
grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/crash_coord.out" || fail "Участники не зарегистрировались перед crash-тестом"
kill -KILL "$CRASH_KILL_PID" 2>/dev/null || true
set +e
wait "$COORD_PID"
CRASH_COORD_STATUS=$?
set -e
COORD_PID=""
for pid in "${CRASH_WORKERS[@]}"; do wait "$pid" 2>/dev/null || true; done
CRASH_WORKERS=()
[[ "$CRASH_COORD_STATUS" -ne 0 ]] || fail "Координатор не обнаружил аварийную смерть рабочего процесса"
grep -q 'Аварийное завершение процессной модели' "$TMPDIR_TEST/crash_coord.out" || fail "Нет диагностики аварийно завершившегося процесса"
ok "Обнаружение аварийной смерти независимого участника"

# В родительском режиме координатор должен обнаруживать завершение своего
# дочернего процесса через waitpid(WNOHANG), а не ждать ACK до тайм-аута.
PARENT_CRASH_OUT="$TMPDIR_TEST/parent_crash.out"
"$BUILD/IDZ3_named" "$TMPDIR_TEST/long_tick.conf" >"$PARENT_CRASH_OUT" 2>&1 & PARENT_PID=$!
CHILD_PID=""
for _ in $(seq 1 200); do
  CHILD_PID=$(sed -n 's/.*роль=artillery-A, PID=\([0-9][0-9]*\).*/\1/p' "$PARENT_CRASH_OUT" | head -n 1)
  [[ -n "$CHILD_PID" ]] && break
  sleep 0.02
done
if [[ -z "$CHILD_PID" ]]; then
  kill -KILL "$PARENT_PID" 2>/dev/null || true
  wait "$PARENT_PID" 2>/dev/null || true
  fail "Не удалось получить PID дочерней артиллерии для crash-теста"
fi
kill -KILL "$CHILD_PID" 2>/dev/null || true
set +e
wait "$PARENT_PID"
PARENT_CRASH_STATUS=$?
set -e
[[ "$PARENT_CRASH_STATUS" -ne 0 ]] || fail "Родительский координатор не обнаружил смерть дочерней артиллерии"
grep -q 'Аварийное завершение процессной модели' "$PARENT_CRASH_OUT" || fail "Нет диагностики смерти дочернего процесса"
ok "Обнаружение аварийной смерти дочернего процесса в fork-режиме"

# Проверяем robust process-shared mutex: смерть владельца внутри критической
# секции должна переводить модель в fail-stop, а не продолжать игру с частично
# изменённым состоянием.
"${CC:-cc}" -std=gnu11 -D_POSIX_C_SOURCE=200809L -Iinclude \
  tests/hold_mutex.c build/libidz3_core.a -pthread -lrt \
  -o "$TMPDIR_TEST/hold_mutex" || fail "Сборка helper для EOWNERDEAD-теста"
OWNER_SESSION="owner_$$_idz3"
"$BUILD/IDZ3_init" "$OWNER_SESSION" "$TMPDIR_TEST/long_tick.conf" >"$TMPDIR_TEST/owner_init.out" 2>&1 || fail "Инициализация owner-death сессии"
"$BUILD/IDZ3_coordinator" "$OWNER_SESSION" >"$TMPDIR_TEST/owner_coord.out" 2>&1 & COORD_PID=$!
sleep 0.05
"$BUILD/IDZ3_side" "$OWNER_SESSION" A >"$TMPDIR_TEST/owner_side_a.out" 2>&1 & OWNER_WORKERS+=("$!")
"$BUILD/IDZ3_side" "$OWNER_SESSION" B >"$TMPDIR_TEST/owner_side_b.out" 2>&1 & OWNER_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$OWNER_SESSION" A >"$TMPDIR_TEST/owner_art_a.out" 2>&1 & OWNER_WORKERS+=("$!")
"$BUILD/IDZ3_artillery" "$OWNER_SESSION" B >"$TMPDIR_TEST/owner_art_b.out" 2>&1 & OWNER_WORKERS+=("$!")
for _ in $(seq 1 100); do
  grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/owner_coord.out" && break
  sleep 0.05
done
grep -q 'Все четыре независимых участника подключены' "$TMPDIR_TEST/owner_coord.out" || fail "Участники не зарегистрировались перед owner-death-тестом"
"$TMPDIR_TEST/hold_mutex" "$OWNER_SESSION" >"$TMPDIR_TEST/owner_holder.out" 2>&1 & OWNER_HOLDER_PID=$!
for _ in $(seq 1 100); do
  grep -q '^LOCKED$' "$TMPDIR_TEST/owner_holder.out" && break
  sleep 0.02
done
grep -q '^LOCKED$' "$TMPDIR_TEST/owner_holder.out" || fail "Helper не захватил process-shared mutex"
kill -KILL "$OWNER_HOLDER_PID" 2>/dev/null || true
set +e
wait "$COORD_PID"
OWNER_COORD_STATUS=$?
set -e
COORD_PID=""
for pid in "${OWNER_WORKERS[@]}"; do wait "$pid" 2>/dev/null || true; done
OWNER_WORKERS=()
wait "$OWNER_HOLDER_PID" 2>/dev/null || true
[[ "$OWNER_COORD_STATUS" -ne 0 ]] || fail "Смерть владельца mutex не остановила моделирование"
grep -q 'Причина: ошибка процессной модели' "$TMPDIR_TEST/owner_coord.out" || fail "EOWNERDEAD не отражён как ошибка модели"
ok "Fail-stop при смерти владельца robust process-shared mutex (EOWNERDEAD)"

if compgen -G '/dev/shm/sem.idz3_*' >/dev/null; then
  fail "После тестов остались именованные POSIX-семафоры"
fi
ok "Очистка именованных POSIX-семафоров"

if compgen -G '/dev/shm/idz3_shm_*' >/dev/null; then
  fail "После тестов остались объекты POSIX Shared Memory"
fi
ok "Очистка POSIX Shared Memory после завершения"

echo "Успешно: $PASS/$TOTAL проверок."
