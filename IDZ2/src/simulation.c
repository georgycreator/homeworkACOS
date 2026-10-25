#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include "side.h"
#include "log.h"
#include "signals.h"
#include "colors.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Участники модели (пять потоков):
 *   side_worker[A,B]      — потоки государств: в начале такта пополняют запас
 *                           и проверяют условия прекращения огня СВОЕЙ стороны;
 *   artillery_worker[A,B] — огневые подразделения: выбирают координаты и стреляют;
 *   координатор           — основной поток: ведёт такты, принимает решение
 *                           о глобальном завершении и обрабатывает сигналы.
 *
 * Синхронизация.
 *   mutex       — защищает всё изменяемое состояние модели: выстрел, изменение
 *                 клетки, статистика, флаги active. Каждая операция «проверить
 *                 условия завершения — выстрелить — проверить снова» выполняется
 *                 под одной блокировкой, поэтому завершившая сторона не успевает
 *                 выстрелить ещё раз;
 *   tick_sem[]  — координатор сообщает потоку государства о начале такта;
 *   tick_ack[]  — государство подтверждает, что обслужило такт;
 *   fire_sem[]  — только в быстром режиме (tick_ms=0): координатор разрешает
 *                 огневому подразделению сделать выстрел текущего такта;
 *   fire_ack[]  — подразделение сообщает, что отработало такт;
 *   event_sem   — участник сообщает координатору о событии, чтобы он проверил
 *                 завершение раньше следующего такта.
 *
 * Два режима времени.
 *   tick_ms > 0 — такт длится tick_ms миллисекунд реального времени.
 *                 Огневой поток опрашивает состояние, выдерживает собственную
 *                 задержку реакции и стреляет не чаще одного раза за такт.
 *   tick_ms = 0 — быстрое моделирование без реального времени. Координатор
 *                 не начинает такт N+1, пока оба подразделения не отработали
 *                 такт N (каждое стреляет ровно один раз, если может). Задержки
 *                 реакции в этом режиме не применяются. Раньше такты в этом режиме
 *                 не были связаны с выстрелами, и лимит тактов мог закончиться
 *                 прежде, чем огневые потоки получали процессор.
 */
#define POLL_SLICE_MS      20   // максимальная длительность одного ожидания координатора
#define GUNNER_POLL_MS      5   // период опроса огневого потока в режиме с реальным временем

typedef struct {
    config_t *cfg;
    pthread_mutex_t mutex;
    sem_t event_sem;
    sem_t tick_sem[2];
    sem_t tick_ack[2];
    sem_t fire_sem[2];
    sem_t fire_ack[2];
    int fast;                 // 1 — режим tick_ms=0
    int tick;
    int ticks_completed;
    int side_processed_tick[2];
    int stopping;
    int event_pending;        // event_sem работает как бинарное уведомление
    simulation_stop_reason_t stop_reason;
} simulation_t;

typedef struct {
    simulation_t *sim;
    int side_index;           // 0 — A, 1 — B
} worker_arg_t;

// Собственное состояние огневого потока (не разделяется с другими потоками).
typedef struct {
    int last_fired_tick;          // такт последнего выстрела (один выстрел за такт)
    int next_area_tick;           // ближайший такт, начиная с которого положен залп
    struct timespec next_action;  // момент готовности после задержки реакции
} gunner_state_t;

static side_t *side_by_index(simulation_t *sim, int index) {
    return index == 0 ? &sim->cfg->A : &sim->cfg->B;
}

static side_t *enemy_by_index(simulation_t *sim, int index) {
    return index == 0 ? &sim->cfg->B : &sim->cfg->A;
}

static void notify_coordinator_locked(simulation_t *sim) {
    if (!sim->event_pending) {
        sim->event_pending = 1;
        (void)sem_post(&sim->event_sem);
    }
}

