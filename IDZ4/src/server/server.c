// server.c — сервер-координатор «мортирной дуэли» (IDZ4, TCP).
//
// Один процесс, один поток, мультиплексирование через poll(). Сервер владеет
// единственным достоверным состоянием обеих территорий и боезапаса. Участники
// (государства и огневые подразделения) — отдельные клиентские программы,
// которые общаются с сервером короткими текстовыми сообщениями (см. proto.h).
//
// Такт модели:
//   1) фаза СТОРОН: сервер шлёт TICK государствам; каждое при необходимости
//      присылает RESUPPLY и обязательно ACK;
//   2) сервер проверяет условия прекращения огня и шлёт GUNTICK подразделениям;
//   3) фаза ОГНЯ: подразделения присылают SHOT/PASS; сервер регистрирует выстрелы;
//   4) конец такта: при tick_ms=0 — когда ответили все, иначе — по истечении tick_ms.
// Если участник пропал, моделирование ставится на ПАУЗУ до его возвращения.
#define _POSIX_C_SOURCE 200809L
#include "battle.h"
#include "colors.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "signals.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLIENTS        1024                    // 4 роли + наблюдатели (ещё ограничено ulimit -n)
#define HELLO_TIMEOUT_MS   5000
#define WMAX_BYTES         (16u * 1024u * 1024u)   // предел очереди отправки одного клиента
#define MAP_MAX_ROWS       60                      // карты крупнее наблюдателям не рисуются
#define MAP_MAX_COLS       150

typedef struct {
    conn_t conn;
    int    used;
    int    role;          // ROLE_NONE до получения HELLO
    char   peer[64];
    long   since_ms;
    long   last_rx_ms;    // когда от клиента в последний раз приходила строка (для PING/PONG)
    int    bad;           // счётчик некорректных сообщений
} client_t;

typedef enum { PH_LOBBY = 0, PH_SIDES, PH_FIRE, PH_DONE } phase_t;

typedef struct { int has; char kind; int r, c, roll; } pending_t;

typedef struct {
    config_t config;
    int      listen_fd;
    client_t cl[MAX_CLIENTS];
    int      slot[ROLE_PARTICIPANTS];     // индекс клиента в cl[] или -1

    phase_t  phase;
    int      paused;
    long     pause_since;
    int      tick, ticks_completed;
    int      side_acked[2];
    int      gun_done[2];
    pending_t pend[2];                    // отложенные выстрелы (режим tick_ms=0)
    long     side_wait_since, fire_started;
    int      last_resupply[2];
    int      next_area_tick[2];
    unsigned gun_seed[2];
    int      stop_reason;

    int      opt_grace_ms, opt_reply_ms, opt_min_obs, opt_history;
    long     last_ping_ms;

    // Точное состояние ГПСЧ и обхода секторов каждого подразделения (присылается в SHOT):
    // позволяет продолжить бой после переподключения так, как будто разрыва не было.
    unsigned gun_rng[2];
    int      gun_sector[2];
    int      gun_has_state[2];

    // История последних строк журнала для поздно подключившихся наблюдателей.
    char   **hist;
    int      hist_head, hist_count;

    // Накопленный «заголовок» журнала: его получают наблюдатели при подключении.
    sbuf_t   banner;
    int      capturing;
    long     started_ms;
    int      rejected[2];                 // отклонённые выстрелы по сторонам
} server_t;

static server_t S;

// ---------------------------------------------------------------- вспомогательное

static side_t *side(int i) { return i == 0 ? &S.config.A : &S.config.B; }

static int is_lockstep(void) { return S.config.tick_ms <= 0; }

static client_t *client_of_role(int role) {
    if (role < 0 || role >= ROLE_PARTICIPANTS) return NULL;
    return S.slot[role] >= 0 ? &S.cl[S.slot[role]] : NULL;
}

// Сообщение участнику по роли (если он подключён).
static void send_role(int role, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_role(int role, const char *fmt, ...) {
    client_t *c = client_of_role(role);
    if (!c) return;
    va_list ap;
    va_start(ap, fmt);
    conn_vsend(&c->conn, fmt, ap);
    va_end(ap);
}

static int count_observers(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (S.cl[i].used && S.cl[i].role == ROLE_OBSERVER) n++;
    return n;
}

static int count_participants(void) {
    int n = 0;
    for (int r = 0; r < ROLE_PARTICIPANTS; r++) if (S.slot[r] >= 0) n++;
    return n;
}

// Разбить многострочный текст на строки и отправить как <TAG> <строка> одному клиенту.
static void send_text_lines(conn_t *c, const char *tag, const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        size_t n = j - i;
        if (n > 3900) n = 3900;                       // строка протокола не длиннее PROTO_MAX_LINE
        conn_send(c, "%s %.*s", tag, (int)n, text + i);
        i = j + 1;
    }
}

// Кольцевой буфер последних строк журнала (для наблюдателей, подключившихся позже).
static void hist_add(const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        size_t n = j - i;
        if (n > 3900) n = 3900;
        char *line = (char *)malloc(n + 1);
        if (line) {
            memcpy(line, text + i, n);
            line[n] = '\0';
            const int pos = (S.hist_head + S.hist_count) % S.opt_history;
            if (S.hist_count == S.opt_history) {      // буфер полон — вытесняем самую старую строку
                free(S.hist[S.hist_head]);
                S.hist[S.hist_head] = line;
                S.hist_head = (S.hist_head + 1) % S.opt_history;
            } else {
                S.hist[pos] = line;
                S.hist_count++;
            }
        }
        i = j + 1;
    }
}

// Приёмник log_printf: всё, что сервер печатает «для всех», уходит наблюдателям.
static void obs_sink(const char *text, size_t len, void *ctx) {
    (void)ctx;
    if (S.capturing) sb_printf(&S.banner, "%.*s", (int)len, text);
    else if (S.opt_history > 0 && S.hist) hist_add(text, len);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (S.cl[i].used && S.cl[i].role == ROLE_OBSERVER)
            send_text_lines(&S.cl[i].conn, "EVT", text, len);
}

// Текст только наблюдателям (и в журнал-файл): подробные карты.
static void observers_only(const char *text) {
    log_file("%s", text);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (S.cl[i].used && S.cl[i].role == ROLE_OBSERVER)
            send_text_lines(&S.cl[i].conn, "EVT", text, strlen(text));
}

