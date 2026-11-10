#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *const sem_suffixes[IPC_SEM_COUNT] = {
    "side_tick_a", "side_tick_b", "side_ack_a", "side_ack_b",
    "fire_tick_a", "fire_tick_b", "fire_ack_a", "fire_ack_b", "ready"
};

int ipc_session_valid(const char *session) {
    if (!session || !*session || strlen(session) >= 32) return 0;
    for (const unsigned char *p = (const unsigned char *)session; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    }
    return 1;
}

static size_t cells_bytes(const grid_t *grid) {
    return (size_t)grid->rows * (size_t)grid->cols * sizeof(cell_t);
}

static size_t align_up(size_t value, size_t alignment) {
    const size_t rem = value % alignment;
    return rem ? value + alignment - rem : value;
}

static void clear_handle(ipc_handle_t *ipc) {
    memset(ipc, 0, sizeof(*ipc));
    ipc->shm_fd = -1;
    for (int i = 0; i < IPC_SEM_COUNT; ++i) ipc->named_sems[i] = SEM_FAILED;
}

int ipc_lock(ipc_shared_t *shared) {
    if (!shared) {
        errno = EINVAL;
        return -1;
    }

    const int rc = pthread_mutex_lock(&shared->state_mutex);
#ifdef EOWNERDEAD
    if (rc == EOWNERDEAD) {
        // Robust mutex сообщает, что предыдущий владелец умер внутри
        // критической секции. Невозможно достоверно определить, на каком
        // поле он остановился, поэтому НЕ продолжаем игровую симуляцию.
        // Помечаем сессию аварийной, делаем mutex консистентным только для
        // безопасного чтения/завершения и возвращаем ошибку вызывающему коду.
        const int consistent_rc = pthread_mutex_consistent(&shared->state_mutex);
        if (consistent_rc == 0) {
            shared->fatal_error = 1;
            shared->stopping = 1;
            shared->stop_reason = 3; // SIM_STOP_ERROR; ipc.h не зависит от simulation.h
            (void)pthread_mutex_unlock(&shared->state_mutex);
            errno = EOWNERDEAD;
        } else {
            // Освобождаем mutex в состоянии ENOTRECOVERABLE, чтобы не
            // оставить остальные процессы навечно заблокированными.
            (void)pthread_mutex_unlock(&shared->state_mutex);
            errno = consistent_rc;
        }
        return -1;
    }
#endif
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void ipc_unlock(ipc_shared_t *shared) {
    (void)pthread_mutex_unlock(&shared->state_mutex);
}

static sem_t *sem_handle(ipc_handle_t *ipc, ipc_sem_index_t index) {
    if (!ipc || index < 0 || index >= IPC_SEM_COUNT) return SEM_FAILED;
    return ipc->mode == IPC_SEM_UNNAMED
        ? &ipc->shared->unnamed_sems[index]
        : ipc->named_sems[index];
}

int ipc_sem_post(ipc_handle_t *ipc, ipc_sem_index_t index) {
    sem_t *sem = sem_handle(ipc, index);
    return sem == SEM_FAILED ? -1 : sem_post(sem);
}

int ipc_sem_wait(ipc_handle_t *ipc, ipc_sem_index_t index) {
    sem_t *sem = sem_handle(ipc, index);
    if (sem == SEM_FAILED) return -1;
    for (;;) {
        if (sem_wait(sem) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

int ipc_sem_timedwait_ms(ipc_handle_t *ipc, ipc_sem_index_t index, long timeout_ms) {
    sem_t *sem = sem_handle(ipc, index);
    if (sem == SEM_FAILED) return -1;
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) < 0) return -1;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;) {
        if (sem_timedwait(sem, &deadline) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) return 1;
        return -1;
    }
}

void ipc_model_view(ipc_shared_t *shared, config_t *out) {
    *out = shared->model;
    out->A.grid.cells = (cell_t *)((char *)shared + shared->cells_a_offset);
    out->B.grid.cells = (cell_t *)((char *)shared + shared->cells_b_offset);
}

void ipc_model_save(ipc_shared_t *shared, const config_t *model) {
    shared->model = *model;
    shared->model.A.grid.cells = NULL;
    shared->model.B.grid.cells = NULL;
}

void ipc_model_copy_back(ipc_shared_t *shared, config_t *destination) {
    config_t view;
    ipc_model_view(shared, &view);
    cell_t *a_cells = destination->A.grid.cells;
    cell_t *b_cells = destination->B.grid.cells;
    const size_t a_bytes = cells_bytes(&view.A.grid);
    const size_t b_bytes = cells_bytes(&view.B.grid);
    *destination = view;
    destination->A.grid.cells = a_cells;
    destination->B.grid.cells = b_cells;
    memcpy(a_cells, view.A.grid.cells, a_bytes);
    memcpy(b_cells, view.B.grid.cells, b_bytes);
}

static int init_named_sems(ipc_handle_t *ipc) {
    for (int i = 0; i < IPC_SEM_COUNT; ++i) {
        sem_unlink(ipc->shared->sem_names[i]);
        ipc->named_sems[i] = sem_open(ipc->shared->sem_names[i],
                                      O_CREAT | O_EXCL, 0600, 0);
        if (ipc->named_sems[i] == SEM_FAILED) return -1;
    }
    return 0;
}

int ipc_create(ipc_handle_t *ipc, const config_t *cfg, ipc_mode_t mode,
               const char *session) {
    if (!ipc || !cfg || (mode != IPC_SEM_NAMED && mode != IPC_SEM_UNNAMED)) return -1;
    clear_handle(ipc);

    char generated[32];
    if (!session) {
        (void)snprintf(generated, sizeof(generated), "p%ld", (long)getpid());
        session = generated;
    }
    if (!ipc_session_valid(session)) { errno = EINVAL; return -1; }

    const size_t a_bytes = cells_bytes(&cfg->A.grid);
    const size_t b_bytes = cells_bytes(&cfg->B.grid);
    size_t off_a = align_up(sizeof(ipc_shared_t), _Alignof(cell_t));
    size_t off_b = off_a + a_bytes;
    size_t total = off_b + b_bytes;

    ipc->mode = mode;
    ipc->owner = 1;
    (void)snprintf(ipc->shm_name, sizeof(ipc->shm_name), "/idz3_shm_%s", session);
    ipc->shm_fd = shm_open(ipc->shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (ipc->shm_fd < 0) return -1;
    if (ftruncate(ipc->shm_fd, (off_t)total) < 0) goto fail;

    ipc->shared = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->shared == MAP_FAILED) { ipc->shared = NULL; goto fail; }
    ipc->mapping_size = total;
    memset(ipc->shared, 0, total);
    ipc_shared_t *s = ipc->shared;
    s->magic = IPC_MAGIC;
    s->version = IPC_VERSION;
    s->mapping_size = total;
    s->cells_a_offset = off_a;
    s->cells_b_offset = off_b;
    s->mode = mode;
    strncpy(s->session, session, sizeof(s->session) - 1);
    strncpy(s->shm_name, ipc->shm_name, sizeof(s->shm_name) - 1);
    for (int i = 0; i < IPC_SEM_COUNT; ++i) {
        (void)snprintf(s->sem_names[i], sizeof(s->sem_names[i]),
                       "/idz3_%s_%s", session, sem_suffixes[i]);
    }

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) goto fail;
    int attr_ok = 1;
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) attr_ok = 0;
#if defined(__linux__)
    if (attr_ok && pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0) attr_ok = 0;
