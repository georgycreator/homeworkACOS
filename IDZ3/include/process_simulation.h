// process_simulation.h — исполнение игровой модели отдельными процессами.
#ifndef PROCESS_SIMULATION_H
#define PROCESS_SIMULATION_H

#include "config.h"
#include "ipc.h"

typedef enum {
    SIM_STOP_NATURAL = 0,
    SIM_STOP_LIMIT = 1,
    SIM_STOP_SIGNAL = 2,
    SIM_STOP_ERROR = 3
} simulation_stop_reason_t;

simulation_stop_reason_t simulation_run_processes(config_t *cfg, int *ticks_done,
                                                  ipc_mode_t mode);

// Точки входа независимых исполняемых файлов IDZ3_side / IDZ3_artillery.
int independent_worker_main(const char *session, int is_artillery,
                            int side_index);
int independent_coordinator_main(const char *session);
int independent_init_main(const char *session, const char *config_path);
int independent_cleanup_main(const char *session);

#endif