// Сообщение всем: консоль сервера, журнал, наблюдатели и участники (TXT).
static void notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notice(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf("%s\n", buf);
    for (int r = 0; r < ROLE_PARTICIPANTS; r++) {
        client_t *c = client_of_role(r);
        if (c) conn_send(&c->conn, "TXT %s", buf);
    }
}

static void render_maps(sbuf_t *sb) {
    if (S.config.A.grid.rows > MAP_MAX_ROWS || S.config.A.grid.cols > MAP_MAX_COLS ||
        S.config.B.grid.rows > MAP_MAX_ROWS || S.config.B.grid.cols > MAP_MAX_COLS) {
        sb_printf(sb, "(карты слишком велики для вывода; см. сводку по территориям)\n");
        sb_printf(sb, "Территория A: живых целей %d, стоимость %" PRId64 "\n",
                  S.config.A.grid.alive_targets, S.config.A.grid.alive_cost);
        sb_printf(sb, "Территория B: живых целей %d, стоимость %" PRId64 "\n",
                  S.config.B.grid.alive_targets, S.config.B.grid.alive_cost);
        return;
    }
    grid_render(&S.config.A.grid, "Территория A", 0, sb);
    grid_render(&S.config.B.grid, "Территория B", 0, sb);
}

// ---------------------------------------------------------------- удаление клиента

static void lobby_status(void) {
    char missing[128] = "";
    for (int r = 0; r < ROLE_PARTICIPANTS; r++) {
        if (S.slot[r] >= 0) continue;
        if (missing[0]) strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
        strncat(missing, role_name(r), sizeof(missing) - strlen(missing) - 1);
    }
    if (missing[0])
        notice("[ЛОББИ] Подключено участников: %d/%d. Ожидаются: %s", count_participants(),
               ROLE_PARTICIPANTS, missing);
    else if (count_observers() < S.opt_min_obs)
        notice("[ЛОББИ] Все участники подключены. Ожидаются наблюдатели: %d/%d",
               count_observers(), S.opt_min_obs);
}

static void remove_client(int idx, const char *why) {
    client_t *c = &S.cl[idx];
    if (!c->used) return;
    const int role = c->role;
    log_printf("[СЕТЬ] Отключён %s (%s): %s\n", role == ROLE_NONE ? "неопознанный клиент" : role_name(role),
               c->peer, why);
    conn_close(&c->conn);
    c->used = 0;
    if (role >= 0 && role < ROLE_PARTICIPANTS) {
        S.slot[role] = -1;
        if (S.phase == PH_SIDES || S.phase == PH_FIRE) {
            if (!S.paused) {
                S.paused = 1;
                S.pause_since = now_ms();
                for (int r = 0; r < ROLE_PARTICIPANTS; r++) {
                    client_t *o = client_of_role(r);
                    if (o) conn_send(&o->conn, "PAUSE участник %s отключился", role_name(role));
                }
                log_printf("[ПАУЗА] Участник %s отключился. Моделирование приостановлено на такте %d; "
                           "ожидание возвращения до %d с.\n", role_name(role), S.tick, S.opt_grace_ms / 1000);
            }
        } else if (S.phase == PH_LOBBY) {
            lobby_status();
        }
    }
}

// ---------------------------------------------------------------- приветствие

static void send_state(conn_t *c, const char *key, int64_t v) { conn_send(c, "STATE %s %" PRId64, key, v); }

static void send_map_rows(conn_t *c, char tag, const grid_t *g) {
    char *row = (char *)malloc((size_t)g->cols + 1);
    if (!row) return;
    for (int r = 0; r < g->rows; r++) {
        grid_row_chars(g, r, row);
        conn_send(c, "MAP %c %d %s", tag, r, row);
    }
    free(row);
}

static void send_targets(conn_t *c, char tag, const grid_t *g) {
    int sent = 0;
    for (int r = 0; r < g->rows && sent < PROTO_MAX_TGT; r++)
        for (int col = 0; col < g->cols && sent < PROTO_MAX_TGT; col++) {
            const cell_t *cell = &g->cells[(size_t)r * (size_t)g->cols + (size_t)col];
            if (cell->state == CELL_EMPTY) continue;
            conn_send(c, "TGT %c %d %d %d %s", tag, r, col, cell->cost, grid_type_name(g, cell));
            sent++;
        }
}

static const char *strategy_token(strategy_t s) {
    return s == STRAT_RANDOM ? "random" : s == STRAT_CENTER ? "center" : "sector";
}

static void send_status_to_observer(conn_t *c) {
    sbuf_t sb;
    sb_init(&sb);
    sb_printf(&sb, "\n--- Текущее состояние (такт %d, %s) ---\n", S.tick,
              S.phase == PH_LOBBY ? "ожидание участников" : S.paused ? "пауза" : "идёт бой");
    side_render_stats(&S.config.A, &sb);
    side_render_stats(&S.config.B, &sb);
    render_maps(&sb);
    send_text_lines(c, "EVT", sb_str(&sb), sb.len);
    sb_free(&sb);
}

