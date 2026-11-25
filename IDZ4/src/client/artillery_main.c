// artillery_main.c — клиент «огневое подразделение».
//
// Самостоятельный участник модели: сам выбирает цель по своей стратегии
// (случайно / к центру / по секторам), сам «бросает кубик» точности и сам
// выдерживает интервал между выстрелами. Сервер лишь регистрирует выстрелы и
// сообщает итог. Подразделение не знает ни целей противника, ни состояния
// его территории — только размеры поля и то, что приходит в ответах.
#define _POSIX_C_SOURCE 200809L
#include "artillery.h"
#include "client_common.h"
#include "colors.h"
#include "log.h"
#include "proto.h"
#include "signals.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *side_arg = NULL;   // значение ключа -r/--side

static int extra(int opt, const char *arg) {
    if (opt != 'r') return -1;
    side_arg = arg;
    return 0;
}

typedef struct {
    int  side;                       // 0 — A, 1 — B
    char self_name[32], enemy_name[32];
    int  tick_ms, enemy_rows, enemy_cols;
    unsigned seed;
    long shots_fired;                // с сервера: сколько выстрелов уже зарегистрировано
    int  resume;                     // 1 — сервер прислал точное состояние ГПСЧ и сектора
    unsigned resume_rng;
    int  resume_sector;
    artillery_t brain;               // здесь живут только стратегия, ГПСЧ и сектор
    char strategy[16];               // random | center | sector (с сервера)
    int  reaction_ms, jitter_ms, accuracy;
    int  ready, ceased;

    // Запланированный выстрел (режим tick_ms > 0: подразделение «целится» заранее).
    int  pending;
    int  p_tick;
    char p_kind;
    long p_due;
    long next_action;                // не стрелять раньше этого момента (перезарядка)
} gun_t;

static void init_brain(gun_t *g) {
    artillery_init(&g->brain);
    g->brain.strategy = !strcmp(g->strategy, "random") ? STRAT_RANDOM
                      : !strcmp(g->strategy, "center") ? STRAT_CENTER : STRAT_SECTOR;
    artillery_seed(&g->brain, g->seed);
    if (g->resume) {
        // Переподключение: сервер хранит состояние, присланное в последнем SHOT, поэтому
        // подразделение продолжает ровно ту же последовательность, что и без обрыва.
        g->brain.rng_state = g->resume_rng;
        g->brain.sector_pos = g->resume_sector;
    }
}

static void fire_now(client_app_t *app, gun_t *g) {
    int r, c;
    artillery_pick_coords(&g->brain, g->enemy_rows, g->enemy_cols, &r, &c);
    // Как и в IDZ3, бросок точности делается только при accuracy < 100: так при
    // одинаковом seed последовательность выстрелов совпадает с оригиналом.
    const int roll = g->accuracy < 100 ? artillery_random_bounded(&g->brain, 100) : 0;
    log_printf("%s[АРТИЛЛЕРИЯ %s]%s такт %d: %s по (%d,%d), бросок точности %d\n", C_CYAN, g->self_name, C_RESET,
               g->p_tick, g->p_kind == 'H' ? "ТЯЖЁЛЫЙ ЗАЛП" : "выстрел", r, c, roll);
    // Интервал до следующего выстрела разыгрывается ДО отправки: тогда состояние ГПСЧ в
    // сообщении учитывает все использованные случайные числа этого выстрела.
    if (g->tick_ms > 0) {
        int delay = g->reaction_ms;
        if (g->jitter_ms > 0) delay += artillery_random_bounded(&g->brain, g->jitter_ms + 1);
        g->next_action = now_ms() + delay;
    }
    client_send(app, "SHOT %d %c %d %d %d %u %d", g->p_tick, g->p_kind, r, c, roll,
                g->brain.rng_state, g->brain.sector_pos);
    g->pending = 0;
}

