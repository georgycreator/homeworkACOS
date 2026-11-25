// side.h — государство: своя территория, параметры огневого подразделения, потери.
// В IDZ4 боевая логика вынесена в battle.h (она выполняется только сервером),
// а здесь остаётся описание данных стороны.
#ifndef SIDE_H
#define SIDE_H

#include "grid.h"
#include "artillery.h"
#include "util.h"

typedef struct {
    char name[32];          // название государства
    grid_t grid;            // своя территория с целями
    artillery_t art;        // боезапас, бюджет, стратегия, статистика подразделения
    int64_t lost_cost;      // суммарная стоимость потерянных своих целей
    int active;             // 1 — сторона продолжает бой
} side_t;

int  side_init(side_t *s, const char *name, int rows, int cols);
void side_free(side_t *s);

// Сформировать статистику государства и его подразделения в буфер.
void side_render_stats(const side_t *s, sbuf_t *sb);

#endif // SIDE_H