static void send_welcome(client_t *cl) {
    conn_t *c = &cl->conn;
    const int role = cl->role;
    conn_send(c, "WELCOME %s %d", role_name(role), PROTO_VERSION);
    conn_send(c, "CFG tick_ms %d", S.config.tick_ms);
    conn_send(c, "CFG max_ticks %d", S.config.max_ticks);

    if (role == ROLE_OBSERVER) {
        // Наблюдатель получает заголовок журнала и текущую сводку.
        send_text_lines(c, "EVT", sb_str(&S.banner), S.banner.len);
        if (S.hist_count > 0) {
            conn_send(c, "EVT --- ранее (последние %d строк журнала) ---", S.hist_count);
            for (int k = 0; k < S.hist_count; k++)
                conn_send(c, "EVT %s", S.hist[(S.hist_head + k) % S.opt_history]);
            conn_send(c, "EVT --- конец истории ---");
        }
        if (S.phase != PH_LOBBY) send_status_to_observer(c);
    } else {
        const int i = role_side_index(role);
        side_t *me = side(i), *enemy = side(1 - i);
        const artillery_t *a = &me->art;
        conn_send(c, "CFG self_name %s", me->name);
        conn_send(c, "CFG enemy_name %s", enemy->name);
        conn_send(c, "CFG self_rows %d", me->grid.rows);
        conn_send(c, "CFG self_cols %d", me->grid.cols);
        conn_send(c, "CFG enemy_rows %d", enemy->grid.rows);   // размеры поля противника известны, цели — нет
        conn_send(c, "CFG enemy_cols %d", enemy->grid.cols);
        if (role_is_side(role)) {
            conn_send(c, "CFG replenish_every %d", a->replenish_every);
            conn_send(c, "CFG replenish_amount %d", a->replenish_amount);
            send_state(c, "active", me->active);
            send_state(c, "alive_targets", me->grid.alive_targets);
            send_state(c, "alive_cost", me->grid.alive_cost);
            send_state(c, "lost_cost", me->lost_cost);
            send_state(c, "initial_targets", me->grid.initial_targets);
            send_state(c, "initial_cost", me->grid.initial_cost);
            send_state(c, "ammo_left", a->ammo_left);
            send_state(c, "last_resupply", S.last_resupply[i]);
            send_map_rows(c, i == 0 ? 'A' : 'B', &me->grid);     // государство видит только свою карту
            send_targets(c, i == 0 ? 'A' : 'B', &me->grid);
        } else {
            conn_send(c, "CFG strategy %s", strategy_token(a->strategy));
            conn_send(c, "CFG accuracy %d", a->accuracy);
            conn_send(c, "CFG reaction_ms %d", a->reaction_ms);
            conn_send(c, "CFG jitter_ms %d", a->reaction_jitter_ms);
            conn_send(c, "CFG area_every %d", a->area_every);
            conn_send(c, "CFG area_radius %d", a->area_radius);
            conn_send(c, "CFG gun_seed %u", S.gun_seed[i]);
            send_state(c, "active", me->active);
            send_state(c, "ammo_left", a->ammo_left);
            send_state(c, "area_ammo", a->area_ammo);
            send_state(c, "spent", a->spent_cost);
            send_state(c, "shots_fired", a->shots_fired);
            if (S.gun_has_state[i]) {                 // точное возобновление после переподключения
                send_state(c, "rng_state", S.gun_rng[i]);
                send_state(c, "sector_pos", S.gun_sector[i]);
            }
        }
    }
    conn_send(c, "WELCOME_END %d %d", (int)S.phase, S.tick);
}

// ---------------------------------------------------------------- шаги такта

static void send_guntick(int i) {
    side_t *s = side(i);
    const artillery_t *a = &s->art;
    const int reg = s->active && artillery_can_fire(a);
    const int area = s->active && artillery_can_area_strike(a);
    const int due = area && a->area_every > 0 && S.tick >= S.next_area_tick[i];
    send_role(ROLE_GUN_A + i, "GUNTICK %d %d %d %d %" PRId64 " %d %" PRId64 " %d",
              S.tick, reg, area, due, a->ammo_left, a->area_ammo, a->spent_cost, a->shots_fired);
}

// Подсказка роли о текущей фазе — после (пере)подключения во время боя.
static void send_prompt(int role) {
    if (S.phase == PH_SIDES && role_is_side(role) && !S.side_acked[role_side_index(role)])
        send_role(role, "TICK %d", S.tick);
    else if (S.phase == PH_FIRE && role_is_gun(role)) {
        const int i = role_side_index(role);
        if (side(i)->active && !S.gun_done[i]) send_guntick(i);
    }
}

// Проверить условия прекращения огня обеих сторон; о смене состояния сообщить.
static void check_ceasefire(void) {
    for (int i = 0; i < 2; i++) {
        char why[256];
        if (!battle_check_done(side(i), side(1 - i), why, sizeof(why))) continue;
        log_printf("[%s] ПРЕКРАЩАЕТ ОГОНЬ: %s.\n", side(i)->name, why);
        send_role(ROLE_SIDE_A + i, "CEASEFIRE %s", why);
        send_role(ROLE_GUN_A + i, "CEASEFIRE %s", why);
    }
}

static int both_inactive(void) { return !S.config.A.active && !S.config.B.active; }

static void finish_match(stop_reason_t reason);
static void reject_shot(int i, int tick, reject_t why);

static void begin_tick(int n) {
    S.tick = n;
    S.phase = PH_SIDES;
    S.side_acked[0] = S.side_acked[1] = 0;
    S.gun_done[0] = S.gun_done[1] = 0;
    S.pend[0].has = S.pend[1].has = 0;
    S.side_wait_since = now_ms();
    log_printf("\n========== ТАКТ %d ==========\n", n);
    send_role(ROLE_SIDE_A, "TICK %d", n);
    send_role(ROLE_SIDE_B, "TICK %d", n);
}

static void begin_fire(void) {
    check_ceasefire();
    if (both_inactive()) {
        S.ticks_completed = S.tick + 1;
        finish_match(STOP_NATURAL);
        return;
    }
    S.phase = PH_FIRE;
    S.fire_started = now_ms();
    for (int i = 0; i < 2; i++) {
        if (!side(i)->active) { S.gun_done[i] = 1; continue; }
        send_guntick(i);
        // Подразделение без возможности стрелять отвечает PASS; не ждём его в режиме tick_ms=0.
        if (!artillery_can_fire(&side(i)->art) && !artillery_can_area_strike(&side(i)->art))
            S.gun_done[i] = 1;
    }
}

