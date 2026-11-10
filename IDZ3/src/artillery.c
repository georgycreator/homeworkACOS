#define _POSIX_C_SOURCE 200809L
#include "artillery.h"
#include "log.h"
#include "colors.h"
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

void artillery_init(artillery_t *a) {
    memset(a, 0, sizeof(*a));
    a->accuracy = 100;
    a->strategy = STRAT_RANDOM;
    a->rng_state = 0xA341316Cu;
}

// Небольшой независимый xorshift32: у каждого подразделения свой ГПСЧ.
// Это исключает гонки вокруг rand() и делает последовательность случайных
// чисел каждой стороны воспроизводимой при заданном seed.
//
// Качество: «сырой» seed в xorshift32 давал почти одинаковые первые выходы
// у соседних seed, а приведение к диапазону через % брало слабые младшие
// биты — на малых полях «случайная» стратегия почти не повторяла клетки.
// Поэтому seed перемешивается финализатором MurmurHash3, первые выходы
// отбрасываются, а диапазон получается по старшим битам.
static unsigned int mix32(unsigned int z) {
    z ^= z >> 16; z *= 0x85ebca6bu;
    z ^= z >> 13; z *= 0xc2b2ae35u;
    z ^= z >> 16;
    return z;
}

unsigned int artillery_random(artillery_t *a) {
    unsigned int x = a->rng_state;
    if (x == 0) x = 0xA341316Cu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    a->rng_state = x;
    return x;
}

void artillery_seed(artillery_t *a, unsigned int seed) {
    unsigned int s = mix32(seed + 0x9E3779B9u);
    a->rng_state = s ? s : 0xA341316Cu;
    for (int i = 0; i < 8; i++) (void)artillery_random(a);   // прогрев
}

int artillery_random_bounded(artillery_t *a, int upper_bound) {
    if (upper_bound <= 1) return 0;
    // Умножение на диапазон и сдвиг: используются старшие биты, смещение
    // не превышает upper_bound / 2^32.
    return (int)(((uint64_t)artillery_random(a) * (uint64_t)upper_bound) >> 32);
}

// Запас обычных снарядов есть.
int artillery_has_regular_ammo(const artillery_t *a) {
    return a->ammo_left > 0;
}

// Тяжёлые снаряды есть и их применение разрешено (area_every > 0).
// Радиус 0 и цена 0 допустимы: залп накрывает одну клетку, снаряд бесплатный.
int artillery_has_area_ammo(const artillery_t *a) {
    return a->area_ammo > 0 && a->area_every > 0;
}

int artillery_regular_affordable(const artillery_t *a) {
    return a->budget_limit <= 0 || a->spent_cost + a->ammo_cost <= a->budget_limit;
}

int artillery_area_affordable(const artillery_t *a) {
    return a->budget_limit <= 0 || a->spent_cost + a->area_cost <= a->budget_limit;
}

int artillery_can_fire(const artillery_t *a) {
    return artillery_has_regular_ammo(a) && artillery_regular_affordable(a);
}

int artillery_can_area_strike(const artillery_t *a) {
    return artillery_has_area_ammo(a) && artillery_area_affordable(a);
}

void artillery_pick_coords(artillery_t *a, int rows, int cols,
                           int *out_r, int *out_c) {
    int r = 0, c = 0;
    switch (a->strategy) {
        case STRAT_RANDOM:
            r = artillery_random_bounded(a, rows);
            c = artillery_random_bounded(a, cols);
            break;
        case STRAT_CENTER: {
            // Приближённое смещение к центру: сумма двух равномерных.
            int rr = (artillery_random_bounded(a, rows) + artillery_random_bounded(a, rows)) / 2;
            int cc = (artillery_random_bounded(a, cols) + artillery_random_bounded(a, cols)) / 2;
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
