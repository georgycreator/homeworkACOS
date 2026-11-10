#define _POSIX_C_SOURCE 200809L
#include "process_simulation.h"
#include "side.h"
#include "signals.h"
#include "log.h"
#include "colors.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKER_COUNT IPC_WORKER_COUNT
#define ALL_WORKERS_MASK IPC_ALL_WORKERS_MASK
#define POLL_MS 5
#define READY_TIMEOUT_MS 30000

typedef struct {
    int last_fired_tick;
    int next_area_tick;
    struct timespec next_action;
} gunner_state_t;

static const ipc_sem_index_t side_tick_sem[2] = {IPC_SIDE_TICK_A, IPC_SIDE_TICK_B};
static const ipc_sem_index_t side_ack_sem[2] = {IPC_SIDE_ACK_A, IPC_SIDE_ACK_B};
static const ipc_sem_index_t fire_tick_sem[2] = {IPC_FIRE_TICK_A, IPC_FIRE_TICK_B};
static const ipc_sem_index_t fire_ack_sem[2] = {IPC_FIRE_ACK_A, IPC_FIRE_ACK_B};
static const char *const side_names[2] = {"side-A", "side-B"};
static const char *const artillery_names[2] = {"artillery-A", "artillery-B"};

static side_t *side_by_index(config_t *cfg, int i) { return i == 0 ? &cfg->A : &cfg->B; }
static side_t *enemy_by_index(config_t *cfg, int i) { return i == 0 ? &cfg->B : &cfg->A; }
static int check_worker_health(ipc_handle_t *ipc);
static int wait_sem_coordinator(ipc_handle_t *ipc, ipc_sem_index_t sem_index);
static void set_stopping(ipc_handle_t *ipc, simulation_stop_reason_t reason);

static int time_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static void sleep_ms(long ms, int watch_signals) {
    while (ms > 0) {
        long part = ms > 10 ? 10 : ms;
        struct timespec req = {part / 1000, (part % 1000) * 1000000L};
        while (nanosleep(&req, &req) < 0 && errno == EINTR)
            if (watch_signals && signals_stop_requested()) return;
        ms -= part;
        if (watch_signals && signals_stop_requested()) return;
    }
}

static void wake_all_workers(ipc_handle_t *ipc) {
    static const ipc_sem_index_t sems[] = {
        IPC_SIDE_TICK_A, IPC_SIDE_TICK_B, IPC_FIRE_TICK_A, IPC_FIRE_TICK_B,
        IPC_READY, IPC_SIDE_ACK_A, IPC_SIDE_ACK_B, IPC_FIRE_ACK_A, IPC_FIRE_ACK_B
    };
    for (size_t i = 0; i < sizeof(sems) / sizeof(sems[0]); ++i)
        (void)ipc_sem_post(ipc, sems[i]);
}

static int register_worker(ipc_handle_t *ipc, int role_index, const char *role) {
    if (role_index < 0 || role_index >= WORKER_COUNT) { errno = EINVAL; return -1; }
    if (ipc_lock(ipc->shared) < 0) return -1;

    const unsigned int bit = 1u << (unsigned int)role_index;
    int duplicate_pid = 0;
    for (int i = 0; i < WORKER_COUNT; ++i)
        if (ipc->shared->pids[i] == getpid()) duplicate_pid = 1;

    if (ipc->shared->stopping || (ipc->shared->registered_mask & bit) || duplicate_pid) {
        log_printf("[ERROR] Повторная/недопустимая регистрация роли=%s, PID=%ld; сессия остановлена.\n",
                   role, (long)getpid());
        ipc->shared->fatal_error = ipc->shared->stopping = 1;
        ipc->shared->stop_reason = SIM_STOP_ERROR;
        ipc_unlock(ipc->shared);
        wake_all_workers(ipc);
        errno = EEXIST;
        return -1;
    }

    ipc->shared->pids[role_index] = getpid();
    ipc->shared->registered_mask |= bit;
    ++ipc->shared->ready_count;
    log_printf("[ПРОЦЕСС] роль=%s, PID=%ld: подключён к сессии %s (%d/%d)\n",
               role, (long)getpid(), ipc->shared->session,
               ipc->shared->ready_count, WORKER_COUNT);
    ipc_unlock(ipc->shared);
    return ipc_sem_post(ipc, IPC_READY);
}