// Выполнить зарегистрированный выстрел стороны i и разослать последствия.
static void execute_shot(int i, char kind, int r, int c, int roll, int check_now) {
    side_t *sh = side(i), *vi = side(1 - i);
    shot_result_t res;
    battle_execute(sh, vi, kind, r, c, roll, &res);

    // Расписание тяжёлых залпов: следующий — не раньше чем через area_every тактов.
    if (kind == 'H' && sh->art.area_every > 0 && S.tick >= S.next_area_tick[i]) {
        do { S.next_area_tick[i] += sh->art.area_every; } while (S.next_area_tick[i] <= S.tick);
    }

    sbuf_t txt;
    sb_init(&txt);
    battle_describe(sh, vi, S.tick, &res, &txt);
    log_printf("%s", sb_str(&txt));
    sb_free(&txt);

    // Стрелку — итог выстрела (без сведений о составе территории противника).
    send_role(ROLE_GUN_A + i, "RESULT %d ok %s %c %d %d %d %d %" PRId64 " %" PRId64 " %d %" PRId64,
              S.tick, outcome_token(res.outcome), kind, r, c, roll, res.destroyed,
              res.destroyed_cost, sh->art.ammo_left, sh->art.area_ammo, sh->art.spent_cost);

    // Пострадавшему государству — координаты падения и потерянные цели.
    for (int k = 0; k < res.kills_n; k++)
        send_role(ROLE_SIDE_A + (1 - i), "DTGT %d %d %d %s",
                  res.kills[k].r, res.kills[k].c, res.kills[k].cost, res.kills[k].type);
    send_role(ROLE_SIDE_A + (1 - i), "IMPACT %d %c %d %d %s %d %" PRId64 " %d %" PRId64 " %" PRId64,
              S.tick, kind, r, c, outcome_token(res.outcome), res.destroyed, res.destroyed_cost,
              vi->grid.alive_targets, vi->grid.alive_cost, vi->lost_cost);

    if (res.destroyed > 0) {
        sbuf_t maps;
        sb_init(&maps);
        sb_printf(&maps, "\n>>> Состояние территорий после изменений:\n");
        render_maps(&maps);
        observers_only(sb_str(&maps));
        sb_free(&maps);
    }
    battle_result_free(&res);
    // Условия прекращения огня проверяются сразу после каждого выстрела (как в IDZ3).
    // check_now=0 оставлен для режимов, где проверку нужно отложить.
    if (check_now) check_ceasefire();
}

// Конец такта: ответы приняты (или вышло время).
static void end_tick(void) {
    // Режим tick_ms=0: оба подразделения прислали выстрел «одновременно». В IDZ3 порядок внутри
    // такта определялся гонкой процессов; здесь он фиксирован и ЧЕРЕДУЕТСЯ по чётности такта
    // (чётный — первой A, нечётный — первой B), поэтому результат воспроизводим и ни одна сторона
    // не имеет постоянного преимущества. Как и в IDZ3, выстрел, уничтоживший последние цели
    // противника, останавливает его огонь немедленно: его выстрел в этом такте не исполняется.
    const int first = (S.tick % 2 == 0) ? 0 : 1;
    for (int k = 0; k < 2; k++) {
        const int i = (k == 0) ? first : 1 - first;
        if (!S.pend[i].has) continue;
        S.pend[i].has = 0;
        const reject_t why = battle_validate(side(i), side(1 - i), S.pend[i].kind,
                                             S.pend[i].r, S.pend[i].c, S.pend[i].roll);
        if (why != REJ_NONE) { reject_shot(i, S.tick, why); continue; }
        execute_shot(i, S.pend[i].kind, S.pend[i].r, S.pend[i].c, S.pend[i].roll, 1);
    }
    S.ticks_completed = S.tick + 1;
    check_ceasefire();
    if (both_inactive()) {
        log_printf("\n[STOP] Обе стороны прекратили огонь на такте %d\n", S.tick);
        finish_match(STOP_NATURAL);
        return;
    }
    if (S.config.max_ticks > 0 && S.ticks_completed >= S.config.max_ticks) {
        log_printf("\n[STOP] Лимит тактов: %d\n", S.config.max_ticks);
        finish_match(STOP_LIMIT);
        return;
    }
    begin_tick(S.tick + 1);
}

static void start_match(void) {
    S.started_ms = now_ms();
    notice("[СТАРТ] Все участники подключены (наблюдателей: %d). Начинаем моделирование.",
           count_observers());
    for (int r = 0; r < ROLE_PARTICIPANTS; r++) send_role(r, "START");
    begin_tick(0);
}

// ---------------------------------------------------------------- обработка сообщений

static void reject_shot(int i, int tick, reject_t why) {
    S.rejected[i]++;
    log_printf("[ВЫСТРЕЛ ОТКЛОНЁН] %s, такт %d: %s\n", side(i)->name, tick, reject_text(why));
    send_role(ROLE_GUN_A + i, "RESULT %d %s", tick, reject_token(why));
}

static void handle_shot(int i, long tick, char kind, long r, long c, long roll) {
    if (S.phase != PH_FIRE || tick != S.tick) {
        reject_shot(i, (int)tick, tick < S.tick || S.phase == PH_SIDES ? REJ_LATE : REJ_BADSTATE);
        return;
    }
    if (!side(i)->active) { reject_shot(i, S.tick, REJ_CEASEFIRE); return; }
    if (S.gun_done[i] && !S.pend[i].has) { reject_shot(i, S.tick, REJ_DUP); return; }
    if (S.pend[i].has) { reject_shot(i, S.tick, REJ_DUP); return; }
    const reject_t why = battle_validate(side(i), side(1 - i), kind, (int)r, (int)c, (int)roll);
    if (why != REJ_NONE) { reject_shot(i, S.tick, why); return; }
    if (kind == 'H') {
        // Расписание тяжёлых залпов контролирует сервер: залп допустим, если пришёл срок
        // (area_every) либо обычными снарядами стрелять нельзя — как в IDZ3.
        const artillery_t *a = &side(i)->art;
        const int due = a->area_every > 0 && S.tick >= S.next_area_tick[i];
        if (!due && artillery_can_fire(a)) { reject_shot(i, S.tick, REJ_NOTDUE); return; }
    }

    S.gun_done[i] = 1;
    if (is_lockstep()) {
        // Одновременный залп: решение откладывается до конца такта, порядок A, B —
        // поэтому результат не зависит от того, чей пакет пришёл раньше.
        S.pend[i] = (pending_t){ 1, kind, (int)r, (int)c, (int)roll };
    } else {
        execute_shot(i, kind, (int)r, (int)c, (int)roll, 1);
    }
}