static int on_line(client_app_t *app, const char *cmd, const char *rest) {
    gun_t *g = (gun_t *)app->ctx;
    char tmp[PROTO_MAX_LINE];
    char *av[16];
    snprintf(tmp, sizeof(tmp), "%s", rest);
    const int n = util_split(tmp, av, 16);

    if (!strcmp(cmd, "WELCOME")) return 0;
    if (!strcmp(cmd, "CFG") && n >= 2) {
        if (!strcmp(av[0], "self_name")) snprintf(g->self_name, sizeof(g->self_name), "%s", rest + 10);
        else if (!strcmp(av[0], "enemy_name")) snprintf(g->enemy_name, sizeof(g->enemy_name), "%s", rest + 11);
        else if (!strcmp(av[0], "tick_ms")) g->tick_ms = atoi(av[1]);
        else if (!strcmp(av[0], "enemy_rows")) g->enemy_rows = atoi(av[1]);
        else if (!strcmp(av[0], "enemy_cols")) g->enemy_cols = atoi(av[1]);
        else if (!strcmp(av[0], "gun_seed")) g->seed = (unsigned)strtoul(av[1], NULL, 10);
        else if (!strcmp(av[0], "reaction_ms")) g->reaction_ms = atoi(av[1]);
        else if (!strcmp(av[0], "jitter_ms")) g->jitter_ms = atoi(av[1]);
        else if (!strcmp(av[0], "accuracy")) g->accuracy = atoi(av[1]);
        else if (!strcmp(av[0], "strategy")) snprintf(g->strategy, sizeof(g->strategy), "%s", av[1]);
        return 0;
    }
    if (!strcmp(cmd, "STATE") && n >= 2) {
        if (!strcmp(av[0], "shots_fired")) g->shots_fired = atol(av[1]);
        if (!strcmp(av[0], "rng_state")) { g->resume_rng = (unsigned)strtoul(av[1], NULL, 10); g->resume |= 1; }
        if (!strcmp(av[0], "sector_pos")) { g->resume_sector = atoi(av[1]); g->resume |= 2; }
        if (!strcmp(av[0], "active")) g->ceased = atoi(av[1]) == 0;
        return 0;
    }
    if (!strcmp(cmd, "WELCOME_END")) {
        g->resume = (g->resume == 3);                 // нужны оба значения
        init_brain(g);
        g->ready = 1;
        log_printf("[АРТИЛЛЕРИЯ %s] Готова к бою: цель — территория %s (%dx%d), стратегия %s, "
                   "уже сделано выстрелов: %ld%s\n", g->self_name, g->enemy_name, g->enemy_rows,
                   g->enemy_cols, g->strategy, g->shots_fired,
                   g->resume ? " (состояние генератора восстановлено с сервера)" : "");
        return 0;
    }
    if (!strcmp(cmd, "GUNTICK") && n >= 8 && g->ready) {
        const int tick = atoi(av[0]), reg = atoi(av[1]), area = atoi(av[2]), due = atoi(av[3]);
        const long ammo = atol(av[4]); const int area_ammo = atoi(av[5]);
        g->pending = 0;                                // новый такт отменяет несостоявшийся выстрел
        if (g->ceased) return 0;
        if (!reg && !area) {
            log_printf("[АРТИЛЛЕРИЯ %s] такт %d: стрелять нечем (обычных %ld, тяжёлых %d) — ожидаю\n",
                       g->self_name, tick, ammo, area_ammo);
            client_send(app, "PASS %d", tick);
            return 0;
        }
        g->p_tick = tick;
        g->p_kind = (due && area) ? 'H' : reg ? 'R' : 'H';
        const long now = now_ms();
        g->p_due = now > g->next_action ? now : g->next_action;
        if (g->tick_ms > 0 && g->p_due - now >= g->tick_ms) {
            // Подразделение ещё перезаряжается и в этот такт не успеет.
            log_printf("[АРТИЛЛЕРИЯ %s] такт %d: перезарядка, пропускаю такт\n", g->self_name, tick);
            return 0;
        }
        g->pending = 1;
        if (g->tick_ms <= 0 || g->p_due <= now) fire_now(app, g);
        return 0;
    }
    if (!strcmp(cmd, "RESULT") && n >= 2) {
        const int tick = atoi(av[0]);
        if (strcmp(av[1], "ok") != 0) {
            log_printf("%s[АРТИЛЛЕРИЯ %s]%s такт %d: выстрел НЕ принят сервером — %s\n", C_YELLOW,
                       g->self_name, C_RESET, tick, reject_text(reject_parse(av[1])));
            return 0;
        }
        if (n < 12) return 0;
        const int out = outcome_parse(av[2]);
        const int destroyed = atoi(av[7]);
        const char *color = (out == OUT_HIT || out == OUT_AREA_HIT) ? C_GREEN : C_YELLOW;
        const char *what = out == OUT_MISS_ACCURACY ? "ПРОМАХ (не хватило точности)"
            : out == OUT_MISS_DESTROYED ? "ПРОМАХ (клетка уже разрушена)"
            : out == OUT_MISS_EMPTY ? "ПРОМАХ (пустая клетка)"
            : out == OUT_AREA_NONE ? "в зоне поражения целей нет"
            : "ПОПАДАНИЕ";
        log_printf("%s[АРТИЛЛЕРИЯ %s]%s   -> %s%s%s", C_CYAN, g->self_name, C_RESET, color, what, C_RESET);
        if (destroyed > 0) log_printf(": уничтожено целей %d на сумму %s", destroyed, av[8]);
        log_printf(". Осталось: обычных %s, тяжёлых %s; израсходовано %s\n", av[9], av[10], av[11]);
        return 0;
    }
    if (!strcmp(cmd, "SUPPLY") && n >= 3) {
        log_printf("[АРТИЛЛЕРИЯ %s] Пополнение боезапаса: +%s (обычных теперь %s)\n", g->self_name, av[1], av[2]);
        return 0;
    }
    if (!strcmp(cmd, "CEASEFIRE")) {
        g->ceased = 1;
        g->pending = 0;
        log_printf("%s[АРТИЛЛЕРИЯ %s] ПРЕКРАЩАЮ ОГОНЬ: %s%s\n", C_MAGENTA, g->self_name, rest, C_RESET);
        return 0;
    }
    if (!strcmp(cmd, "TXT") || !strcmp(cmd, "PAUSE") || !strcmp(cmd, "RESUME")) {
        log_printf("[СЕРВЕР] %s\n", rest);
        return 0;
    }
    if (!strcmp(cmd, "START")) { log_printf("[СЕРВЕР] Бой начался\n"); return 0; }
    if (!strcmp(cmd, "SUMMARY")) { log_printf("%s\n", rest); return 0; }
    if (!strcmp(cmd, "END")) {
        log_printf("[СЕРВЕР] Моделирование завершено: %s\n",
                   stop_reason_text(n >= 1 ? atoi(av[0]) : -1));
        return 1;
    }
    return 0;
}

