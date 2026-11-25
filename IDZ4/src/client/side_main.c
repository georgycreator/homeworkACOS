// side_main.c — клиент «государство».
//
// Владеет своим представлением территории (карта своих целей, потери),
// самостоятельно инициирует пополнение запаса по правилам своей стороны и
// подтверждает серверу каждый такт. О чужой территории ничего не знает.
#define _POSIX_C_SOURCE 200809L
#include "client_common.h"
#include "colors.h"
#include "grid.h"
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
    int     side;                       // 0 — A, 1 — B
    char    self_name[32], enemy_name[32];
    int     rows, cols;
    int     replenish_every, replenish_amount;
    int     active;
    int     have_grid;
    grid_t  grid;                       // собственная территория
    int64_t lost_cost, initial_cost;
    int     initial_targets;
    int64_t ammo_left;
    int     last_resupply;
    int     alive_targets;              // агрегаты из STATE приходят до карты — запоминаем
    int64_t alive_cost;
} state_t;

// Карту печатаем только если она помещается на экран (как и у наблюдателей: не более 60x150).
// Огромные поля (до 1000x1000) вывод миллиона клеток замедлил бы настолько, что клиент
// перестал бы вовремя отвечать серверу.
static void print_own_map(state_t *s) {
    char title[96];
    snprintf(title, sizeof(title), "Территория %s", s->self_name);
    if (s->grid.rows > 60 || s->grid.cols > 150) {
        log_printf("%s (%dx%d): карта слишком велика для вывода; живых целей %d, стоимость %" PRId64 "\n",
                   title, s->grid.rows, s->grid.cols, s->grid.alive_targets, s->grid.alive_cost);
        return;
    }
    grid_print(&s->grid, title);
}

