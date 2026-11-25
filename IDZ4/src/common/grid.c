#define _POSIX_C_SOURCE 200809L
#include "grid.h"
#include "log.h"
#include "colors.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

int grid_init(grid_t *g, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return -1;
    memset(g, 0, sizeof(*g));
    g->rows = rows;
    g->cols = cols;
    g->cells = (cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(cell_t));
    if (!g->cells) return -1;
    // Тип 0 всегда существует: так цель без указанного типа тоже имеет название.
    strncpy(g->type_names[0], GRID_DEFAULT_TYPE, GRID_TYPE_NAME_LEN - 1);
    g->type_count = 1;
    return 0;
}

void grid_free(grid_t *g) {
    free(g->cells);
    g->cells = NULL;
    g->rows = g->cols = 0;
}

cell_t *grid_at(grid_t *g, int r, int c) {
    if (!g) return NULL;
    if (r < 0 || r >= g->rows || c < 0 || c >= g->cols) return NULL;
    return &g->cells[(size_t)r * (size_t)g->cols + (size_t)c];
}

// Найти тип по названию или добавить новый. -1, если таблица заполнена.
static int grid_type_id(grid_t *g, const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < g->type_count; i++)
        if (strncmp(g->type_names[i], name, GRID_TYPE_NAME_LEN - 1) == 0) return i;
    if (g->type_count >= GRID_MAX_TYPES) return -1;
    strncpy(g->type_names[g->type_count], name, GRID_TYPE_NAME_LEN - 1);
    return g->type_count++;
}

int grid_place_target(grid_t *g, int r, int c, int64_t cost, const char *type) {
    // Стоимость проверяется ДО приведения к int: раньше значение больше
    // INT_MAX молча превращалось в отрицательное.
    if (cost < 0 || cost > GRID_MAX_TARGET_COST) return -1;
    cell_t *cell = grid_at(g, r, c);
    if (!cell) return -1;                      // координата вне поля
    if (cell->state != CELL_EMPTY) return -1;  // клетка занята
    int tid = grid_type_id(g, type);
    if (tid < 0) return -1;                    // слишком много типов

    cell->state = CELL_TARGET;
    cell->cost  = (int)cost;
    cell->type  = (uint8_t)tid;

    g->initial_targets++;
    g->initial_cost += cost;
    g->alive_targets++;
    g->alive_cost   += cost;
    return 0;
}

const char *grid_type_name(const grid_t *g, const cell_t *cell) {
    if (!cell || cell->type >= g->type_count) return GRID_DEFAULT_TYPE;
    return g->type_names[cell->type];
}

// Формирование карты в буфер. Цвета: зелёный T — цель, красный x — уничтожено,
// тусклая точка — пусто. Формат: слева номер строки, сверху — номер столбца
// (одна цифра, для карт шире 10 столбцов цифры зацикливаются по модулю 10).
void grid_render(const grid_t *g, const char *title, int color, sbuf_t *sb) {
    const char *bold = color ? C_BOLD : "", *reset = color ? C_RESET : "";
    const char *dim = color ? C_DIM : "", *green = color ? C_GREEN : "", *red = color ? C_RED : "";

    sb_printf(sb, "%s%s%s (%dx%d), живых целей: %d, стоимость: %" PRId64 "\n",
              bold, title, reset, g->rows, g->cols, g->alive_targets, g->alive_cost);

    sb_printf(sb, "    ");
    for (int c = 0; c < g->cols; c++) sb_printf(sb, "%d ", c % 10);
    sb_printf(sb, "\n");

    for (int r = 0; r < g->rows; r++) {
        sb_printf(sb, "%2d |", r);
        for (int c = 0; c < g->cols; c++) {
            const cell_t *cell = &g->cells[(size_t)r * (size_t)g->cols + (size_t)c];
            const char *col = reset;
            char sym = '?';
            switch (cell->state) {
                case CELL_EMPTY:     col = dim;   sym = '.'; break;
                case CELL_TARGET:    col = green; sym = 'T'; break;
                case CELL_DESTROYED: col = red;   sym = 'x'; break;
                default:             col = red;   sym = '?'; break;
            }
            sb_printf(sb, "%s%c%s ", col, sym, reset);
        }
        sb_printf(sb, "\n");
    }
    sb_printf(sb, "  Легенда: %s.%s пусто, %sT%s цель, %sx%s уничтожено\n",
              dim, reset, green, reset, red, reset);
}

void grid_print(const grid_t *g, const char *title) {
    sbuf_t sb;
    sb_init(&sb);
    grid_render(g, title, 1, &sb);
    log_printf("%s", sb_str(&sb));
    sb_free(&sb);
}

// Список целей: расположение, тип и стоимость. Длинный список обрезается.
void grid_render_targets(const grid_t *g, int limit, sbuf_t *sb) {
    int shown = 0, total = 0;
    for (int r = 0; r < g->rows; r++) {
        for (int c = 0; c < g->cols; c++) {
            const cell_t *cell = &g->cells[(size_t)r * (size_t)g->cols + (size_t)c];
            if (cell->state == CELL_EMPTY) continue;
            total++;
            if (shown >= limit) continue;
            sb_printf(sb, "    (%d,%d) %s, стоимость %d\n", r, c,
                      grid_type_name(g, cell), cell->cost);
            shown++;
        }
    }
    if (total > shown)
        sb_printf(sb, "    ... и ещё %d (показаны первые %d)\n", total - shown, shown);
}

void grid_print_targets(const grid_t *g) {
    sbuf_t sb;
    sb_init(&sb);
    grid_render_targets(g, 40, &sb);
    log_printf("%s", sb_str(&sb));
    sb_free(&sb);
}

void grid_row_chars(const grid_t *g, int row, char *out) {
    for (int c = 0; c < g->cols; c++) {
        const cell_t *cell = &g->cells[(size_t)row * (size_t)g->cols + (size_t)c];
        out[c] = cell->state == CELL_TARGET ? 'T' : cell->state == CELL_DESTROYED ? 'x' : '.';
    }
    out[g->cols] = '\0';
}
