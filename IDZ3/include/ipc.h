// ipc.h — POSIX Shared Memory и два варианта POSIX-семафоров.
#ifndef IPC_H
#define IPC_H

#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <sys/types.h>

#define IPC_MAGIC 0x49445A33u
#define IPC_VERSION 2u
#define IPC_WORKER_COUNT 4
#define IPC_ALL_WORKERS_MASK ((1u << IPC_WORKER_COUNT) - 1u)
#define IPC_NAME_LEN 64
#define IPC_SEM_COUNT 9

typedef enum {
    IPC_SEM_NAMED = 1,
    IPC_SEM_UNNAMED = 2
} ipc_mode_t;

typedef enum {
    IPC_SIDE_TICK_A = 0,
    IPC_SIDE_TICK_B,
    IPC_SIDE_ACK_A,
    IPC_SIDE_ACK_B,
    IPC_FIRE_TICK_A,
    IPC_FIRE_TICK_B,
    IPC_FIRE_ACK_A,
    IPC_FIRE_ACK_B,
    IPC_READY
} ipc_sem_index_t;

typedef struct {
    unsigned int magic;
    unsigned int version;
    size_t mapping_size;
    size_t cells_a_offset;
    size_t cells_b_offset;
    ipc_mode_t mode;
    char session[32];
    char shm_name[IPC_NAME_LEN];
    char sem_names[IPC_SEM_COUNT][IPC_NAME_LEN];

    // В model хранятся все поля предметной модели, кроме указателей grid.cells.
    // Сами массивы клеток находятся далее в этом же отображении памяти.
    config_t model;

    pthread_mutex_t state_mutex; // PTHREAD_PROCESS_SHARED
    sem_t unnamed_sems[IPC_SEM_COUNT];

    int current_tick;
    int ticks_completed;
    int side_processed_tick[2];
    int stopping;
    int stop_reason;
    int ready_count;
    int finished_workers;
    unsigned int registered_mask; // уникальные роли: side A/B, artillery A/B
    unsigned int finished_mask;   // роли, завершившиеся после остановки модели
    int fatal_error;              // авария участника или восстановление robust mutex
    pid_t pids[IPC_WORKER_COUNT]; // PID сохраняются до удаления сессии
} ipc_shared_t;

typedef struct {
    int shm_fd;
    ipc_shared_t *shared;
    size_t mapping_size;
    ipc_mode_t mode;
    int owner;
    int mutex_initialized;
    int unnamed_sem_count;
    char shm_name[IPC_NAME_LEN];
    sem_t *named_sems[IPC_SEM_COUNT];
} ipc_handle_t;

int ipc_create(ipc_handle_t *ipc, const config_t *cfg, ipc_mode_t mode,
               const char *session);
int ipc_attach(ipc_handle_t *ipc, const char *session);
void ipc_close(ipc_handle_t *ipc);
void ipc_destroy(ipc_handle_t *ipc);

int ipc_lock(ipc_shared_t *shared);
void ipc_unlock(ipc_shared_t *shared);
int ipc_sem_post(ipc_handle_t *ipc, ipc_sem_index_t index);
int ipc_sem_wait(ipc_handle_t *ipc, ipc_sem_index_t index);
int ipc_sem_timedwait_ms(ipc_handle_t *ipc, ipc_sem_index_t index, long timeout_ms);

void ipc_model_view(ipc_shared_t *shared, config_t *out);
void ipc_model_save(ipc_shared_t *shared, const config_t *model);
void ipc_model_copy_back(ipc_shared_t *shared, config_t *destination);

int ipc_session_valid(const char *session);

#endif