static void mark_worker_finished(ipc_handle_t *ipc, int role_index, const char *role) {
    if (ipc_lock(ipc->shared) < 0) return;
    const unsigned int bit = 1u << (unsigned int)role_index;
    if ((ipc->shared->registered_mask & bit) && !(ipc->shared->finished_mask & bit)) {
        log_printf("[ПРОЦЕСС] роль=%s, PID=%ld: завершён\n", role, (long)getpid());
        ipc->shared->finished_mask |= bit;
        ++ipc->shared->finished_workers;
    }
    ipc_unlock(ipc->shared);
}

static long random_reaction_delay(artillery_t *art) {
    long delay = art->reaction_ms;
    if (art->reaction_jitter_ms > 0)
        delay += artillery_random_bounded(art, art->reaction_jitter_ms + 1);
    return delay < 0 ? 0 : delay;
}

static int try_fire_locked(ipc_shared_t *shared, config_t *cfg, int index,
                           gunner_state_t *state, int respect_time) {
    side_t *self = side_by_index(cfg, index), *enemy = enemy_by_index(cfg, index);
    const int tick = shared->current_tick;
    side_check_done(self, enemy);
    if (!self->active || shared->side_processed_tick[0] != tick ||
        shared->side_processed_tick[1] != tick || tick == state->last_fired_tick) return 0;

    if (respect_time) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || time_cmp(&now, &state->next_action) < 0)
            return 0;
    }

    const int regular = artillery_can_fire(&self->art);
    const int area = artillery_can_area_strike(&self->art);
    if (!regular && !area) { side_check_done(self, enemy); return 0; }

    const int area_due = self->art.area_every > 0 && tick >= state->next_area_tick && area;
    const int use_area = area_due || (!regular && area);
    const int changed = use_area ? side_area_strike(self, enemy, tick) : side_fire(self, enemy, tick);
    if (use_area && self->art.area_every > 0 && tick >= state->next_area_tick) {
        do { state->next_area_tick += self->art.area_every; }
        while (state->next_area_tick <= tick);
    }

    state->last_fired_tick = tick;
    if (respect_time) {
        (void)clock_gettime(CLOCK_MONOTONIC, &state->next_action);
        const long delay = random_reaction_delay(&self->art);
        state->next_action.tv_sec += delay / 1000;
        state->next_action.tv_nsec += (delay % 1000) * 1000000L;
        if (state->next_action.tv_nsec >= 1000000000L) {
            ++state->next_action.tv_sec;
            state->next_action.tv_nsec -= 1000000000L;
        }
    }
    side_check_done(self, enemy);
    if (changed) {
        log_printf("\n%s>>> Состояние территорий после изменений:%s\n", C_BOLD, C_RESET);
        grid_print(&cfg->A.grid, "Территория A");
        grid_print(&cfg->B.grid, "Территория B");
    }
    return 1;
}