#endif
    if (!attr_ok || pthread_mutex_init(&s->state_mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        goto fail;
    }
    pthread_mutexattr_destroy(&attr);
    ipc->mutex_initialized = 1;

    s->model = *cfg;
    s->model.A.grid.cells = NULL;
    s->model.B.grid.cells = NULL;
    memcpy((char *)s + off_a, cfg->A.grid.cells, a_bytes);
    memcpy((char *)s + off_b, cfg->B.grid.cells, b_bytes);
    s->current_tick = -1;
    s->side_processed_tick[0] = -1;
    s->side_processed_tick[1] = -1;

    if (mode == IPC_SEM_UNNAMED) {
        int made = 0;
        for (; made < IPC_SEM_COUNT; ++made) {
            if (sem_init(&s->unnamed_sems[made], 1, 0) < 0) break;
            ipc->unnamed_sem_count++;
        }
        if (made != IPC_SEM_COUNT) goto fail_mutex;
    } else if (init_named_sems(ipc) < 0) {
        goto fail_mutex;
    }
    return 0;

fail_mutex:
fail:
    ipc_destroy(ipc);
    return -1;
}

int ipc_attach(ipc_handle_t *ipc, const char *session) {
    if (!ipc_session_valid(session)) { errno = EINVAL; return -1; }
    clear_handle(ipc);
    (void)snprintf(ipc->shm_name, sizeof(ipc->shm_name), "/idz3_shm_%s", session);
    ipc->shm_fd = shm_open(ipc->shm_name, O_RDWR, 0600);
    if (ipc->shm_fd < 0) return -1;
    struct stat st;
    if (fstat(ipc->shm_fd, &st) < 0 || st.st_size < (off_t)sizeof(ipc_shared_t)) goto fail;
    ipc->mapping_size = (size_t)st.st_size;
    ipc->shared = mmap(NULL, ipc->mapping_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ipc->shm_fd, 0);
    if (ipc->shared == MAP_FAILED) { ipc->shared = NULL; goto fail; }
    if (ipc->shared->magic != IPC_MAGIC || ipc->shared->version != IPC_VERSION ||
        ipc->shared->mapping_size != ipc->mapping_size ||
        ipc->shared->mode != IPC_SEM_NAMED) {
        errno = EPROTO;
        goto fail;
    }
    ipc->mode = IPC_SEM_NAMED;
    ipc->mutex_initialized = 1;
    for (int i = 0; i < IPC_SEM_COUNT; ++i) {
        ipc->named_sems[i] = sem_open(ipc->shared->sem_names[i], 0);
        if (ipc->named_sems[i] == SEM_FAILED) goto fail;
    }
    return 0;
fail:
    ipc_close(ipc);
    return -1;
}

