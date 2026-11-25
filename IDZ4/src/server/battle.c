#define _POSIX_C_SOURCE 200809L
#include "battle.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void battle_result_init(shot_result_t *res) { memset(res, 0, sizeof(*res)); }

void battle_result_free(shot_result_t *res) {
    free(res->kills);
    memset(res, 0, sizeof(*res));
}

// Добавить уничтоженную цель в список результата.
static void add_kill(shot_result_t *res, int r, int c, const cell_t *cell, const char *type) {
    if (res->kills_n == res->kills_cap) {
        const int ncap = res->kills_cap ? res->kills_cap * 2 : 8;
        kill_t *p = (kill_t *)realloc(res->kills, (size_t)ncap * sizeof(kill_t));
        if (!p) return;                         // нет памяти: список усечётся, счётчики верны
        res->kills = p;
        res->kills_cap = ncap;
    }
    kill_t *k = &res->kills[res->kills_n++];
    k->r = r; k->c = c; k->cost = cell->cost;
    strncpy(k->type, type, sizeof(k->type) - 1);
    k->type[sizeof(k->type) - 1] = '\0';
}

reject_t battle_validate(const side_t *shooter, const side_t *victim,
                         char kind, int r, int c, int roll) {
    if (kind != 'R' && kind != 'H') return REJ_BADARGS;
    if (roll < 0 || roll > 99) return REJ_BADARGS;
    if (r < 0 || r >= victim->grid.rows || c < 0 || c >= victim->grid.cols) return REJ_BADCOORD;
    if (!shooter->active) return REJ_CEASEFIRE;

    const artillery_t *a = &shooter->art;
    if (kind == 'R') {
        if (!artillery_has_regular_ammo(a)) return REJ_NOAMMO;
        if (!artillery_regular_affordable(a)) return REJ_BUDGET;
    } else {
        if (!artillery_has_area_ammo(a)) return REJ_NOAMMO;
        if (!artillery_area_affordable(a)) return REJ_BUDGET;
    }
    return REJ_NONE;
}

// Уничтожить цель в клетке: единственное место, где меняются агрегаты территории,
// поэтому «одна цель учитывается один раз» и «числа не уходят в минус» выполняются
// по построению (цель переходит TARGET -> DESTROYED ровно один раз).
static void destroy_cell(side_t *victim, cell_t *cell) {
    cell->state = CELL_DESTROYED;
    victim->grid.alive_targets--;
    victim->grid.alive_cost -= cell->cost;
    victim->lost_cost += cell->cost;
}

void battle_execute(side_t *shooter, side_t *victim, char kind,
                    int r, int c, int roll, shot_result_t *res) {
    battle_result_init(res);
    res->kind = kind; res->r = r; res->c = c; res->roll = roll;
    artillery_t *a = &shooter->art;

    // Расход боеприпаса — один снаряд, независимо от исхода.
    if (kind == 'R') {
        a->ammo_left--;
        a->spent_cost += a->ammo_cost;
    } else {
        a->area_ammo--;
        a->spent_cost += a->area_cost;
        a->area_shots++;
    }
    a->shots_fired++;

    // Точность: бросок не меньше точности — снаряд ушёл мимо.
    if (a->accuracy < 100 && roll >= a->accuracy) {
        a->misses++;
        res->outcome = OUT_MISS_ACCURACY;
        return;
    }

    if (kind == 'R') {
        cell_t *cell = grid_at(&victim->grid, r, c);
        if (!cell) { a->misses++; res->outcome = OUT_MISS_EMPTY; return; }
        switch (cell->state) {
            case CELL_TARGET:
                add_kill(res, r, c, cell, grid_type_name(&victim->grid, cell));
                destroy_cell(victim, cell);
                a->hits++;
                a->targets_destroyed++;
                res->destroyed = 1;
                res->destroyed_cost = cell->cost;
                res->outcome = OUT_HIT;
                return;
            case CELL_DESTROYED:
                a->misses++; res->outcome = OUT_MISS_DESTROYED; return;
            case CELL_EMPTY:
            default:
                a->misses++; res->outcome = OUT_MISS_EMPTY; return;
        }
    }

    // Тяжёлый залп: квадрат (2R+1)x(2R+1) вокруг точки прицеливания.
    const int R = a->area_radius;
    for (int dr = -R; dr <= R; dr++) {
        for (int dc = -R; dc <= R; dc++) {
            cell_t *cell = grid_at(&victim->grid, r + dr, c + dc);
            if (!cell || cell->state != CELL_TARGET) continue;
            add_kill(res, r + dr, c + dc, cell, grid_type_name(&victim->grid, cell));
            destroy_cell(victim, cell);
            a->targets_destroyed++;
            a->area_targets_destroyed++;
            res->destroyed++;
            res->destroyed_cost += cell->cost;
        }
    }
    if (res->destroyed == 0) {
        a->misses++;
        res->outcome = OUT_AREA_NONE;
        return;
    }
    a->hits++;
    res->outcome = OUT_AREA_HIT;
}

