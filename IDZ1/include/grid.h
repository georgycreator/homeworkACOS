// grid.h — территория: прямоугольная клеточная карта с целями.
#ifndef GRID_H
#define GRID_H

#include <stdint.h>

// Состояние клетки территории.
typedef enum {
    CELL_EMPTY = 0,   // пустая клетка
    CELL_TARGET,      // живая цель
    CELL_DESTROYED    // уничтоженная цель
} cell_state_t;

// Одна клетка: состояние + стоимость цели (0 для пустой).
typedef struct {
    cell_state_t state;
    int          cost;
} cell_t;

// Территория: rows x cols клеток, хранятся в одномерном массиве row-major.
typedef struct {
    int     rows;
    int     cols;
    cell_t *cells;

    // Агрегаты для отчётности и проверки условий завершения.
    int initial_targets;   // сколько целей было изначально
    int64_t initial_cost;      // суммарная стартовая стоимость
    int alive_targets;     // сколько живых целей сейчас
    int64_t alive_cost;        // суммарная стоимость живых целей
} grid_t;

// Создать/освободить сетку. Возвращает 0 при успехе, -1 при ошибке.
int     grid_init(grid_t *g, int rows, int cols);
void    grid_free(grid_t *g);

// Доступ к клетке по координатам. NULL, если вне границ.
cell_t *grid_at(grid_t *g, int r, int c);

// Разместить цель. Игнорируется, если клетка вне границ или уже занята.
int     grid_place_target(grid_t *g, int r, int c, int cost);

// Напечатать карту (в консоль и в лог) с заголовком.
void    grid_print(const grid_t *g, const char *title);

#endif // GRID_H