void ipc_close(ipc_handle_t *ipc) {
    if (!ipc) return;
    for (int i = 0; i < IPC_SEM_COUNT; ++i) {
        if (ipc->named_sems[i] != SEM_FAILED && ipc->named_sems[i] != NULL) {
            sem_close(ipc->named_sems[i]);
            ipc->named_sems[i] = SEM_FAILED;
        }
    }
    if (ipc->shared && ipc->shared != MAP_FAILED) {
        munmap(ipc->shared, ipc->mapping_size);
        ipc->shared = NULL;
    }
    if (ipc->shm_fd >= 0) {
        close(ipc->shm_fd);
        ipc->shm_fd = -1;
    }
}

void ipc_destroy(ipc_handle_t *ipc) {
    if (!ipc) return;
    if (ipc->owner) {
        if (ipc->shared) {
            if (ipc->mode == IPC_SEM_UNNAMED) {
                for (int i = 0; i < ipc->unnamed_sem_count; ++i)
                    (void)sem_destroy(&ipc->shared->unnamed_sems[i]);
            } else {
                for (int i = 0; i < IPC_SEM_COUNT; ++i)
                    (void)sem_unlink(ipc->shared->sem_names[i]);
            }
            if (ipc->mutex_initialized)
                (void)pthread_mutex_destroy(&ipc->shared->state_mutex);
        }
        if (ipc->shm_name[0]) (void)shm_unlink(ipc->shm_name);
    }
    ipc_close(ipc);
}
