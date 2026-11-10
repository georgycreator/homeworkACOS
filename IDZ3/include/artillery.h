// artillery.h — огневое подразделение государства.
// Отдельная сущность: свой боезапас, бюджет, стратегия, статистика.
#ifndef ARTILLERY_H
#define ARTILLERY_H

#include <stdint.h>

// Стратегии выбора координат удара.
typedef enum {
    STRAT_RANDOM = 0,   // чистый случайный выбор
    STRAT_CENTER,       // смещение к центру (треугольное распределение)
    STRAT_SECTOR        // последовательный обход всех клеток
} strategy_t;

typedef struct {
    // Боезапас и экономика.
    // int64_t — чтобы при больших допустимых конфигах
    // не было переполнения (30 000 * 100 000 > INT_MAX).
    int64_t ammo_left;      // остаток обычных снарядов
    int64_t ammo_cost;      // цена одного обычного выстрела
    int64_t spent_cost;     // накопленные расходы (обычные + тяжёлые)
    int64_t budget_limit;   // 0 = без лимита

    // Пополнение запаса.
    int replenish_every;    // каждые N тактов; 0 = не пополнять
    int replenish_amount;   // +сколько снарядов за раз

    // Стратегия и точность.
    strategy_t strategy;
    int accuracy;           // 0..100

    // Интервал между выстрелами подразделения (мс): reaction_ms плюс
    // случайная добавка 0..reaction_jitter_ms. Применяется в режиме с
    // ненулевой паузой такта; такт — квант модели, поэтому реальный интервал
    // не короче одного такта. При tick_ms=0 время не моделируется, и
    // подразделение стреляет один раз за такт.
    int reaction_ms;
    int reaction_jitter_ms;

    // Тяжёлые (площадные) снаряды.
    int area_ammo;
    int area_cost;          // цена одного тяжёлого снаряда (0 допустим)
    int area_radius;        // радиус поражения в клетках (0 — только клетка прицеливания)
    int area_every;         // период залпов в тактах; 0 = тяжёлые снаряды не применяются

    // Статистика.
    // shots_fired — число ВСЕХ сделанных выстрелов (обычные + тяжёлые).
    // hits        — число результативных выстрелов.
    // misses      — число безрезультатных выстрелов.
    // Инвариант: shots_fired == hits + misses.
    // area_shots  — дополнительно: сколько из них были тяжёлыми.
    int shots_fired;
    int hits;
    int misses;
    int targets_destroyed;
    int area_shots;
    int area_targets_destroyed;

    int sector_pos;         // служебное состояние стратегии SECTOR
    unsigned int rng_state; // отдельное состояние ГПСЧ для этого подразделения
} artillery_t;

void artillery_init(artillery_t *a);
void artillery_seed(artillery_t *a, unsigned int seed);
unsigned int artillery_random(artillery_t *a);
int  artillery_random_bounded(artillery_t *a, int upper_bound);
int  artillery_can_fire(const artillery_t *a);
int  artillery_can_area_strike(const artillery_t *a);
// Есть ли в принципе применимый запас (без учёта бюджета).
int  artillery_has_regular_ammo(const artillery_t *a);
int  artillery_has_area_ammo(const artillery_t *a);
// Хватает ли бюджета на следующий выстрел данного типа.
int  artillery_regular_affordable(const artillery_t *a);
int  artillery_area_affordable(const artillery_t *a);
void artillery_pick_coords(artillery_t *a, int rows, int cols,
                           int *out_r, int *out_c);
void artillery_print_stats(const artillery_t *a, const char *owner_name);

#endif // ARTILLERY_H