static int sem_wait_retry(sem_t *sem) {
    for (;;) {
        if (sem_wait(sem) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

static void add_milliseconds(struct timespec *ts, long ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

static int timespec_compare(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    return 0;
}

// Сколько миллисекунд осталось до момента end (по CLOCK_MONOTONIC), с округлением вверх.
static long ms_until(const struct timespec *end) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (long long)(end->tv_sec - now.tv_sec) * 1000000000LL +
                   (long long)(end->tv_nsec - now.tv_nsec);
    if (ns <= 0) return 0;
    return (long)((ns + 999999LL) / 1000000LL);
}

static void sleep_ms_interruptible(simulation_t *sim, long milliseconds) {
    // Короткие отрезки сна позволяют потокам быстро увидеть остановку.
    long left = milliseconds;
    if (left <= 0) left = 1;
    while (left > 0) {
        long slice = left > 10 ? 10 : left;
        struct timespec req = { slice / 1000, (slice % 1000) * 1000000L };
        while (nanosleep(&req, &req) < 0 && errno == EINTR) {
            if (signals_stop_requested()) return;
        }
        left -= slice;
        pthread_mutex_lock(&sim->mutex);
        int stopping = sim->stopping;
        pthread_mutex_unlock(&sim->mutex);
        if (stopping || signals_stop_requested()) return;
    }
}

// Проверка завершения от имени координатора. Координатор не является стороной
// конфликта, поэтому именно он обновляет флаги active после выстрелов:
// стрелок меняет только территорию противника, а флаг жертвы выставляет
// либо сама жертва (перед своим следующим выстрелом), либо координатор.
static int refresh_and_check_finished_locked(simulation_t *sim) {
    side_check_done(&sim->cfg->A, &sim->cfg->B);
    side_check_done(&sim->cfg->B, &sim->cfg->A);
    return !sim->cfg->A.active && !sim->cfg->B.active;
}

static void *side_worker(void *opaque) {
    worker_arg_t *arg = (worker_arg_t *)opaque;
    simulation_t *sim = arg->sim;
    int index = arg->side_index;
    side_t *side = side_by_index(sim, index);
    log_printf("[ПОТОК] роль=side, сторона=%s, pthread=%lu: запущен\n",
               side->name, (unsigned long)pthread_self());

    for (;;) {
        if (sem_wait_retry(&sim->tick_sem[index]) < 0) break;

        pthread_mutex_lock(&sim->mutex);
        int stopping = sim->stopping;
        if (!stopping) {
            side_t *self = side_by_index(sim, index);
            side_t *enemy = enemy_by_index(sim, index);
            const int tick = sim->tick;

            // Пополнение выполняется в начале такта, до проверки доступности огня.
            if (self->art.replenish_every > 0 &&
                (tick + 1) % self->art.replenish_every == 0) {
                side_replenish(self);
            }

            // Каждое государство проверяет условия прекращения огня само.
            side_check_done(self, enemy);
            sim->side_processed_tick[index] = tick;
            notify_coordinator_locked(sim);
        }
        pthread_mutex_unlock(&sim->mutex);

        // Подтверждение отправляется и при остановке, чтобы координатор,
        // если он ожидал этот такт, не остался заблокированным.
        (void)sem_post(&sim->tick_ack[index]);
        if (stopping) break;
    }
    log_printf("[ПОТОК] роль=side, сторона=%s, pthread=%lu: завершён\n",
               side->name, (unsigned long)pthread_self());
    return NULL;
}

static long next_reaction_delay_ms(artillery_t *art) {
    long delay = art->reaction_ms;
    if (art->reaction_jitter_ms > 0) {
        delay += artillery_random_bounded(art, art->reaction_jitter_ms + 1);
    }
    return delay < 0 ? 0 : delay;
}

/*
 * Одна попытка выстрела огневого подразделения. Вызывается под sim->mutex.
 * Выполняет не более одного выстрела (обычного или тяжёлого залпа).
 * respect_time = 1 — учитывать задержку реакции (режим с реальным временем).
 * Возвращает 1, если выстрел был сделан.
 */
static int gunner_try_fire_locked(simulation_t *sim, int index,
                                  gunner_state_t *st, int respect_time) {
    // Сигнал может быть уже получен координатором, но он ещё не успел
    // выставить sim->stopping. Не начинаем новый выстрел после запроса остановки.
    if (signals_stop_requested()) return 0;

    side_t *self = side_by_index(sim, index);
    side_t *enemy = enemy_by_index(sim, index);
    const int tick = sim->tick;

    // Условия завершения проверяются непосредственно перед выстрелом
    // под той же блокировкой: после этого выстрел и изменение состояния
    // неделимы относительно второго огневого потока.
    side_check_done(self, enemy);

    const int tick_ready = sim->side_processed_tick[0] == tick &&
                           sim->side_processed_tick[1] == tick;
    if (!self->active || !tick_ready || tick == st->last_fired_tick) return 0;

    if (respect_time) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_compare(&now, &st->next_action) < 0) return 0;
    }

    const int can_regular = artillery_can_fire(&self->art);
    const int can_area = artillery_can_area_strike(&self->art);
    if (!can_regular && !can_area) return 0;

    // Тяжёлый залп становится положен с такта next_area_tick и выполняется
    // при ближайшей возможности: при ненулевой реакции поток может проснуться
    // позже точного такта, и залп не должен пропасть. Если обычных снарядов
    // больше нет, тяжёлый залп — допустимый запасной способ продолжить огонь.
    const int area_due = self->art.area_every > 0 &&
                         tick >= st->next_area_tick && can_area;
    const int use_area = area_due || (!can_regular && can_area);

    int changed;
    if (use_area) {
        changed = side_area_strike(self, enemy, tick);
        // Сохраняем фазу периодичности и пропускаем все наступившие сроки,
        // чтобы не делать несколько залпов за один такт.
        if (self->art.area_every > 0 && tick >= st->next_area_tick) {
            do {
                st->next_area_tick += self->art.area_every;
            } while (st->next_area_tick <= tick);
        }
    } else {
        changed = side_fire(self, enemy, tick);
    }

    st->last_fired_tick = tick;
    if (respect_time) {
        clock_gettime(CLOCK_MONOTONIC, &st->next_action);
        add_milliseconds(&st->next_action, next_reaction_delay_ms(&self->art));
    }

    // Стрелок проверяет только себя. Состояние противника обновят он сам
    // перед своим выстрелом или координатор по уведомлению.
    side_check_done(self, enemy);

    if (changed) {
        log_printf("\n%s>>> Состояние территорий после изменений:%s\n",
                   C_BOLD, C_RESET);
        grid_print(&sim->cfg->A.grid, "Территория A");
        grid_print(&sim->cfg->B.grid, "Территория B");
    }
    notify_coordinator_locked(sim);
    return 1;
}

static void *artillery_worker(void *opaque) {
    worker_arg_t *arg = (worker_arg_t *)opaque;
    simulation_t *sim = arg->sim;
    const int index = arg->side_index;
    const char *owner = side_by_index(sim, index)->name;
    log_printf("[ПОТОК] роль=artillery, сторона=%s, pthread=%lu: запущен\n",
               owner, (unsigned long)pthread_self());

    gunner_state_t st;
    st.last_fired_tick = -1;
    st.next_area_tick = side_by_index(sim, index)->art.area_every > 0
        ? side_by_index(sim, index)->art.area_every : 0;
    clock_gettime(CLOCK_MONOTONIC, &st.next_action);

    for (;;) {
        // Быстрый режим: ждём разрешения координатора на выстрел этого такта.
        if (sim->fast && sem_wait_retry(&sim->fire_sem[index]) < 0) break;

        pthread_mutex_lock(&sim->mutex);
        if (sim->stopping) {
            pthread_mutex_unlock(&sim->mutex);
            break;
        }
        (void)gunner_try_fire_locked(sim, index, &st, !sim->fast);
        pthread_mutex_unlock(&sim->mutex);

        if (sim->fast) {
            (void)sem_post(&sim->fire_ack[index]);   // такт отработан (выстрел или пропуск)
            continue;
        }
        sleep_ms_interruptible(sim, GUNNER_POLL_MS);
        if (signals_stop_requested()) break;
    }
    log_printf("[ПОТОК] роль=artillery, сторона=%s, pthread=%lu: завершён\n",
               owner, (unsigned long)pthread_self());
    return NULL;
}

/*
 * Ожидание конца такта. Возвращает:
 *   0 — такт истёк, можно начинать следующий; 1 — обе стороны прекратили огонь;
 *   2 — получен сигнал; -1 — ошибка.
 * Ожидание нарезано на отрезки не длиннее POLL_SLICE_MS: сигнал, пришедший
 * между проверкой флага и входом в sem_timedwait, будет замечен не позже
 * чем через один отрезок, а не через весь tick_ms.
 */
static int wait_for_tick_or_event(simulation_t *sim, long tick_ms) {
    if (tick_ms <= 0) {
        // Быстрый режим: такт уже полностью отработан рукопожатием в start_tick().
        if (signals_stop_requested()) return 2;
        pthread_mutex_lock(&sim->mutex);
        const int finished = refresh_and_check_finished_locked(sim);
        pthread_mutex_unlock(&sim->mutex);
        return finished ? 1 : 0;
    }

    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) return -1;
    add_milliseconds(&end, tick_ms);

    for (;;) {
        if (signals_stop_requested()) return 2;

        pthread_mutex_lock(&sim->mutex);
        const int finished = refresh_and_check_finished_locked(sim);
        pthread_mutex_unlock(&sim->mutex);
        if (finished) return 1;

        long left = ms_until(&end);
        if (left <= 0) return 0;
        long slice = left < POLL_SLICE_MS ? left : POLL_SLICE_MS;

        // sem_timedwait использует абсолютное время CLOCK_REALTIME.
        struct timespec abs;
        if (clock_gettime(CLOCK_REALTIME, &abs) < 0) return -1;
        add_milliseconds(&abs, slice);

        if (sem_timedwait(&sim->event_sem, &abs) == 0) {
            pthread_mutex_lock(&sim->mutex);
            sim->event_pending = 0;
            pthread_mutex_unlock(&sim->mutex);
            // Событие лишь будит координатор; начало следующего такта
            // по-прежнему определяется дедлайном, а не скоростью потоков.
            continue;
        }
        if (errno == EINTR || errno == ETIMEDOUT) continue;
        return -1;
    }
}