static long timer_ms(client_app_t *app) {
    gun_t *g = (gun_t *)app->ctx;
    if (!g->pending) return -1;
    const long left = g->p_due - now_ms();
    return left < 0 ? 0 : left;
}

static void on_timer(client_app_t *app) {
    gun_t *g = (gun_t *)app->ctx;
    if (g->pending) fire_now(app, g);
}

int main(int argc, char **argv) {
    static gun_t g;
    static client_app_t app;
    int rc;

    static const struct option extra_long[] = { {"side", required_argument, 0, 'r'}, {0, 0, 0, 0} };

    colors_init();
    rc = client_parse_common(&app, argc, argv, "r:", extra, extra_long);
    if (rc != 0 || !side_arg || (strcmp(side_arg, "A") && strcmp(side_arg, "B"))) {
        fprintf(rc < 0 || !side_arg ? stderr : stdout,
                "Использование: %s -s IP -p PORT -r A|B [--retries N] [--retry-ms MS] [--log FILE] [--quiet]\n"
                "  Огневое подразделение стороны A или B.\n", argv[0]);
        return rc == 1 ? 0 : 1;
    }
    g.side = side_arg[0] == 'B';
    app.role = g.side ? "gun-B" : "gun-A";
    app.on_line = on_line;
    app.timer_ms = timer_ms;
    app.on_timer = on_timer;
    app.ctx = &g;
    return client_run(&app);
}
