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

    // Интервал между выстрелами подразделения (мс).
    // next_fire_tick — на каком такте подразделение снова готово стрелять.
    // Пересчитывается после каждого выстрела: next_fire = tick + reload,
    // где reload = reaction_ms / tick_ms + rand%jitter. Это даёт
    // имитацию независимых асинхронных сторон в одном потоке.
    int reaction_ms;
    int reaction_jitter_ms;
    int next_fire_tick;

    // Тяжёлые (площадные) снаряды.
    int area_ammo;
    int area_cost;
    int area_radius;
    int area_every;

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
} artillery_t;

void artillery_init(artillery_t *a);
int  artillery_can_fire(const artillery_t *a);
int  artillery_can_area_strike(const artillery_t *a);
void artillery_pick_coords(artillery_t *a, int rows, int cols,
                           int *out_r, int *out_c);
void artillery_print_stats(const artillery_t *a, const char *owner_name);

#endif // ARTILLERY_H