static int start_tick(simulation_t *sim, int tick) {
    pthread_mutex_lock(&sim->mutex);
    if (sim->stopping) {
        pthread_mutex_unlock(&sim->mutex);
        return -1;
    }
    sim->tick = tick;
    sim->side_processed_tick[0] = -1;
    sim->side_processed_tick[1] = -1;
    log_printf("\n%s========== ТАКТ %d ==========%s\n",
               C_BOLD, tick, C_RESET);
    pthread_mutex_unlock(&sim->mutex);

    // Фаза 1: государства пополняют запас и проверяют свои условия.
    if (sem_post(&sim->tick_sem[0]) < 0 || sem_post(&sim->tick_sem[1]) < 0)
        return -1;
    if (sem_wait_retry(&sim->tick_ack[0]) < 0 ||
        sem_wait_retry(&sim->tick_ack[1]) < 0)
        return -1;

    // Фаза 2 (только быстрый режим): оба подразделения отрабатывают такт.
    if (sim->fast) {
        if (sem_post(&sim->fire_sem[0]) < 0 || sem_post(&sim->fire_sem[1]) < 0)
            return -1;
        if (sem_wait_retry(&sim->fire_ack[0]) < 0 ||
            sem_wait_retry(&sim->fire_ack[1]) < 0)
            return -1;
    }

    pthread_mutex_lock(&sim->mutex);
    sim->ticks_completed = tick + 1;
    pthread_mutex_unlock(&sim->mutex);
    return 0;
}