/* Общий цикл государства и артиллерии; игровая логика остаётся в side.c/artillery.c. */
static int worker_loop(ipc_handle_t *ipc, int index, int artillery, int independent) {
    colors_init();
    const int role_index = artillery ? 2 + index : index;
    const char *role = artillery ? artillery_names[index] : side_names[index];
    config_t initial;
    ipc_model_view(ipc->shared, &initial);
    if (independent && log_open_append(initial.log_path) < 0) {
        fprintf(stderr, "Не удалось открыть общий лог: %s\n", strerror(errno));
        return 1;
    }
    if (register_worker(ipc, role_index, role) < 0) {
        if (independent) log_close();
        return 1;
    }

    gunner_state_t gunner = {.last_fired_tick = -1};
    const int fast = artillery && initial.tick_ms <= 0;
    if (artillery) {
        gunner.next_area_tick = side_by_index(&initial, index)->art.area_every;
        (void)clock_gettime(CLOCK_MONOTONIC, &gunner.next_action);
    }

    int error = 0;
    for (;;) {
        if (!artillery || fast) {
            const ipc_sem_index_t sem = artillery ? fire_tick_sem[index] : side_tick_sem[index];
            if (ipc_sem_wait(ipc, sem) < 0) { error = 1; break; }
        }
        if (ipc_lock(ipc->shared) < 0) { error = 1; break; }
        const int stopping = ipc->shared->stopping;
        if (!stopping) {
            config_t model;
            ipc_model_view(ipc->shared, &model);
            if (artillery) {
                if (try_fire_locked(ipc->shared, &model, index, &gunner, !fast))
                    ipc_model_save(ipc->shared, &model);
            } else {
                side_t *self = side_by_index(&model, index);
                side_t *enemy = enemy_by_index(&model, index);
                const int tick = ipc->shared->current_tick;
                if (self->art.replenish_every > 0 &&
                    (tick + 1) % self->art.replenish_every == 0) side_replenish(self);
                side_check_done(self, enemy);
                ipc_model_save(ipc->shared, &model);
                ipc->shared->side_processed_tick[index] = tick;
            }
        }
        ipc_unlock(ipc->shared);
        if (stopping) break;

        if (!artillery) {
            if (ipc_sem_post(ipc, side_ack_sem[index]) < 0) { error = 1; break; }
        } else if (fast) {
            if (ipc_sem_post(ipc, fire_ack_sem[index]) < 0) { error = 1; break; }
        } else {
            sleep_ms(POLL_MS, 0);
        }
    }

    mark_worker_finished(ipc, role_index, role);
    if (independent) log_close();
    return error;
}

static int pid_is_alive(pid_t pid) {
    if (pid <= 0) return 0;
#ifdef __linux__
    char path[64], data[512];
    (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t n = read(fd, data, sizeof(data) - 1);
        close(fd);
        if (n > 0) {
            data[n] = '\0';
            char *end = strrchr(data, ')');
            if (end && end[1] == ' ' && (end[2] == 'Z' || end[2] == 'X')) return 0;
        }
    }
#endif
    return kill(pid, 0) == 0 || errno == EPERM || errno != ESRCH;
}

static void mark_model_fatal(ipc_handle_t *ipc, const char *message) {
    log_printf("[ERROR] Аварийное завершение процессной модели: %s\n", message);
    if (ipc_lock(ipc->shared) == 0) {
        ipc->shared->fatal_error = ipc->shared->stopping = 1;
        ipc->shared->stop_reason = SIM_STOP_ERROR;
        ipc_unlock(ipc->shared);
    }
    wake_all_workers(ipc);
}

static int check_worker_health(ipc_handle_t *ipc) {
    pid_t pids[WORKER_COUNT];
    unsigned int registered, finished;
    int stopping, fatal;
    if (ipc_lock(ipc->shared) < 0) {
        log_printf("[ERROR] Не удалось получить межпроцессную блокировку; возможна авария владельца mutex.\n");
        return -1;
    }
    registered = ipc->shared->registered_mask;
    finished = ipc->shared->finished_mask;
    stopping = ipc->shared->stopping;
    fatal = ipc->shared->fatal_error;
    memcpy(pids, ipc->shared->pids, sizeof(pids));
    ipc_unlock(ipc->shared);
    if (fatal) return -1;
    if (stopping) return 1;

    for (int i = 0; i < WORKER_COUNT; ++i) {
        const unsigned int bit = 1u << (unsigned int)i;
        if (!(registered & bit)) continue;
        if ((finished & bit) || !pid_is_alive(pids[i])) {
            char message[160];
            (void)snprintf(message, sizeof(message), "участник роли %d (PID=%ld) завершился до остановки",
                           i, (long)pids[i]);
            mark_model_fatal(ipc, message);
            return -1;
        }
        int status;
        const pid_t waited = waitpid(pids[i], &status, WNOHANG);
        if (waited == pids[i]) {
            char message[160];
            (void)snprintf(message, sizeof(message), "роль %d (PID=%ld) завершилась раньше остановки",
                           i, (long)pids[i]);
            mark_model_fatal(ipc, message);
            return -1;
        }
        if (waited < 0 && errno != ECHILD && errno != EINTR) {
            mark_model_fatal(ipc, "ошибка waitpid() при мониторинге участника");
            return -1;
        }
    }
    return 0;
}

