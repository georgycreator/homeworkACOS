// simulation.h — многопоточный исполнитель модели.
#ifndef SIMULATION_H
#define SIMULATION_H

#include "config.h"

typedef enum {
    SIM_STOP_NATURAL = 0,  // обе стороны прекратили огонь
    SIM_STOP_LIMIT = 1,    // достигнуто максимальное число тактов
    SIM_STOP_SIGNAL = 2,   // SIGINT или SIGTERM
    SIM_STOP_ERROR = 3     // ошибка создания/работы потоков
} simulation_stop_reason_t;

// Выполнить модель. Два потока государств проверяют условия прекращения огня и пополняют запас,
// два огневых потока независимо стреляют, текущий (основной) поток —
// координатор тактов и завершения.
simulation_stop_reason_t simulation_run(config_t *cfg, int *ticks_done);

#endif // SIMULATION_H
