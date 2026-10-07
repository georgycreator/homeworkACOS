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
    return g->cells ? 0 : -1;
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

int grid_place_target(grid_t *g, int r, int c, int cost) {
    cell_t *cell = grid_at(g, r, c);
    if (!cell) return -1;                // координата вне поля
    if (cell->state != CELL_EMPTY) return -1;  // клетка занята

    cell->state = CELL_TARGET;
    cell->cost  = cost;

    g->initial_targets++;
    g->initial_cost += cost;
    g->alive_targets++;
    g->alive_cost   += cost;
    return 0;
}

// Печать карты. Цвета: зелёный T — цель, красный x — уничтожено,
// тусклая точка — пусто. В лог-файл ANSI-коды не попадут (см. log.c).
// Формат: слева номер строки, сверху — номер столбца (одна цифра;
// для карт шире 10 столбцов цифры зацикливаются по модулю 10).
// Вывод построчный, поэтому размер карты не ограничен буфером.
void grid_print(const grid_t *g, const char *title) {
    log_printf("%s%s%s (%dx%d), живых целей: %d, стоимость: %" PRId64 "\n",
               C_BOLD, title, C_RESET,
               g->rows, g->cols, g->alive_targets, g->alive_cost);

    // Заголовок столбцов.
    char head[1024];
    int hp = 0;
    hp += snprintf(head + hp, sizeof(head) - (size_t)hp, "    ");
    for (int c = 0; c < g->cols && hp < (int)sizeof(head) - 8; c++) {
        hp += snprintf(head + hp, sizeof(head) - (size_t)hp, "%d ", c % 10);
    }
    log_printf("%s\n", head);

    // Строки карты.
    for (int r = 0; r < g->rows; r++) {
        char row[8192];
        int rp = 0;
        rp += snprintf(row + rp, sizeof(row) - (size_t)rp, "%2d |", r);
        for (int c = 0; c < g->cols && rp < (int)sizeof(row) - 32; c++) {
            const cell_t *cell = &g->cells[(size_t)r * (size_t)g->cols + (size_t)c];
            const char *color = C_DIM;
            char sym = '.';
            switch (cell->state) {
                case CELL_EMPTY:     color = C_DIM;   sym = '.'; break;
                case CELL_TARGET:    color = C_GREEN; sym = 'T'; break;
                case CELL_DESTROYED: color = C_RED;   sym = 'x'; break;
            }
            rp += snprintf(row + rp, sizeof(row) - (size_t)rp,
                           "%s%c%s ", color, sym, C_RESET);
        }
        log_printf("%s\n", row);
    }

    log_printf("  Легенда: %s.%s пусто, %sT%s цель, %sx%s уничтожено\n",
               C_DIM, C_RESET, C_GREEN, C_RESET, C_RED, C_RESET);
}
