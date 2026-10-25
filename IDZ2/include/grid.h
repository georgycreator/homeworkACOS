// grid.h — территория: прямоугольная клеточная карта с целями.
#ifndef GRID_H
#define GRID_H

#include <stdint.h>

// Предельная стоимость одной цели. Ограничение гарантирует, что стоимость
// помещается в int и что суммарная стоимость целей (до 10^6 клеток)
// помещается в int64_t без переполнения.
#define GRID_MAX_TARGET_COST   1000000000
// Сколько разных типов целей допускается на одной территории.
#define GRID_MAX_TYPES         16
#define GRID_TYPE_NAME_LEN     32
// Тип, который получает цель, если тип в конфигурации не указан.
#define GRID_DEFAULT_TYPE      "цель"

// Состояние клетки территории.
typedef enum {
    CELL_EMPTY = 0,   // пустая клетка
    CELL_TARGET,      // живая цель
    CELL_DESTROYED    // уничтоженная цель
} cell_state_t;

// Одна клетка: состояние, стоимость цели (0 для пустой) и индекс типа
// цели в таблице типов территории.
typedef struct {
    cell_state_t state;
    int          cost;
    uint8_t      type;
} cell_t;

// Территория: rows x cols клеток, хранятся в одномерном массиве row-major.
typedef struct {
    int     rows;
    int     cols;
    cell_t *cells;

    // Таблица названий типов целей (тип 0 — GRID_DEFAULT_TYPE).
    char    type_names[GRID_MAX_TYPES][GRID_TYPE_NAME_LEN];
    int     type_count;

    // Агрегаты для отчётности и проверки условий завершения.
    int     initial_targets;   // сколько целей было изначально
    int64_t initial_cost;      // суммарная стартовая стоимость
    int     alive_targets;     // сколько живых целей сейчас
    int64_t alive_cost;        // суммарная стоимость живых целей
} grid_t;

// Создать/освободить сетку. Возвращает 0 при успехе, -1 при ошибке.
int     grid_init(grid_t *g, int rows, int cols);
void    grid_free(grid_t *g);

// Доступ к клетке по координатам. NULL, если вне границ.
cell_t *grid_at(grid_t *g, int r, int c);

// Разместить цель типа type (NULL или "" — тип по умолчанию).
// Возвращает -1, если клетка вне границ или занята, стоимость вне диапазона
// 0..GRID_MAX_TARGET_COST либо исчерпана таблица типов.
int     grid_place_target(grid_t *g, int r, int c, int64_t cost, const char *type);

// Название типа цели в клетке.
const char *grid_type_name(const grid_t *g, const cell_t *cell);

// Напечатать карту (в консоль и в лог) с заголовком.
void    grid_print(const grid_t *g, const char *title);

// Напечатать список целей (координаты, тип, стоимость).
void    grid_print_targets(const grid_t *g);

#endif // GRID_H