static void request_stop(simulation_t *sim, simulation_stop_reason_t reason) {
    pthread_mutex_lock(&sim->mutex);
    if (!sim->stopping) {
        sim->stopping = 1;
        sim->stop_reason = reason;
    }
    pthread_mutex_unlock(&sim->mutex);
}

static void release_and_join(simulation_t *sim,
                             pthread_t side_threads[2], int side_created[2],
                             pthread_t artillery_threads[2], int artillery_created[2]) {
    request_stop(sim, sim->stop_reason);
    // Потоки могут ожидать очередного такта или разрешения на выстрел; разблокируем их.
    for (int i = 0; i < 2; ++i) {
        (void)sem_post(&sim->tick_sem[i]);
        (void)sem_post(&sim->fire_sem[i]);
    }
    (void)sem_post(&sim->event_sem);
    for (int i = 0; i < 2; ++i) {
        if (side_created[i]) (void)pthread_join(side_threads[i], NULL);
        if (artillery_created[i]) (void)pthread_join(artillery_threads[i], NULL);
    }
}

simulation_stop_reason_t simulation_run(config_t *cfg, int *ticks_done) {
    if (ticks_done) *ticks_done = 0;
    simulation_t sim;
    memset(&sim, 0, sizeof(sim));
    sim.cfg = cfg;
    sim.tick = 0;
    sim.fast = cfg->tick_ms <= 0;
    sim.side_processed_tick[0] = -1;
    sim.side_processed_tick[1] = -1;
    sim.stop_reason = SIM_STOP_NATURAL;

    if (pthread_mutex_init(&sim.mutex, NULL) != 0) {
        log_printf("[ERROR] Не удалось создать mutex модели.\n");
        return SIM_STOP_ERROR;
    }

    // Все семафоры инициализируются в одном списке; при ошибке уничтожаются
    // только уже созданные.
    sem_t *sems[] = { &sim.event_sem,
                      &sim.tick_sem[0], &sim.tick_sem[1],
                      &sim.tick_ack[0], &sim.tick_ack[1],
                      &sim.fire_sem[0], &sim.fire_sem[1],
                      &sim.fire_ack[0], &sim.fire_ack[1] };
    const int sem_count = (int)(sizeof(sems) / sizeof(sems[0]));
    int sem_ready = 0;
    while (sem_ready < sem_count && sem_init(sems[sem_ready], 0, 0) == 0) sem_ready++;
    if (sem_ready < sem_count) {
        log_printf("[ERROR] Не удалось создать семафоры модели.\n");
        for (int i = 0; i < sem_ready; ++i) sem_destroy(sems[i]);
        pthread_mutex_destroy(&sim.mutex);
        return SIM_STOP_ERROR;
    }

    log_printf("[ПОТОК] роль=coordinator, pthread=%lu: управление моделью\n",
               (unsigned long)pthread_self());
    if (sim.fast)
        log_printf("[РЕЖИМ] tick_ms=0: быстрое моделирование, один выстрел на сторону за такт, "
                   "задержки реакции не применяются\n");
    pthread_t side_threads[2], artillery_threads[2];
    int side_created[2] = {0, 0}, artillery_created[2] = {0, 0};
    worker_arg_t side_args[2] = {{&sim, 0}, {&sim, 1}};
    worker_arg_t artillery_args[2] = {{&sim, 0}, {&sim, 1}};

    // Все рабочие потоки наследуют заблокированные SIGINT/SIGTERM.
    // После их создания сигналы разблокируются только у координатора,
    // поэтому ожидание координатора гарантированно прерывается по Ctrl+C/SIGTERM.
    sigset_t set, old_set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int mask_changed = pthread_sigmask(SIG_BLOCK, &set, &old_set) == 0;

    int create_error = 0;
    for (int i = 0; i < 2 && !create_error; ++i) {
        int rc = pthread_create(&side_threads[i], NULL, side_worker, &side_args[i]);
        if (rc != 0) {
            log_printf("[ERROR] Не удалось создать поток стороны %d: %s\n", i, strerror(rc));
            create_error = 1;
        } else {
            side_created[i] = 1;
        }
    }
    // Огневые потоки создаются сразу: до первого start_tick() они ничего не делают
    // (side_processed_tick = -1, в быстром режиме ждут fire_sem), поэтому такт 0
    // тоже проходит полное рукопожатие и пополнение предшествует первому выстрелу.
    for (int i = 0; i < 2 && !create_error; ++i) {
        int rc = pthread_create(&artillery_threads[i], NULL,
                                artillery_worker, &artillery_args[i]);
        if (rc != 0) {
            log_printf("[ERROR] Не удалось создать огневой поток %d: %s\n", i, strerror(rc));
            create_error = 1;
        } else {
            artillery_created[i] = 1;
        }
    }

    if (mask_changed) {
        int rc = pthread_sigmask(SIG_SETMASK, &old_set, NULL);
        if (rc != 0) {
            log_printf("[ERROR] Не удалось восстановить маску сигналов: %s\n", strerror(rc));
            create_error = 1;
        }
    }

    if (!create_error && start_tick(&sim, 0) < 0) create_error = 1;

    if (create_error) {
        sim.stop_reason = SIM_STOP_ERROR;
        release_and_join(&sim, side_threads, side_created,
                         artillery_threads, artillery_created);
    } else {
        // Координаторская нить — текущая/main. Она ведёт такты и решение
        // о глобальном завершении, но не выполняет выстрелы за стороны.
        for (;;) {
            const int wait_result = wait_for_tick_or_event(&sim, cfg->tick_ms);
            if (wait_result == 2) {
                log_printf("\n%s[STOP]%s Прерывание (сигнал %d) на такте %d\n",
                           C_RED, C_RESET, signals_last_signal(), sim.tick);
                sim.stop_reason = SIM_STOP_SIGNAL;
                break;
            }
            if (wait_result == 1) {
                log_printf("\n%s[STOP]%s Обе стороны прекратили огонь на такте %d\n",
                           C_RED, C_RESET, sim.tick);
                sim.stop_reason = SIM_STOP_NATURAL;
                break;
            }
            if (wait_result < 0) {
                log_printf("[ERROR] Ошибка ожидания события координатором.\n");
                sim.stop_reason = SIM_STOP_ERROR;
                break;
            }

            int next_tick = sim.tick + 1;
            if (signals_stop_requested()) {
                log_printf("\n%s[STOP]%s Прерывание (сигнал %d) на такте %d\n",
                           C_RED, C_RESET, signals_last_signal(), sim.tick);
                sim.stop_reason = SIM_STOP_SIGNAL;
                break;
            }
            if (cfg->max_ticks > 0 && next_tick >= cfg->max_ticks) {
                log_printf("\n%s[STOP]%s Лимит тактов: %d\n",
                           C_RED, C_RESET, cfg->max_ticks);
                sim.stop_reason = SIM_STOP_LIMIT;
                break;
            }

            if (start_tick(&sim, next_tick) < 0) {
                log_printf("[ERROR] Не удалось синхронизировать начало такта.\n");
                sim.stop_reason = SIM_STOP_ERROR;
                break;
            }
        }
        release_and_join(&sim, side_threads, side_created,
                         artillery_threads, artillery_created);
    }

    const simulation_stop_reason_t result = sim.stop_reason;
    if (ticks_done) *ticks_done = sim.ticks_completed;
    for (int i = 0; i < sem_count; ++i) sem_destroy(sems[i]);
    pthread_mutex_destroy(&sim.mutex);
    return result;
}
