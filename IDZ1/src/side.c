#define _POSIX_C_SOURCE 200809L
#include "side.h"
#include "log.h"
#include "colors.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int side_init(side_t *s, const char *name, int rows, int cols) {
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    if (grid_init(&s->grid, rows, cols) < 0) return -1;
    artillery_init(&s->art);
    s->active = 1;
    return 0;
}

void side_free(side_t *s) {
    grid_free(&s->grid);
}

static int side_can_fire(const side_t *s) {
    return s->active && artillery_can_fire(&s->art);
}

static int side_can_area_strike(const side_t *s) {
    return s->active && artillery_can_area_strike(&s->art);
}

int side_fire(side_t *shooter, side_t *victim, int tick) {
    if (!side_can_fire(shooter)) return 0;

    int r, c;
    artillery_pick_coords(&shooter->art, victim->grid.rows,
                          victim->grid.cols, &r, &c);

    shooter->art.ammo_left--;
    shooter->art.spent_cost += shooter->art.ammo_cost;
    shooter->art.shots_fired++;

    log_printf("%s[%s]%s[такт %d] Выстрел по %s(%d,%d)%s. "
               "Осталось снарядов: %" PRId64 ", израсходовано: %" PRId64 "\n",
               C_CYAN, shooter->name, C_RESET, tick,
               C_BOLD, r, c, C_RESET,
               shooter->art.ammo_left, shooter->art.spent_cost);

    if (shooter->art.accuracy < 100) {
        int roll = rand() % 100;
        if (roll >= shooter->art.accuracy) {
            shooter->art.misses++;
            log_printf("  -> %sПРОМАХ%s (точность %d%%, бросок %d)\n",
                       C_YELLOW, C_RESET, shooter->art.accuracy, roll);
            return 0;
        }
    }

    cell_t *cell = grid_at(&victim->grid, r, c);
    if (!cell) {
        shooter->art.misses++;
        log_printf("  -> %sПРОМАХ%s (вне территории)\n", C_YELLOW, C_RESET);
        return 0;
    }

    switch (cell->state) {
        case CELL_TARGET:
            cell->state = CELL_DESTROYED;
            victim->grid.alive_targets--;
            victim->grid.alive_cost -= cell->cost;
            victim->lost_cost += cell->cost;
            shooter->art.hits++;
            shooter->art.targets_destroyed++;
            log_printf("  -> %s%sПОПАДАНИЕ!%s Уничтожена цель стоимостью %s%d%s. "
                       "У %s осталось целей: %d (стоимость %" PRId64 "), "
                       "потери противника: %" PRId64 "\n",
                       C_BOLD, C_GREEN, C_RESET, C_BOLD, cell->cost, C_RESET,
                       victim->name, victim->grid.alive_targets,
                       victim->grid.alive_cost, victim->lost_cost);
            return 1;

        case CELL_DESTROYED:
            shooter->art.misses++;
            log_printf("  -> %sПРОМАХ%s (уже разрушена)\n", C_YELLOW, C_RESET);
            return 0;

        case CELL_EMPTY:
        default:
            shooter->art.misses++;
            log_printf("  -> %sПРОМАХ%s (пустая клетка)\n", C_YELLOW, C_RESET);
            return 0;
    }
}