static int wait_tick_duration(ipc_handle_t *ipc, long duration_ms) {
    long elapsed = 0;
    while (elapsed < duration_ms) {
        if (signals_stop_requested()) return 1;
        const int health = check_worker_health(ipc);
        if (health != 0) return health;
        const long part = duration_ms - elapsed > 10 ? 10 : duration_ms - elapsed;
        sleep_ms(part, 1);
        if (signals_stop_requested()) return 1;
        elapsed += part;
    }
    return 0;
}

static int wait_for_workers_ready(ipc_handle_t *ipc, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 250) {
        if (ipc_lock(ipc->shared) < 0) return -1;
        const unsigned int registered = ipc->shared->registered_mask;
        const int fatal = ipc->shared->fatal_error, stopping = ipc->shared->stopping;
        ipc_unlock(ipc->shared);
        if (fatal) return -1;
        if (stopping) return 1;
        if (registered == ALL_WORKERS_MASK) return 0;
        const int health = check_worker_health(ipc);
        if (health != 0) return health;
        if (ipc_sem_timedwait_ms(ipc, IPC_READY, 250) < 0) return -1;
        if (signals_stop_requested()) return 1;
    }
    errno = ETIMEDOUT;
    return -1;
}

static int refresh_finished(ipc_handle_t *ipc) {
    if (ipc_lock(ipc->shared) < 0) return -1;
    config_t model;
    ipc_model_view(ipc->shared, &model);
    side_check_done(&model.A, &model.B);
    side_check_done(&model.B, &model.A);
    ipc_model_save(ipc->shared, &model);
    const int done = !model.A.active && !model.B.active;
    const int fatal = ipc->shared->fatal_error;
    ipc_unlock(ipc->shared);
    return fatal ? -1 : done;
}

static int start_tick(ipc_handle_t *ipc, int tick, int fast) {
    if (ipc_lock(ipc->shared) < 0) return -1;
    if (ipc->shared->stopping) { ipc_unlock(ipc->shared); return -1; }
    ipc->shared->current_tick = tick;
    ipc->shared->side_processed_tick[0] = ipc->shared->side_processed_tick[1] = -1;
    log_printf("\n%s========== ТАКТ %d ==========%s\n", C_BOLD, tick, C_RESET);
    ipc_unlock(ipc->shared);

    if (ipc_sem_post(ipc, side_tick_sem[0]) < 0 || ipc_sem_post(ipc, side_tick_sem[1]) < 0 ||
        wait_sem_coordinator(ipc, side_ack_sem[0]) != 0 ||
        wait_sem_coordinator(ipc, side_ack_sem[1]) != 0) return -1;
    if (fast && (ipc_sem_post(ipc, fire_tick_sem[0]) < 0 ||
                 ipc_sem_post(ipc, fire_tick_sem[1]) < 0 ||
                 wait_sem_coordinator(ipc, fire_ack_sem[0]) != 0 ||
                 wait_sem_coordinator(ipc, fire_ack_sem[1]) != 0)) return -1;
    if (check_worker_health(ipc) < 0) { set_stopping(ipc, SIM_STOP_ERROR); return -1; }
    return 0;
}

static int wait_sem_coordinator(ipc_handle_t *ipc, ipc_sem_index_t sem_index) {
    for (;;) {
        const int rc = ipc_sem_timedwait_ms(ipc, sem_index, 250);
        if (rc == 0) return 0;
        if (rc < 0) return -1;
        if (signals_stop_requested()) return 1;
        const int health = check_worker_health(ipc);
        if (health < 0) { set_stopping(ipc, SIM_STOP_ERROR); return -1; }
        if (health > 0) return 1;
    }
}