static void handle_resupply(int i, long tick, long amount) {
    side_t *s = side(i);
    const artillery_t *a = &s->art;
    // Как и в IDZ3, пополнение идёт по расписанию и у стороны, прекратившей огонь:
    // пока продолжается моделирование, запас растёт (влияет на итоговый остаток).
    const int ok = S.phase == PH_SIDES && tick == S.tick &&
                   a->replenish_every > 0 && amount == a->replenish_amount &&
                   (tick + 1) % a->replenish_every == 0 && tick > S.last_resupply[i];
    if (!ok) {
        log_printf("[ПОПОЛНЕНИЕ ОТКЛОНЕНО] %s, такт %ld, +%ld\n", s->name, tick, amount);
        send_role(ROLE_SIDE_A + i, "TXT пополнение отклонено сервером (такт %ld)", tick);
        return;
    }
    const int added = battle_replenish(s);
    S.last_resupply[i] = (int)tick;
    log_printf("[%s] Пополнение: +%d снарядов, всего: %" PRId64 "\n", s->name, added, s->art.ammo_left);
    send_role(ROLE_SIDE_A + i, "SUPPLY %d %d %" PRId64, S.tick, added, s->art.ammo_left);
    send_role(ROLE_GUN_A + i, "SUPPLY %d %d %" PRId64, S.tick, added, s->art.ammo_left);
}

static void handle_hello(int idx, char **argv, int argc) {
    client_t *c = &S.cl[idx];
    if (c->role != ROLE_NONE) { conn_send(&c->conn, "ERR повторный HELLO"); c->conn.dead = 1; return; }
    const int role = argc >= 2 ? role_parse(argv[1]) : ROLE_NONE;
    if (role == ROLE_NONE) {
        conn_send(&c->conn, "ERR неизвестная роль (допустимо: side-A side-B gun-A gun-B observer)");
        c->conn.dead = 1;
        return;
    }
    if (role < ROLE_PARTICIPANTS && S.slot[role] >= 0) {
        log_printf("[СЕТЬ] Отказ: роль %s уже занята (попытка с %s)\n", role_name(role), c->peer);
        conn_send(&c->conn, "ERR роль %s уже занята", role_name(role));
        c->conn.dead = 1;
        return;
    }
    c->role = role;
    if (role < ROLE_PARTICIPANTS) S.slot[role] = idx;
    log_printf("[СЕТЬ] Подключён %s (%s)\n", role_name(role), c->peer);
    send_welcome(c);
    if (role == ROLE_OBSERVER) return;

    if (S.phase == PH_LOBBY) {
        lobby_status();
    } else if (S.phase == PH_SIDES || S.phase == PH_FIRE) {
        notice("[ПЕРЕПОДКЛЮЧЕНИЕ] Участник %s вернулся в игру (такт %d)", role_name(role), S.tick);
        S.side_wait_since = S.fire_started = now_ms();   // даём вернувшемуся время ответить
        send_prompt(role);
    }
}

static void handle_line(int idx, const char *line) {
    client_t *c = &S.cl[idx];
    c->last_rx_ms = now_ms();                              // любая строка доказывает, что клиент жив
    char tmp[PROTO_MAX_LINE];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char *argv[16];
    const int argc = util_split(tmp, argv, 16);
    if (argc == 0) return;
    const char *cmd = argv[0];

    if (strcmp(cmd, "HELLO") == 0) { handle_hello(idx, argv, argc); return; }
    if (c->role == ROLE_NONE) { conn_send(&c->conn, "ERR ожидается HELLO"); c->conn.dead = 1; return; }
    if (strcmp(cmd, "BYE") == 0) { c->conn.eof = 1; return; }
    if (strcmp(cmd, "PONG") == 0) return;                  // ответ на PING: «жив» уже отмечено ниже

    long v[8];
    if (strcmp(cmd, "ACK") == 0 && role_is_side(c->role)) {
        if (argc == 2 && util_parse_long(argv[1], 0, 1000000000, &v[0]) == 0) {
            if (S.phase == PH_SIDES && v[0] == S.tick) S.side_acked[role_side_index(c->role)] = 1;
            return;
        }
    } else if (strcmp(cmd, "RESUPPLY") == 0 && role_is_side(c->role)) {
        if (argc == 3 && util_parse_long(argv[1], 0, 1000000000, &v[0]) == 0 &&
            util_parse_long(argv[2], 0, 1000000000, &v[1]) == 0) {
            handle_resupply(role_side_index(c->role), v[0], v[1]);
            return;
        }
    } else if (strcmp(cmd, "SHOT") == 0 && role_is_gun(c->role)) {
        if (argc == 8 && (strcmp(argv[2], "R") == 0 || strcmp(argv[2], "H") == 0) &&
            util_parse_long(argv[1], 0, 1000000000, &v[0]) == 0 &&
            util_parse_long(argv[3], -1000000, 1000000, &v[1]) == 0 &&
            util_parse_long(argv[4], -1000000, 1000000, &v[2]) == 0 &&
            util_parse_long(argv[5], -1000000, 1000000, &v[3]) == 0 &&
            util_parse_long(argv[6], 0, 4294967295L, &v[4]) == 0 &&
            util_parse_long(argv[7], 0, 2000000000L, &v[5]) == 0) {
            const int gi = role_side_index(c->role);
            // Состояние запоминается при любом синтаксически верном SHOT (даже отклонённом):
            // подразделение уже «израсходовало» эти случайные числа.
            S.gun_rng[gi] = (unsigned)v[4];
            S.gun_sector[gi] = (int)v[5];
            S.gun_has_state[gi] = 1;
            handle_shot(gi, v[0], argv[2][0], v[1], v[2], v[3]);
            return;
        }
        // Синтаксически неверный выстрел: отвечаем отказом, ничего не расходуем.
        reject_shot(role_side_index(c->role), S.tick, REJ_BADARGS);
        return;
    } else if (strcmp(cmd, "PASS") == 0 && role_is_gun(c->role)) {
        if (argc == 2 && util_parse_long(argv[1], 0, 1000000000, &v[0]) == 0) {
            if (S.phase == PH_FIRE && v[0] == S.tick) S.gun_done[role_side_index(c->role)] = 1;
            return;
        }
    }

    // Неизвестная или недопустимая для роли команда. Клиент, который шлёт мусор
    // слишком часто, отключается — сервер остаётся в рабочем состоянии.
    c->bad++;
    log_printf("[СЕТЬ] %s (%s): недопустимое сообщение «%.60s» (%d)\n",
               role_name(c->role), c->peer, line, c->bad);
    conn_send(&c->conn, "ERR недопустимое сообщение");
    if (c->bad >= 20) c->conn.dead = 1;
}

// ---------------------------------------------------------------- завершение

