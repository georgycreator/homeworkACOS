// side.h — государство: своя территория, огневое подразделение, потери.
#ifndef SIDE_H
#define SIDE_H

#include "grid.h"
#include "artillery.h"

typedef struct {
    char name[32];          // название государства
    grid_t grid;            // своя территория с целями
    artillery_t art;        // огневое подразделение (отдельная сущность)
    int64_t lost_cost;          // суммарная стоимость потерянных своих целей
    int active;             // 1 — сторона продолжает бой
} side_t;

// Создать/освободить государство.
int  side_init(side_t *s, const char *name, int rows, int cols);
void side_free(side_t *s);

// Выстрел. Возвращает 1, если состояние территории противника изменилось
// (попадание и уничтожение цели), или 0 при промахе.
int  side_fire(side_t *shooter, side_t *victim, int tick);

// Тяжёлый залп по области. Возвращает то же самое.
int  side_area_strike(side_t *shooter, side_t *victim, int tick);

// Пополнение запаса снарядов.
void side_replenish(side_t *s);

// Проверить условия завершения боя для стороны.
void side_check_done(side_t *s, const side_t *enemy);

// Напечатать статистику государства и его подразделения.
void side_print_stats(const side_t *s);

#endif // SIDE_H