static void set_stopping(ipc_handle_t *ipc, simulation_stop_reason_t reason) {
    if (ipc_lock(ipc->shared) == 0) {
        ipc->shared->stopping = 1;
        ipc->shared->stop_reason = (int)(ipc->shared->fatal_error ? SIM_STOP_ERROR : reason);
        ipc_unlock(ipc->shared);
    }
    wake_all_workers(ipc);
}

static simulation_stop_reason_t coordinate(ipc_handle_t *ipc) {
    config_t model;
    ipc_model_view(ipc->shared, &model);
    const int fast = model.tick_ms <= 0;
    simulation_stop_reason_t reason = SIM_STOP_NATURAL;
    log_printf("[ПРОЦЕСС] роль=coordinator, PID=%ld: управление моделью\n", (long)getpid());
    log_printf("[IPC] Shared Memory=%s, семафоры=%s\n", ipc->shared->shm_name,
               ipc->mode == IPC_SEM_NAMED ? "именованные POSIX" : "неименованные POSIX (pshared=1)");
    if (fast) log_printf("[РЕЖИМ] tick_ms=0: быстрый режим, один выстрел каждой артиллерии за такт\n");

    for (int tick = 0;; ++tick) {
        if (signals_stop_requested()) {
            reason = SIM_STOP_SIGNAL;
            log_printf("\n%s[STOP]%s Сигнал %d: остановка на такте %d\n",
                       C_RED, C_RESET, signals_last_signal(), tick);
            break;
        }
        if (start_tick(ipc, tick, fast) < 0) {
            reason = signals_stop_requested() ? SIM_STOP_SIGNAL : SIM_STOP_ERROR;
            break;
        }
        int finished = refresh_finished(ipc);
        if (finished < 0) { reason = SIM_STOP_ERROR; break; }
        if (!fast && !finished) {
            const int wait_rc = wait_tick_duration(ipc, model.tick_ms);
            if (wait_rc != 0) {
                reason = wait_rc > 0 && signals_stop_requested() ? SIM_STOP_SIGNAL : SIM_STOP_ERROR;
                if (reason == SIM_STOP_SIGNAL)
                    log_printf("\n%s[STOP]%s Прерывание пользователем (сигнал %d)\n",
                               C_RED, C_RESET, signals_last_signal());
                break;
            }
            finished = refresh_finished(ipc);
            if (finished < 0) { reason = SIM_STOP_ERROR; break; }
        }
        if (ipc_lock(ipc->shared) < 0) { reason = SIM_STOP_ERROR; break; }
        ipc->shared->ticks_completed = tick + 1;
        const int completed = ipc->shared->ticks_completed, fatal = ipc->shared->fatal_error;
        ipc_unlock(ipc->shared);
        if (fatal) { reason = SIM_STOP_ERROR; break; }
        if (finished) {
            reason = SIM_STOP_NATURAL;
            log_printf("\n%s[STOP]%s Обе стороны прекратили огонь на такте %d\n", C_RED, C_RESET, tick);
            break;
        }
        if (model.max_ticks > 0 && completed >= model.max_ticks) {
            reason = SIM_STOP_LIMIT;
            log_printf("\n%s[STOP]%s Лимит тактов: %d\n", C_RED, C_RESET, model.max_ticks);
            break;
        }
    }
    set_stopping(ipc, reason);
    if (ipc_lock(ipc->shared) == 0) {
        if (ipc->shared->fatal_error) reason = SIM_STOP_ERROR;
        ipc_unlock(ipc->shared);
    }
    return reason;
}