int battle_replenish(side_t *s) {
    if (s->art.replenish_amount <= 0) return 0;
    s->art.ammo_left += s->art.replenish_amount;
    return s->art.replenish_amount;
}

// Условия прекращения огня перенесены из IDZ3 без изменения смысла.
// Меняет только собственный флаг active стороны s; противник — только чтение.
int battle_check_done(side_t *s, const side_t *enemy, char *why, size_t why_cap) {
    if (!s->active) return 0;

    if (s->grid.alive_targets <= 0) {
        s->active = 0;
        snprintf(why, why_cap, "все свои цели уничтожены");
        return 1;
    }
    if (enemy->grid.alive_targets <= 0) {
        s->active = 0;
        snprintf(why, why_cap, "у противника не осталось целей");
        return 1;
    }

    const artillery_t *a = &s->art;
    if (artillery_can_fire(a) || artillery_can_area_strike(a)) return 0;

    const int has_regular = artillery_has_regular_ammo(a);
    const int has_area    = artillery_has_area_ammo(a);
    const int will_replenish = a->replenish_every > 0 && a->replenish_amount > 0;

    // Обычных снарядов нет, но после пополнения выстрел будет оплачен — ждём.
    if (!has_regular && will_replenish && artillery_regular_affordable(a)) return 0;

    const int budget_blocked =
        (has_regular && !artillery_regular_affordable(a)) ||
        (has_area && !artillery_area_affordable(a)) ||
        (!has_regular && will_replenish && !artillery_regular_affordable(a));
    s->active = 0;
    if (budget_blocked)
        snprintf(why, why_cap, "бюджет исчерпан (израсходовано %" PRId64 " из %" PRId64 ")",
                 a->spent_cost, a->budget_limit);
    else
        snprintf(why, why_cap, "закончились боеприпасы (обычные и тяжёлые)");
    return 1;
}

void battle_describe(const side_t *shooter, const side_t *victim, int tick,
                     const shot_result_t *res, sbuf_t *sb) {
    const artillery_t *a = &shooter->art;
    if (res->kind == 'R') {
        sb_printf(sb, "[АРТИЛЛЕРИЯ %s] [такт %d] Выстрел по (%d,%d). "
                      "Осталось снарядов: %" PRId64 ", израсходовано: %" PRId64 "\n",
                  shooter->name, tick, res->r, res->c, a->ammo_left, a->spent_cost);
    } else {
        sb_printf(sb, "[АРТИЛЛЕРИЯ %s] [такт %d] ТЯЖЁЛЫЙ ЗАЛП по области (%d,%d)±%d. "
                      "Тяжёлых снарядов: %d, израсходовано: %" PRId64 "\n",
                  shooter->name, tick, res->r, res->c, a->area_radius,
                  a->area_ammo, a->spent_cost);
    }
    switch (res->outcome) {
        case OUT_MISS_ACCURACY:
            sb_printf(sb, "  -> ПРОМАХ (точность %d%%, бросок %d)\n", a->accuracy, res->roll);
            break;
        case OUT_MISS_DESTROYED:
            sb_printf(sb, "  -> ПРОМАХ (уже разрушена)\n");
            break;
        case OUT_MISS_EMPTY:
            sb_printf(sb, "  -> ПРОМАХ (пустая клетка)\n");
            break;
        case OUT_AREA_NONE:
            sb_printf(sb, "  -> В области поражения целей не было\n");
            break;
        case OUT_HIT:
            sb_printf(sb, "  -> ПОПАДАНИЕ! Уничтожена цель «%s» стоимостью %d. "
                          "У %s осталось целей: %d (стоимость %" PRId64 "), потери противника: %" PRId64 "\n",
                      res->kills_n ? res->kills[0].type : "цель", (int)res->destroyed_cost,
                      victim->name, victim->grid.alive_targets, victim->grid.alive_cost,
                      victim->lost_cost);
            break;
        case OUT_AREA_HIT:
            for (int i = 0; i < res->kills_n && i < 20; i++)
                sb_printf(sb, "    поражена цель «%s» (%d,%d) стоимостью %d\n",
                          res->kills[i].type, res->kills[i].r, res->kills[i].c, res->kills[i].cost);
            if (res->kills_n > 20) sb_printf(sb, "    ... и ещё %d\n", res->kills_n - 20);
            sb_printf(sb, "  -> ПОПАДАНИЕ! Уничтожено целей: %d, стоимость: %" PRId64 ". "
                          "У %s осталось целей: %d (стоимость %" PRId64 "), потери противника: %" PRId64 "\n",
                      res->destroyed, res->destroyed_cost, victim->name,
                      victim->grid.alive_targets, victim->grid.alive_cost, victim->lost_cost);
            break;
    }
}