static void print_verdict(sbuf_t *sb, const side_t *a, const side_t *b) {
    sb_printf(sb, "\n--- Вердикт ---\nПотери: %s — %" PRId64 ", %s — %" PRId64 "\n",
              a->name, a->lost_cost, b->name, b->lost_cost);
    if (a->grid.alive_targets > 0 && b->grid.alive_targets <= 0)
        sb_printf(sb, "Победа: %s (у противника уничтожены все цели)\n", a->name);
    else if (b->grid.alive_targets > 0 && a->grid.alive_targets <= 0)
        sb_printf(sb, "Победа: %s (у противника уничтожены все цели)\n", b->name);
    else if (a->lost_cost != b->lost_cost)
        sb_printf(sb, "Преимущество: %s (потери меньше на %" PRId64 ")\n",
                  a->lost_cost < b->lost_cost ? a->name : b->name,
                  a->lost_cost > b->lost_cost ? a->lost_cost - b->lost_cost : b->lost_cost - a->lost_cost);
    else
        sb_printf(sb, "Ничья по стоимости потерь\n");
}

static void finish_match(stop_reason_t reason) {
    if (S.phase == PH_DONE) return;
    S.phase = PH_DONE;
    S.stop_reason = reason;
    if (S.ticks_completed < S.tick && reason != STOP_NATURAL && reason != STOP_LIMIT)
        S.ticks_completed = S.tick;

    sbuf_t rep;
    sb_init(&rep);
    sb_printf(&rep, "\n========== ИТОГИ IDZ4 ==========\nВсего тактов: %d\nПричина: %s\n",
              S.ticks_completed, stop_reason_text(reason));
    side_render_stats(&S.config.A, &rep);
    side_render_stats(&S.config.B, &rep);
    sb_printf(&rep, "Отклонено выстрелов: %s — %d, %s — %d\n", S.config.A.name, S.rejected[0],
              S.config.B.name, S.rejected[1]);
    print_verdict(&rep, &S.config.A, &S.config.B);
    sb_printf(&rep, "\n--- Финальное состояние ---\n");
    render_maps(&rep);
    sb_printf(&rep, "\n========== ЗАВЕРШЕНИЕ ==========\n");
    log_printf("%s", sb_str(&rep));     // консоль сервера, журнал, наблюдатели (через приёмник)
    sb_free(&rep);

    // Участникам — их собственные итоги.
    for (int i = 0; i < 2; i++) {
        sbuf_t mine;
        sb_init(&mine);
        side_render_stats(side(i), &mine);
        client_t *cs = client_of_role(ROLE_SIDE_A + i);
        if (cs) send_text_lines(&cs->conn, "SUMMARY", sb_str(&mine), mine.len);
        client_t *cg = client_of_role(ROLE_GUN_A + i);
        if (cg) send_text_lines(&cg->conn, "SUMMARY", sb_str(&mine), mine.len);
        sb_free(&mine);
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (S.cl[i].used && S.cl[i].role != ROLE_NONE)
            conn_send(&S.cl[i].conn, "END %d %d", (int)reason, S.ticks_completed);
}

// Дать клиентам время получить END, затем закрыть все соединения.
static void drain_and_close(void) {
    const long deadline = now_ms() + 3000;
    for (;;) {
        struct pollfd p[MAX_CLIENTS];
        int n = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!S.cl[i].used) continue;
            conn_flush(&S.cl[i].conn);
            if (S.cl[i].conn.dead || !conn_has_output(&S.cl[i].conn)) continue;
            p[n].fd = S.cl[i].conn.fd; p[n].events = POLLOUT; p[n].revents = 0; n++;
        }
        const long left = deadline - now_ms();
        if (n == 0 || left <= 0) break;
        (void)poll(p, (nfds_t)n, (int)(left > 200 ? 200 : left));
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!S.cl[i].used) continue;
        if (S.cl[i].conn.fd >= 0) shutdown(S.cl[i].conn.fd, SHUT_WR);
        conn_close(&S.cl[i].conn);
        S.cl[i].used = 0;
    }
}

// ---------------------------------------------------------------- главный цикл

// Продвинуть конечный автомат. Возвращает 1, если состояние изменилось.
static int step(long now) {
    if (S.phase == PH_DONE) return 0;

    if (S.phase == PH_LOBBY) {
        if (count_participants() == ROLE_PARTICIPANTS && count_observers() >= S.opt_min_obs) {
            start_match();
            return 1;
        }
        return 0;
    }

    if (S.paused) {
        if (count_participants() == ROLE_PARTICIPANTS) {
            S.paused = 0;
            S.side_wait_since = S.fire_started = now;
            notice("[ВОЗОБНОВЛЕНИЕ] Все участники на месте. Моделирование продолжается с такта %d", S.tick);
            for (int r = 0; r < ROLE_PARTICIPANTS; r++) send_role(r, "RESUME");
            return 1;
        }
        if (now - S.pause_since > S.opt_grace_ms) {
            log_printf("[ПАУЗА] Участник не вернулся за %d с — завершаем моделирование.\n",
                       S.opt_grace_ms / 1000);
            finish_match(STOP_ABSENT);
            return 1;
        }
        return 0;
    }

    if (S.phase == PH_SIDES) {
        if (S.side_acked[0] && S.side_acked[1]) { begin_fire(); return 1; }
        if (now - S.side_wait_since > S.opt_reply_ms) {
            for (int i = 0; i < 2; i++) {
                if (S.side_acked[i] || S.slot[ROLE_SIDE_A + i] < 0) continue;
                log_printf("[СЕТЬ] %s не подтвердил такт %d за %d мс\n",
                           role_name(ROLE_SIDE_A + i), S.tick, S.opt_reply_ms);
                remove_client(S.slot[ROLE_SIDE_A + i], "нет ответа на TICK");
                return 1;
            }
        }
        return 0;
    }

    if (S.phase == PH_FIRE) {
        if (is_lockstep()) {
            if (S.gun_done[0] && S.gun_done[1]) { end_tick(); return 1; }
            if (now - S.fire_started > S.opt_reply_ms) {
                for (int i = 0; i < 2; i++) {
                    if (S.gun_done[i] || S.slot[ROLE_GUN_A + i] < 0) continue;
                    log_printf("[СЕТЬ] %s не ответил на такт %d за %d мс\n",
                               role_name(ROLE_GUN_A + i), S.tick, S.opt_reply_ms);
                    remove_client(S.slot[ROLE_GUN_A + i], "нет ответа на GUNTICK");
                    return 1;
                }
            }
        } else if (now - S.fire_started >= S.config.tick_ms) {
            end_tick();
            return 1;
        }
    }
    return 0;
}