static void print_verdict(const side_t *a, const side_t *b) {
    log_printf("\n--- Вердикт ---\nПотери: %s — %" PRId64 ", %s — %" PRId64 "\n",
               a->name, a->lost_cost, b->name, b->lost_cost);
    if (a->grid.alive_targets > 0 && b->grid.alive_targets <= 0)
        log_printf("Победа: %s (у противника уничтожены все цели)\n", a->name);
    else if (b->grid.alive_targets > 0 && a->grid.alive_targets <= 0)
        log_printf("Победа: %s (у противника уничтожены все цели)\n", b->name);
    else if (a->lost_cost != b->lost_cost)
        log_printf("Преимущество: %s (потери меньше на %" PRId64 ")\n",
                   a->lost_cost < b->lost_cost ? a->name : b->name,
                   llabs(a->lost_cost - b->lost_cost));
    else log_printf("Ничья по стоимости потерь\n");
}

static void print_final_report(ipc_handle_t *ipc, simulation_stop_reason_t reason) {
    if (ipc_lock(ipc->shared) < 0) return;
    config_t model;
    ipc_model_view(ipc->shared, &model);
    log_printf("\n%s%s========== ИТОГИ IDZ3 ==========%s\nВсего тактов: %d\n",
               C_BOLD, C_CYAN, C_RESET, ipc->shared->ticks_completed);
    switch (reason) {
        case SIM_STOP_NATURAL: log_printf("Причина: обе стороны прекратили огонь\n"); break;
        case SIM_STOP_LIMIT: log_printf("Причина: достигнут лимит тактов\n"); break;
        case SIM_STOP_SIGNAL: log_printf("Причина: прерывание пользователем\n"); break;
        case SIM_STOP_ERROR: log_printf("Причина: ошибка процессной модели\n"); break;
    }
    side_print_stats(&model.A);
    side_print_stats(&model.B);
    print_verdict(&model.A, &model.B);
    log_printf("\n--- Финальное состояние ---\n");
    grid_print(&model.A.grid, "Территория A");
    grid_print(&model.B.grid, "Территория B");
    log_printf("\n%s%s========== ЗАВЕРШЕНИЕ ==========%s\n", C_BOLD, C_GREEN, C_RESET);
    ipc_unlock(ipc->shared);
}

static void wait_parent_children(pid_t pids[WORKER_COUNT]) {
    for (int i = 0; i < WORKER_COUNT; ++i) {
        if (pids[i] <= 0) continue;
        int status;
        while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR) { }
    }
}

simulation_stop_reason_t simulation_run_processes(config_t *cfg, int *ticks_done,
                                                  ipc_mode_t mode) {
    if (ticks_done) *ticks_done = 0;
    ipc_handle_t ipc;
    if (ipc_create(&ipc, cfg, mode, NULL) < 0) {
        log_printf("[ERROR] Не удалось создать POSIX Shared Memory/семафоры: %s\n", strerror(errno));
        return SIM_STOP_ERROR;
    }
    pid_t pids[WORKER_COUNT] = {0};
    for (int role = 0; role < WORKER_COUNT; ++role) {
        const pid_t pid = fork();
        if (pid < 0) {
            log_printf("[ERROR] fork() для участника %d: %s\n", role, strerror(errno));
            set_stopping(&ipc, SIM_STOP_ERROR);
            for (int j = 0; j < role; ++j) if (pids[j] > 0) (void)kill(pids[j], SIGTERM);
            wait_parent_children(pids);
            ipc_model_copy_back(ipc.shared, cfg);
            ipc_destroy(&ipc);
            return SIM_STOP_ERROR;
        }
        if (pid == 0) {
            (void)signal(SIGINT, SIG_IGN);
            (void)signal(SIGTERM, SIG_IGN);
            const int rc = role < 2 ? worker_loop(&ipc, role, 0, 0)
                                    : worker_loop(&ipc, role - 2, 1, 0);
            _exit(rc == 0 ? 0 : 1);
        }
        pids[role] = pid;
    }

    const int ready = wait_for_workers_ready(&ipc, READY_TIMEOUT_MS);
    simulation_stop_reason_t reason;
    if (ready < 0) {
        log_printf("[ERROR] Не все дочерние процессы зарегистрировались\n");
        reason = SIM_STOP_ERROR;
        set_stopping(&ipc, reason);
    } else if (ready > 0) {
        reason = SIM_STOP_SIGNAL;
        set_stopping(&ipc, reason);
    } else reason = coordinate(&ipc);

    wait_parent_children(pids);
    print_final_report(&ipc, reason);
    if (ticks_done && ipc_lock(ipc.shared) == 0) {
        *ticks_done = ipc.shared->ticks_completed;
        ipc_unlock(ipc.shared);
    }
    ipc_model_copy_back(ipc.shared, cfg);
    ipc_destroy(&ipc);
    return reason;
}

