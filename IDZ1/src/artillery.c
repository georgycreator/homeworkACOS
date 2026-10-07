#define _POSIX_C_SOURCE 200809L
#include "artillery.h"
#include "log.h"
#include "colors.h"
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

void artillery_init(artillery_t *a) {
    memset(a, 0, sizeof(*a));
    a->accuracy = 100;
    a->strategy = STRAT_RANDOM;
    a->next_fire_tick = 0;   // готово к первому выстрелу сразу
}

int artillery_can_fire(const artillery_t *a) {
    if (a->ammo_left <= 0) return 0;
    if (a->budget_limit > 0 && a->spent_cost + a->ammo_cost > a->budget_limit)
        return 0;
    return 1;
}

int artillery_can_area_strike(const artillery_t *a) {
    if (a->area_ammo <= 0) return 0;
    // area_every = 0 означает «залпы не применяются» —
    // такие снаряды не считаются боеспособным запасом.
    if (a->area_every <= 0) return 0;
    if (a->area_cost <= 0) return 0;
    if (a->area_radius <= 0) return 0;
    if (a->budget_limit > 0 && a->spent_cost + a->area_cost > a->budget_limit)
        return 0;
    return 1;
}

void artillery_pick_coords(artillery_t *a, int rows, int cols,
                           int *out_r, int *out_c) {
    int r = 0, c = 0;
    switch (a->strategy) {
        case STRAT_RANDOM:
            r = rand() % rows;
            c = rand() % cols;
            break;
        case STRAT_CENTER: {
            // Приближённое смещение к центру: сумма двух равномерных.
            int rr = (rand() % rows + rand() % rows) / 2;
            int cc = (rand() % cols + rand() % cols) / 2;
            r = rr; c = cc;
            break;
        }
        case STRAT_SECTOR: {
            int total = rows * cols;
            int idx = a->sector_pos % total;
            a->sector_pos = (a->sector_pos + 1) % total;
            r = idx / cols;
            c = idx % cols;
            break;
        }
    }
    *out_r = r;
    *out_c = c;
}

void artillery_print_stats(const artillery_t *a, const char *owner_name) {
    log_printf("%s--- Огневое подразделение: %s ---%s\n",
               C_BOLD, owner_name, C_RESET);
    log_printf("  Выстрелов всего: %d (тяжёлых залпов: %d)\n",
               a->shots_fired, a->area_shots);
    log_printf("  Попаданий: %d, промахов: %d\n", a->hits, a->misses);
    log_printf("  Уничтожено целей противника: %d\n", a->targets_destroyed);
    log_printf("  Тяжёлых снарядов осталось: %d\n", a->area_ammo);
    log_printf("  Обычных снарядов осталось: %" PRId64 "\n", a->ammo_left);
    log_printf("  Всего израсходовано: %" PRId64 "\n", a->spent_cost);
}