static int poll_timeout(long now) {
    long t = 100;
    if (S.phase == PH_FIRE && !S.paused && !is_lockstep()) {
        const long left = S.fire_started + S.config.tick_ms - now;
        if (left < t) t = left;
    }
    return t < 0 ? 0 : (int)t;
}

static void accept_clients(void) {
    for (;;) {
        char peer[64];
        const int fd = net_accept(S.listen_fd, peer, sizeof(peer));
        if (fd < 0) return;
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) if (!S.cl[i].used) { idx = i; break; }
        if (idx < 0) {
            const char msg[] = "ERR сервер переполнен\n";
            (void)send(fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        client_t *c = &S.cl[idx];
        memset(c, 0, sizeof(*c));
        conn_init(&c->conn, fd, WMAX_BYTES);
        c->used = 1;
        c->role = ROLE_NONE;
        c->since_ms = c->last_rx_ms = now_ms();
        snprintf(c->peer, sizeof(c->peer), "%s", peer);
    }
}

// Интервал между PING: в 4 раза короче таймаута, чтобы здоровый клиент успевал ответить.
static long ping_interval(void) {
    long v = S.opt_reply_ms / 4;
    return v < 100 ? 100 : v > 1000 ? 1000 : v;
}

static void serve(void) {
    struct pollfd pfds[MAX_CLIENTS + 1];
    int pidx[MAX_CLIENTS + 1];

    while (S.phase != PH_DONE) {
        if (signals_stop_requested()) {
            log_printf("\n[STOP] Сигнал %d: остановка оператором\n", signals_last_signal());
            finish_match(STOP_SIGNAL);
            break;
        }
        const long now = now_ms();
        int n = 0;
        pfds[n].fd = S.listen_fd; pfds[n].events = POLLIN; pfds[n].revents = 0; pidx[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!S.cl[i].used) continue;
            pfds[n].fd = S.cl[i].conn.fd;
            pfds[n].events = (short)(POLLIN | (conn_has_output(&S.cl[i].conn) ? POLLOUT : 0));
            pfds[n].revents = 0;
            pidx[n++] = i;
        }
        const int rc = poll(pfds, (nfds_t)n, poll_timeout(now));
        if (rc < 0 && errno != EINTR) { log_printf("[ERROR] poll: %s\n", strerror(errno)); finish_match(STOP_ERROR); break; }

        if (rc > 0) {
            if (pfds[0].revents & POLLIN) accept_clients();
            for (int k = 1; k < n; k++) {
                const int i = pidx[k];
                client_t *c = &S.cl[i];
                if (!c->used) continue;
                if (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)) conn_fill(&c->conn);
                char line[PROTO_MAX_LINE];
                int lr;
                while (c->used && !c->conn.dead && (lr = conn_getline(&c->conn, line, sizeof(line))) != 0) {
                    if (lr < 0) { c->conn.dead = 1; break; }
                    handle_line(i, line);
                }
            }
        }
        // Проверка «живости»: каждому опознанному клиенту периодически шлём PING; тот, кто
        // дольше reply-timeout не прислал вообще ничего (завис, оборвалась сеть без FIN),
        // отключается. Работает в любом режиме, в том числе при tick_ms > 0, когда сервер
        // не ждёт ответов на такт.
        {
            const long t = now_ms();
            const long interval = ping_interval();
            if (t - S.last_ping_ms >= interval) {
                S.last_ping_ms = t;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (S.cl[i].used && S.cl[i].role != ROLE_NONE) conn_send(&S.cl[i].conn, "PING");
            }
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!S.cl[i].used || S.cl[i].role == ROLE_NONE) continue;
                if (t - S.cl[i].last_rx_ms > S.opt_reply_ms) {
                    log_printf("[СЕТЬ] %s не отвечает на PING более %d мс\n", role_name(S.cl[i].role), S.opt_reply_ms);
                    remove_client(i, "нет ответа на PING");
                }
            }
        }
        // Отправка накопленного и удаление «мёртвых» соединений.
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &S.cl[i];
            if (!c->used) continue;
            conn_flush(&c->conn);
            if (c->conn.dead) remove_client(i, "ошибка соединения");
            else if (c->conn.eof && !conn_has_output(&c->conn)) remove_client(i, "клиент закрыл соединение");
            else if (c->role == ROLE_NONE && now_ms() - c->since_ms > HELLO_TIMEOUT_MS)
                remove_client(i, "нет HELLO");
        }
        for (int guard = 0; guard < 8 && step(now_ms()); guard++) { }
    }
}

// ---------------------------------------------------------------- запуск

static void usage(const char *prog) {
    log_console(
        "Использование: %s [ключи] [файл.conf]\n"
        "  -l, --listen IP        адрес прослушивания (по умолчанию 0.0.0.0)\n"
        "  -p, --port N           TCP-порт (по умолчанию 5000; 0 — выбрать свободный)\n"
        "      --port-file FILE   записать выбранный порт в файл (удобно для скриптов)\n"
        "  -c, --config FILE      конфигурация моделирования (можно и позиционным аргументом)\n"
        "  -i, --interactive      ввести параметры с клавиатуры\n"
        "      --log FILE         журнал (переопределяет log= из конфигурации)\n"
        "      --grace SEC        сколько ждать вернувшегося участника, с (по умолчанию 60)\n"
        "      --reply-timeout MS сколько клиент может молчать (нет ответа на PING/такт), мс (5000)\n"
        "      --history N        сколько последних строк журнала повторять позднему наблюдателю (1000)\n"
        "      --min-observers N  не начинать бой, пока не подключатся N наблюдателей\n"
        "      --quiet            не печатать на консоль (только журнал)\n"
        "  -h, --help             справка\n", prog);
}

static int write_port_file(const char *path, int port) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "%d\n", port);
    const ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return w == n ? 0 : -1;
}