static int wait_independent_workers(ipc_handle_t *ipc, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 100) {
        if (ipc_lock(ipc->shared) < 0) return -1;
        const unsigned int registered = ipc->shared->registered_mask;
        unsigned int finished = ipc->shared->finished_mask;
        const int stopping = ipc->shared->stopping;
        pid_t pids[WORKER_COUNT];
        memcpy(pids, ipc->shared->pids, sizeof(pids));
        ipc_unlock(ipc->shared);
        if (stopping)
            for (int i = 0; i < WORKER_COUNT; ++i)
                if ((registered & (1u << (unsigned int)i)) && !pid_is_alive(pids[i]))
                    finished |= 1u << (unsigned int)i;
        if ((finished & registered) == registered && (stopping || registered == ALL_WORKERS_MASK))
            return 0;
        sleep_ms(100, 0);
    }
    return -1;
}

static void force_stop_unfinished_independent_workers(ipc_handle_t *ipc) {
    pid_t pids[WORKER_COUNT];
    unsigned int registered, finished;
    if (ipc_lock(ipc->shared) < 0) return;
    memcpy(pids, ipc->shared->pids, sizeof(pids));
    registered = ipc->shared->registered_mask;
    finished = ipc->shared->finished_mask;
    ipc_unlock(ipc->shared);
    for (int i = 0; i < WORKER_COUNT; ++i)
        if ((registered & (1u << (unsigned int)i)) && !(finished & (1u << (unsigned int)i)) && pids[i] > 0)
            (void)kill(pids[i], SIGTERM);
    sleep_ms(200, 0);
    for (int i = 0; i < WORKER_COUNT; ++i)
        if ((registered & (1u << (unsigned int)i)) && !(finished & (1u << (unsigned int)i)) &&
            pids[i] > 0 && pid_is_alive(pids[i])) (void)kill(pids[i], SIGKILL);
}

int independent_worker_main(const char *session, int is_artillery, int side_index) {
    ipc_handle_t ipc;
    if (ipc_attach(&ipc, session) < 0) {
        fprintf(stderr, "Не удалось подключиться к сессии '%s': %s\n", session, strerror(errno));
        return 1;
    }
    (void)signal(SIGINT, SIG_IGN);
    (void)signal(SIGTERM, SIG_IGN);
    const int status = worker_loop(&ipc, side_index, is_artillery, 1);
    ipc_close(&ipc);
    return status;
}

int independent_coordinator_main(const char *session) {
    ipc_handle_t ipc;
    if (ipc_attach(&ipc, session) < 0) {
        fprintf(stderr, "Не удалось подключиться к сессии '%s': %s\n", session, strerror(errno));
        return 1;
    }
    if (signals_install() < 0) { ipc_close(&ipc); return 1; }
    config_t model;
    ipc_model_view(ipc.shared, &model);
    if (log_open_append(model.log_path) < 0) {
        fprintf(stderr, "Не удалось открыть лог '%s'\n", model.log_path);
        ipc_close(&ipc);
        return 1;
    }
    if (ipc_lock(ipc.shared) == 0) {
        log_printf("\n%s%s========== СТАРТ НЕЗАВИСИМОЙ МОДЕЛИ ==========%s\n",
                   C_BOLD, C_GREEN, C_RESET);
        config_print(&model);
        grid_print(&model.A.grid, "Территория A");
        grid_print(&model.B.grid, "Территория B");
        ipc_unlock(ipc.shared);
    }

    const int ready = wait_for_workers_ready(&ipc, READY_TIMEOUT_MS);
    simulation_stop_reason_t reason;
    if (ready == 0) {
        log_printf("[IPC] Все четыре независимых участника подключены. Начинаем моделирование.\n");
        reason = coordinate(&ipc);
    } else {
        reason = signals_stop_requested() ? SIM_STOP_SIGNAL : SIM_STOP_ERROR;
        log_printf("[ERROR] Не удалось зарегистрировать четыре уникальных участника (тайм-аут/ошибка/сигнал).\n");
        set_stopping(&ipc, reason);
    }
    if (wait_independent_workers(&ipc, 10000) < 0) {
        log_printf("[ERROR] Часть независимых участников не завершилась вовремя; принудительная остановка.\n");
        force_stop_unfinished_independent_workers(&ipc);
        reason = SIM_STOP_ERROR;
    }
    print_final_report(&ipc, reason);
    log_close();
    ipc.owner = 1;
    ipc_destroy(&ipc);
    return reason == SIM_STOP_ERROR ? 1 : 0;
}

