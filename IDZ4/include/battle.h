// battle.h — боевая логика, исполняемая ТОЛЬКО сервером (координатором).
// Клиенты не меняют территории напрямую: они присылают запрос SHOT, сервер
// проверяет его, регистрирует и применяет к территории противника.
#ifndef BATTLE_H
#define BATTLE_H

#include "side.h"
#include "proto.h"
#include "util.h"

typedef struct {
    int  r, c, cost;
    char type[GRID_TYPE_NAME_LEN];
} kill_t;

// Результат одного зарегистрированного выстрела.
typedef struct {
    char      kind;            // 'R' — обычный, 'H' — тяжёлый
    int       r, c, roll;      // координаты прицеливания и бросок точности
    outcome_t outcome;
    int       destroyed;       // сколько целей уничтожено этим выстрелом
    int64_t   destroyed_cost;  // их суммарная стоимость
    kill_t   *kills;           // список уничтоженных целей (динамический)
    int       kills_n, kills_cap;
} shot_result_t;

void battle_result_init(shot_result_t *res);
void battle_result_free(shot_result_t *res);

// Проверка запроса. REJ_NONE — выстрел допустим (ничего не изменено).
reject_t battle_validate(const side_t *shooter, const side_t *victim,
                         char kind, int r, int c, int roll);

// Применить допустимый выстрел: расходует РОВНО ОДИН снаряд, изменяет только
// территорию victim и статистику shooter.
void battle_execute(side_t *shooter, side_t *victim, char kind,
                    int r, int c, int roll, shot_result_t *res);

// Пополнение запаса. Возвращает добавленное число снарядов.
int  battle_replenish(side_t *s);

// Проверить условия прекращения огня. Если сторона только что стала
// неактивной — возвращает 1 и записывает причину в why.
int  battle_check_done(side_t *s, const side_t *enemy, char *why, size_t why_cap);

// Текст о выстреле для журнала (без цвета).
void battle_describe(const side_t *shooter, const side_t *victim, int tick,
                     const shot_result_t *res, sbuf_t *sb);

#endif // BATTLE_H