int main(int argc, char **argv) {
    // Сервер не вызывает colors_init(): тексты, уходящие по сети, должны быть без ANSI.
    memset(&S, 0, sizeof(S));
    for (int r = 0; r < ROLE_PARTICIPANTS; r++) S.slot[r] = -1;
    S.opt_grace_ms = 60000;
    S.opt_reply_ms = 5000;
    sb_init(&S.banner);

    const char *host = "0.0.0.0", *conf_path = NULL, *log_override = NULL, *port_file = NULL;
    long port = 5000, num;
    int interactive = 0, quiet = 0;
    S.opt_history = 1000;

    static const struct option longopts[] = {
        {"listen", required_argument, 0, 'l'}, {"port", required_argument, 0, 'p'},
        {"port-file", required_argument, 0, 1000}, {"config", required_argument, 0, 'c'},
        {"interactive", no_argument, 0, 'i'}, {"log", required_argument, 0, 1001},
        {"grace", required_argument, 0, 1002}, {"reply-timeout", required_argument, 0, 1003},
        {"min-observers", required_argument, 0, 1004}, {"quiet", no_argument, 0, 1005},
        {"history", required_argument, 0, 1006},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "l:p:c:ih", longopts, NULL)) != -1) {
        switch (opt) {
            case 'l': host = optarg; break;
            case 'p':
                if (util_parse_long(optarg, 0, 65535, &port) < 0) { fprintf(stderr, "Неверный порт: %s\n", optarg); return 1; }
                break;
            case 'c': conf_path = optarg; break;
            case 'i': interactive = 1; break;
            case 1000: port_file = optarg; break;
            case 1001: log_override = optarg; break;
            case 1002:
                if (util_parse_long(optarg, 0, 86400, &num) < 0) { fprintf(stderr, "Неверное --grace\n"); return 1; }
                S.opt_grace_ms = (int)(num * 1000);
                break;
            case 1003:
                if (util_parse_long(optarg, 50, 600000, &num) < 0) { fprintf(stderr, "Неверное --reply-timeout\n"); return 1; }
                S.opt_reply_ms = (int)num;
                break;
            case 1004:
                if (util_parse_long(optarg, 0, 60, &num) < 0) { fprintf(stderr, "Неверное --min-observers\n"); return 1; }
                S.opt_min_obs = (int)num;
                break;
            case 1005: quiet = 1; break;
            case 1006:
                if (util_parse_long(optarg, 0, 1000000, &num) < 0) { fprintf(stderr, "Неверное --history\n"); return 1; }
                S.opt_history = (int)num;
                break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }
    if (optind < argc) {
        if (conf_path || optind + 1 < argc) { fprintf(stderr, "Лишние аргументы\n"); usage(argv[0]); return 1; }
        conf_path = argv[optind];
    }
    if (signals_install() < 0) { fprintf(stderr, "Не удалось установить обработчики сигналов\n"); return 1; }
    if (quiet) log_set_console(0);

    if (config_defaults(&S.config) < 0) { fprintf(stderr, "Ошибка инициализации конфигурации\n"); return 1; }
    if (conf_path && config_load(&S.config, conf_path) < 0) {
        fprintf(stderr, "Ошибка в конфигурационном файле: %s\n", conf_path);
        config_free(&S.config);
        return 1;
    }
    if (log_override) snprintf(S.config.log_path, sizeof(S.config.log_path), "%s", log_override);
    if (log_open(S.config.log_path) < 0) {
        fprintf(stderr, "Не удалось открыть лог: %s\n", S.config.log_path);
        config_free(&S.config);
        return 1;
    }
    if (interactive && config_read_stdin(&S.config) < 0) {
        log_printf("\n[STOP] Интерактивный ввод прерван\n");
        log_close(); config_free(&S.config);
        return signals_stop_requested() ? 0 : 1;
    }

    S.gun_seed[0] = S.config.seed ^ 0x9E3779B9u;      // как в IDZ3: у каждого подразделения свой seed
    S.gun_seed[1] = S.config.seed ^ 0x85EBCA6Bu;
    S.next_area_tick[0] = S.config.A.art.area_every;
    S.next_area_tick[1] = S.config.B.art.area_every;
    S.last_resupply[0] = S.last_resupply[1] = -1;
    S.tick = -1;
    if (S.opt_history > 0) S.hist = (char **)calloc((size_t)S.opt_history, sizeof(char *));

    int bound = (int)port;
    S.listen_fd = net_listen(host, (int)port, &bound);
    if (S.listen_fd < 0) {
        fprintf(stderr, "Не удалось начать прослушивание %s:%ld: %s\n", host, port, strerror(errno));
        log_close(); config_free(&S.config);
        return 1;
    }
    if (port_file && write_port_file(port_file, bound) < 0) {
        fprintf(stderr, "Не удалось записать порт в %s\n", port_file);
        close(S.listen_fd); log_close(); config_free(&S.config);
        return 1;
    }

    // Заголовок журнала собираем для наблюдателей, которые подключатся позже.
    S.capturing = 1;
    log_set_sink(obs_sink, NULL);
    log_printf("\n========== СТАРТ IDZ4 (TCP) ==========\n");
    log_printf("Сервер-координатор слушает %s:%d\n", host, bound);
    config_print(&S.config);
    if (S.config.max_ticks <= 0) log_printf("Режим: без ограничения тактов. Для остановки — Ctrl+C.\n");
    else log_printf("Режим: лимит тактов = %d.\n", S.config.max_ticks);
    log_printf("\n--- Начальное состояние ---\n");
    {
        sbuf_t maps;
        sb_init(&maps);
        render_maps(&maps);
        log_printf("%s", sb_str(&maps));
        sb_free(&maps);
    }
    S.capturing = 0;
    log_printf("\n[ЛОББИ] Ожидание участников: side-A side-B gun-A gun-B (наблюдатели — по желанию)\n");

    serve();
    drain_and_close();

    log_printf("\n[MAIN] Сервер завершён. Тактов: %d.\n", S.ticks_completed);
    log_set_sink(NULL, NULL);
    close(S.listen_fd);
    log_close();
    for (int k = 0; k < S.hist_count; k++) free(S.hist[(S.hist_head + k) % S.opt_history]);
    free(S.hist);
    sb_free(&S.banner);
    config_free(&S.config);
    return (S.stop_reason == STOP_ERROR || S.stop_reason == STOP_ABSENT) ? 1 : 0;
}