int independent_init_main(const char *session, const char *config_path) {
    if (!ipc_session_valid(session)) {
        fprintf(stderr, "Некорректный идентификатор сессии: используйте до 31 символа [A-Za-z0-9_-]\n");
        return 1;
    }
    config_t cfg;
    if (config_defaults(&cfg) < 0) { fprintf(stderr, "Не удалось инициализировать конфигурацию\n"); return 1; }
    if (config_path && config_load(&cfg, config_path) < 0) {
        fprintf(stderr, "Ошибка загрузки конфигурации: %s\n", config_path);
        config_free(&cfg);
        return 1;
    }
    artillery_seed(&cfg.A.art, cfg.seed ^ 0x9E3779B9u);
    artillery_seed(&cfg.B.art, cfg.seed ^ 0x85EBCA6Bu);
    if (cfg.log_path[0] != '/') {
        char cwd[192], absolute_log[sizeof(cfg.log_path)];
        if (!getcwd(cwd, sizeof(cwd)) ||
            snprintf(absolute_log, sizeof(absolute_log), "%s/%s", cwd, cfg.log_path) >= (int)sizeof(absolute_log)) {
            fprintf(stderr, "Не удалось сформировать абсолютный путь к лог-файлу\n");
            config_free(&cfg);
            return 1;
        }
        strncpy(cfg.log_path, absolute_log, sizeof(cfg.log_path) - 1);
        cfg.log_path[sizeof(cfg.log_path) - 1] = '\0';
    }
    if (log_open(cfg.log_path) < 0) {
        fprintf(stderr, "Не удалось создать лог-файл '%s': %s\n", cfg.log_path, strerror(errno));
        config_free(&cfg);
        return 1;
    }
    log_printf("[INIT] Создание независимой сессии IDZ3: %s\n", session);
    log_close();

    ipc_handle_t ipc;
    if (ipc_create(&ipc, &cfg, IPC_SEM_NAMED, session) < 0) {
        fprintf(stderr, "Не удалось создать сессию '%s': %s\n", session, strerror(errno));
        config_free(&cfg);
        return 1;
    }
    printf("Создана независимая сессия IDZ3: %s\nShared Memory: %s\nЛог: %s\n",
           session, ipc.shared->shm_name, cfg.log_path);
    printf("Теперь запустите IDZ3_coordinator, два IDZ3_side и два IDZ3_artillery.\n");
    ipc_close(&ipc);
    config_free(&cfg);
    return 0;
}

int independent_cleanup_main(const char *session) {
    ipc_handle_t ipc;
    if (ipc_attach(&ipc, session) < 0) {
        fprintf(stderr, "Не удалось открыть сессию '%s': %s\n", session, strerror(errno));
        return 1;
    }
    set_stopping(&ipc, SIM_STOP_SIGNAL);
    if (wait_independent_workers(&ipc, 5000) < 0) force_stop_unfinished_independent_workers(&ipc);
    ipc.owner = 1;
    ipc_destroy(&ipc);
    printf("IPC-ресурсы сессии '%s' удалены.\n", session);
    return 0;
}