static int on_line(client_app_t *app, const char *cmd, const char *rest) {
    state_t *s = (state_t *)app->ctx;
    char tmp[PROTO_MAX_LINE];
    char *av[16];
    snprintf(tmp, sizeof(tmp), "%s", rest);
    const int n = util_split(tmp, av, 16);

    if (!strcmp(cmd, "WELCOME")) return 0;
    if (!strcmp(cmd, "CFG") && n >= 2) {
        if (!strcmp(av[0], "self_name")) snprintf(s->self_name, sizeof(s->self_name), "%s", rest + 10);
        else if (!strcmp(av[0], "enemy_name")) snprintf(s->enemy_name, sizeof(s->enemy_name), "%s", rest + 11);
        else if (!strcmp(av[0], "self_rows")) s->rows = atoi(av[1]);
        else if (!strcmp(av[0], "self_cols")) s->cols = atoi(av[1]);
        else if (!strcmp(av[0], "replenish_every")) s->replenish_every = atoi(av[1]);
        else if (!strcmp(av[0], "replenish_amount")) s->replenish_amount = atoi(av[1]);
        return 0;
    }
    if (!strcmp(cmd, "STATE") && n >= 2) {
        const int64_t v = strtoll(av[1], NULL, 10);
        if (!strcmp(av[0], "active")) s->active = (int)v;
        else if (!strcmp(av[0], "lost_cost")) s->lost_cost = v;
        else if (!strcmp(av[0], "initial_cost")) s->initial_cost = v;
        else if (!strcmp(av[0], "initial_targets")) s->initial_targets = (int)v;
        else if (!strcmp(av[0], "ammo_left")) s->ammo_left = v;
        else if (!strcmp(av[0], "last_resupply")) s->last_resupply = (int)v;
        else if (!strcmp(av[0], "alive_targets")) s->alive_targets = (int)v;
        else if (!strcmp(av[0], "alive_cost")) s->alive_cost = v;
        return 0;
    }
    if (!strcmp(cmd, "MAP") && n >= 3) {
        // MAP <A|B> <строка> <символы>; первая строка карты создаёт сетку.
        if (!s->have_grid) {
            if (s->rows <= 0 || s->cols <= 0 || grid_init(&s->grid, s->rows, s->cols) < 0) return 1;
            s->have_grid = 1;
        }
        const int r = atoi(av[1]);
        for (int c = 0; c < s->cols && av[2][c]; c++) {
            cell_t *cell = grid_at(&s->grid, r, c);
            if (cell) cell->state = av[2][c] == 'T' ? CELL_TARGET : av[2][c] == 'x' ? CELL_DESTROYED : CELL_EMPTY;
        }
        return 0;
    }
    if (!strcmp(cmd, "TGT") && n >= 5) {
        log_printf("[ГОСУДАРСТВО %s] своя цель (%s,%s): %s, стоимость %s\n", s->self_name, av[1], av[2], av[4], av[3]);
        return 0;
    }
    if (!strcmp(cmd, "WELCOME_END")) {
        if (!s->have_grid) { log_printf("[ГОСУДАРСТВО] Нет данных о территории\n"); return 1; }
        s->grid.alive_targets = s->alive_targets;      // агрегаты — после того как карта создана
        s->grid.alive_cost = s->alive_cost;
        log_printf("[ГОСУДАРСТВО %s] Готово к бою. Своих целей: %d (стоимость %" PRId64 "), потери: %" PRId64 "\n",
                   s->self_name, s->grid.alive_targets, s->grid.alive_cost, s->lost_cost);
        print_own_map(s);
        return 0;
    }
    if (!strcmp(cmd, "TICK") && n >= 1) {
        const int tick = atoi(av[0]);
        log_printf("[ГОСУДАРСТВО %s] такт %d: целей %d (стоимость %" PRId64 "), потери %" PRId64 "\n",
                   s->self_name, tick, s->grid.alive_targets, s->grid.alive_cost, s->lost_cost);
        // Пополнение по собственному расписанию: каждые N тактов.
        // Как и в IDZ3, пополнение продолжается и после прекращения огня, пока идёт бой.
        if (s->replenish_every > 0 && s->replenish_amount > 0 &&
            (tick + 1) % s->replenish_every == 0 && tick > s->last_resupply) {
            log_printf("[ГОСУДАРСТВО %s] запрашиваю пополнение запаса: +%d\n", s->self_name, s->replenish_amount);
            client_send(app, "RESUPPLY %d %d", tick, s->replenish_amount);
            s->last_resupply = tick;
        }
        client_send(app, "ACK %d", tick);
        return 0;
    }
    if (!strcmp(cmd, "DTGT") && n >= 4) {
        cell_t *cell = grid_at(&s->grid, atoi(av[0]), atoi(av[1]));
        if (cell) cell->state = CELL_DESTROYED;
        log_printf("%s[ГОСУДАРСТВО %s]%s   потеряна цель «%s» в (%s,%s), стоимость %s\n", C_RED, s->self_name,
                   C_RESET, av[3], av[0], av[1], av[2]);
        return 0;
    }
    if (!strcmp(cmd, "IMPACT") && n >= 10) {
        const int out = outcome_parse(av[4]);
        const int destroyed = atoi(av[5]);
        s->grid.alive_targets = atoi(av[7]);
        s->grid.alive_cost = strtoll(av[8], NULL, 10);
        s->lost_cost = strtoll(av[9], NULL, 10);
        log_printf("[ГОСУДАРСТВО %s] такт %s: по нашей территории %s в (%s,%s) — %s\n", s->self_name, av[0],
                   av[1][0] == 'H' ? "тяжёлый залп" : "выстрел", av[2], av[3],
                   destroyed > 0 ? "ЕСТЬ ПОТЕРИ" : (out == OUT_AREA_NONE ? "залп без последствий" : "мимо"));
        if (destroyed > 0) {
            log_printf("  Потеряно целей: %d на сумму %s. Осталось: %d (стоимость %" PRId64 "), всего потерь: %" PRId64 "\n",
                       destroyed, av[6], s->grid.alive_targets, s->grid.alive_cost, s->lost_cost);
            print_own_map(s);
        }
        return 0;
    }
    if (!strcmp(cmd, "SUPPLY") && n >= 3) {
        s->ammo_left = strtoll(av[2], NULL, 10);
        log_printf("[ГОСУДАРСТВО %s] Запас пополнен: +%s (обычных снарядов теперь %s)\n", s->self_name, av[1], av[2]);
        return 0;
    }
    if (!strcmp(cmd, "CEASEFIRE")) {
        s->active = 0;
        log_printf("%s[ГОСУДАРСТВО %s] Сторона прекращает огонь: %s%s\n", C_MAGENTA, s->self_name, rest, C_RESET);
        return 0;
    }
    if (!strcmp(cmd, "TXT") || !strcmp(cmd, "PAUSE") || !strcmp(cmd, "RESUME")) {
        log_printf("[СЕРВЕР] %s\n", rest);
        return 0;
    }
    if (!strcmp(cmd, "START")) { log_printf("[СЕРВЕР] Бой начался\n"); return 0; }
    if (!strcmp(cmd, "SUMMARY")) { log_printf("%s\n", rest); return 0; }
    if (!strcmp(cmd, "END")) {
        log_printf("[СЕРВЕР] Моделирование завершено: %s\n", stop_reason_text(n >= 1 ? atoi(av[0]) : -1));
        if (s->have_grid) print_own_map(s);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static state_t st;
    static client_app_t app;
    static const struct option extra_long[] = { {"side", required_argument, 0, 'r'}, {0, 0, 0, 0} };

    colors_init();
    const int rc = client_parse_common(&app, argc, argv, "r:", extra, extra_long);
    if (rc != 0 || !side_arg || (strcmp(side_arg, "A") && strcmp(side_arg, "B"))) {
        fprintf(rc < 0 || !side_arg ? stderr : stdout,
                "Использование: %s -s IP -p PORT -r A|B [--retries N] [--retry-ms MS] [--log FILE] [--quiet]\n"
                "  Государство A или B: хранит свою территорию, пополняет запас, подтверждает такты.\n", argv[0]);
        return rc == 1 ? 0 : 1;
    }
    st.side = side_arg[0] == 'B';
    st.last_resupply = -1;
    app.role = st.side ? "side-B" : "side-A";
    app.on_line = on_line;
    app.ctx = &st;
    const int code = client_run(&app);
    if (st.have_grid) grid_free(&st.grid);
    return code;
}
