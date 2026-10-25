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

// Печать карты. Цвета: зелёный T — цель, красный x — уничтожено,
// тусклая точка — пусто. В лог-файл ANSI-коды не попадут (см. log.c).
// Формат: слева номер строки, сверху — номер столбца (одна цифра;
// для карт шире 10 столбцов цифры зацикливаются по модулю 10).
// Буфер строки выделяется под фактическую ширину карты: раньше строки
// шире примерно 600 клеток обрезались.
void grid_print(const grid_t *g, const char *title) {
    log_printf("%s%s%s (%dx%d), живых целей: %d, стоимость: %" PRId64 "\n",
               C_BOLD, title, C_RESET,
               g->rows, g->cols, g->alive_targets, g->alive_cost);

    // Одна клетка занимает не более 2 + 5 + 4 + 1 = 12 байт (цвет, символ, сброс, пробел).
    const size_t cap = (size_t)g->cols * 24u + 64u;
    char *line = (char *)malloc(cap);
    if (!line) return;

    size_t p = 0;
    p += (size_t)snprintf(line + p, cap - p, "    ");
    for (int c = 0; c < g->cols; c++)
        p += (size_t)snprintf(line + p, cap - p, "%d ", c % 10);
    log_printf("%s\n", line);

    for (int r = 0; r < g->rows; r++) {
        p = 0;
        p += (size_t)snprintf(line + p, cap - p, "%2d |", r);
        for (int c = 0; c < g->cols; c++) {
            const cell_t *cell = &g->cells[(size_t)r * (size_t)g->cols + (size_t)c];
            const char *color;
            char sym;
            switch (cell->state) {
                case CELL_EMPTY:     color = C_DIM;   sym = '.'; break;
                case CELL_TARGET:    color = C_GREEN; sym = 'T'; break;
                case CELL_DESTROYED: color = C_RED;   sym = 'x'; break;
            }
            p += (size_t)snprintf(line + p, cap - p, "%s%c%s ", color, sym, C_RESET);
        }
        log_printf("%s\n", line);
    }
    free(line);

    log_printf("  Легенда: %s.%s пусто, %sT%s цель, %sx%s уничтожено\n",
               C_DIM, C_RESET, C_GREEN, C_RESET, C_RED, C_RESET);
}

// Список целей: расположение, тип и стоимость. Длинный список обрезается.
void grid_print_targets(const grid_t *g) {
    enum { LIMIT = 40 };
    int shown = 0, total = 0;
    for (int r = 0; r < g->rows; r++) {
        for (int c = 0; c < g->cols; c++) {
            const cell_t *cell = &g->cells[(size_t)r * (size_t)g->cols + (size_t)c];
            if (cell->state == CELL_EMPTY) continue;
            total++;
            if (shown >= LIMIT) continue;
            log_printf("    (%d,%d) %s, стоимость %d\n", r, c,
                       grid_type_name(g, cell), cell->cost);
            shown++;
        }
    }
    if (total > shown)
        log_printf("    ... и ещё %d (показаны первые %d)\n", total - shown, shown);
}