int side_area_strike(side_t *shooter, side_t *victim, int tick) {
    if (!side_can_area_strike(shooter)) return 0;

    int r, c;
    artillery_pick_coords(&shooter->art, victim->grid.rows,
                          victim->grid.cols, &r, &c);

    shooter->art.area_ammo--;
    shooter->art.spent_cost += shooter->art.area_cost;
    shooter->art.area_shots++;
    shooter->art.shots_fired++;      // тяжёлый залп — тоже выстрел

    log_printf("\n%s%s[%s]%s%s[такт %d] ТЯЖЁЛЫЙ ЗАЛП%s по области "
               "%s(%d,%d)±%d%s. Тяжёлых снарядов: %d, израсходовано: %" PRId64 "\n",
               C_BOLD, C_RED, shooter->name, C_RESET, C_BOLD, tick, C_RESET,
               C_BOLD, r, c, shooter->art.area_radius, C_RESET,
               shooter->art.area_ammo, shooter->art.spent_cost);

    if (shooter->art.accuracy < 100) {
        int roll = rand() % 100;
        if (roll >= shooter->art.accuracy) {
            shooter->art.misses++;
            log_printf("  -> %sЗАЛП НЕ ДОСТИГ ЦЕЛИ%s "
                       "(точность %d%%, бросок %d)\n",
                       C_YELLOW, C_RESET, shooter->art.accuracy, roll);
            return 0;
        }
    }

    int destroyed = 0;
    int64_t cost_destroyed = 0;
    int R = shooter->art.area_radius;
    for (int dr = -R; dr <= R; dr++) {
        for (int dc = -R; dc <= R; dc++) {
            cell_t *cell = grid_at(&victim->grid, r + dr, c + dc);
            if (!cell) continue;
            if (cell->state == CELL_TARGET) {
                cell->state = CELL_DESTROYED;
                victim->grid.alive_targets--;
                victim->grid.alive_cost -= cell->cost;
                victim->lost_cost += cell->cost;
                shooter->art.targets_destroyed++;
                shooter->art.area_targets_destroyed++;
                destroyed++;
                cost_destroyed += cell->cost;
                log_printf("    %sпоражена%s цель (%d,%d) стоимостью %d\n",
                           C_GREEN, C_RESET, r + dr, c + dc, cell->cost);
            }
        }
    }

    if (destroyed == 0) {
        log_printf("  -> %sВ области поражения целей не было%s\n",
                   C_YELLOW, C_RESET);
        shooter->art.misses++;
        return 0;
    }
    shooter->art.hits++;
    log_printf("  -> %s%sУничтожено целей: %d, стоимость: %" PRId64 "%s. "
               "У %s осталось целей: %d (стоимость %" PRId64 "), "
               "потери противника: %" PRId64 "\n",
               C_BOLD, C_GREEN, destroyed, cost_destroyed, C_RESET,
               victim->name, victim->grid.alive_targets,
               victim->grid.alive_cost, victim->lost_cost);
    return 1;
}

void side_replenish(side_t *s) {
    if (s->art.replenish_amount <= 0) return;
    s->art.ammo_left += s->art.replenish_amount;
    log_printf("%s[%s]%s Пополнение: +%d снарядов, всего: %" PRId64 "\n",
               C_BLUE, s->name, C_RESET,
               s->art.replenish_amount, s->art.ammo_left);
}

// Проверка условий завершения для стороны. Различает четыре причины.
void side_check_done(side_t *s, const side_t *enemy) {
    if (!s->active) return;

    // 1) Все свои цели уничтожены.
    if (s->grid.alive_targets <= 0) {
        s->active = 0;
        log_printf("%s[%s]%s ПРЕКРАЩАЕТ ОГОНЬ: все свои цели уничтожены.\n",
                   C_MAGENTA, s->name, C_RESET);
        return;
    }

    // 2) У противника не осталось живых целей.
    if (enemy->grid.alive_targets <= 0) {
        s->active = 0;
        log_printf("%s[%s]%s ПРЕКРАЩАЕТ ОГОНЬ: у противника не осталось целей.\n",
                   C_MAGENTA, s->name, C_RESET);
        return;
    }

    // 3) Сторона не может сделать ни одного выстрела вообще.
    int can_regular = artillery_can_fire(&s->art);
    int can_area    = artillery_can_area_strike(&s->art);

    if (!can_regular && !can_area) {
        // 3a) Нет снарядов ни одного типа.
        if (s->art.ammo_left <= 0
            && (s->art.area_ammo <= 0 || s->art.area_every <= 0)) {
            // Если назначено пополнение — сторона "ждёт" снаряды
            // и не выключается окончательно.
            if (s->art.replenish_every > 0 && s->art.replenish_amount > 0) {
                return;
            }
            s->active = 0;
            log_printf("%s[%s]%s ПРЕКРАЩАЕТ ОГОНЬ: закончились боеприпасы "
                       "(обычные и тяжёлые).\n", C_MAGENTA, s->name, C_RESET);
            return;
        }
        // 3b) Снаряды есть, но бюджет не позволяет следующий выстрел.
        s->active = 0;
        log_printf("%s[%s]%s ПРЕКРАЩАЕТ ОГОНЬ: бюджет исчерпан "
                   "(израсходовано %" PRId64 " из %" PRId64 ").\n",
                   C_MAGENTA, s->name, C_RESET,
                   s->art.spent_cost, s->art.budget_limit);
        return;
    }
}

void side_print_stats(const side_t *s) {
    log_printf("%s=== Государство: %s ===%s\n", C_BOLD, s->name, C_RESET);
    log_printf("  Своих целей: живых %d, стоимость %" PRId64 "\n",
               s->grid.alive_targets, s->grid.alive_cost);
    log_printf("  Потеряно своих целей на сумму: %" PRId64 "\n", s->lost_cost);
    log_printf("  Активна: %s\n", s->active ? "да" : "нет");
    artillery_print_stats(&s->art, s->name);
}
