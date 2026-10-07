#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "grid.h"
#include "side.h"
#include "log.h"
#include "signals.h"
#include "colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

static void delay_ms(long ms) {
    if (ms <= 0) return;
    struct timespec req = { ms / 1000, (ms % 1000) * 1000000L };
    struct timespec rem;
    while (nanosleep(&req, &rem) < 0) {
        if (errno != EINTR) break;
        if (signals_stop_requested()) break;
        req = rem;
    }
}

// Пересчитать, на каком такте подразделение сможет стрелять снова.
// reload = max(1, reaction_ms / tick_ms) + rand%jitter_ticks.
// При tick_ms == 0 подразделение стреляет каждый такт (reload = 1).
static void artillery_reload(side_t *s, int tick, int tick_ms) {
    int base = 1;
    if (tick_ms > 0 && s->art.reaction_ms > 0) {
        base = s->art.reaction_ms / tick_ms;
        if (base < 1) base = 1;
    }
    int jitter = 0;
    if (tick_ms > 0 && s->art.reaction_jitter_ms > 0) {
        int jmax = s->art.reaction_jitter_ms / tick_ms;
        if (jmax > 0) jitter = rand() % (jmax + 1);
    }
    s->art.next_fire_tick = tick + base + jitter;
}

static void print_usage(const char *prog) {
    log_console("Использование: %s [ключи] [файл.conf]\n", prog);
    log_console("  %s                    - параметры по умолчанию\n", prog);
    log_console("  %s my.conf            - загрузить конфиг\n", prog);
    log_console("  %s -i | --interactive - интерактивный ввод\n", prog);
    log_console("  %s -h | --help        - справка\n", prog);
}

// Пополнение запасов. Вызывается в НАЧАЛЕ такта для всех сторон,
// включая те, что были помечены active=0 из-за отсутствия снарядов.
// Если у стороны настроено пополнение, она может "оживать" и
// возвращаться в бой.
static void tick_replenish(side_t *s, int tick) {
    if (s->art.replenish_every <= 0) return;
    if ((tick + 1) % s->art.replenish_every != 0) return;
    side_replenish(s);
    // Если сторона ждала снаряды — снова активируем её.
    if (!s->active
        && s->grid.alive_targets > 0
        && (artillery_can_fire(&s->art) || artillery_can_area_strike(&s->art))) {
        s->active = 1;
    }
}


// Итоговый вердикт: кто победил или у кого меньше потерь.
static void print_verdict(const side_t *a, const side_t *b) {
    log_printf("\n--- Вердикт ---\n");
    log_printf("Потери: %s — %" PRId64 ", %s — %" PRId64 "\n",
               a->name, a->lost_cost, b->name, b->lost_cost);
    if (a->grid.alive_targets > 0 && b->grid.alive_targets <= 0)
        log_printf("Победа: %s (у противника уничтожены все цели)\n", a->name);
    else if (b->grid.alive_targets > 0 && a->grid.alive_targets <= 0)
        log_printf("Победа: %s (у противника уничтожены все цели)\n", b->name);
    else if (a->lost_cost < b->lost_cost)
        log_printf("Преимущество: %s (потери меньше на %" PRId64 ")\n",
                   a->name, b->lost_cost - a->lost_cost);
    else if (b->lost_cost < a->lost_cost)
        log_printf("Преимущество: %s (потери меньше на %" PRId64 ")\n",
                   b->name, a->lost_cost - b->lost_cost);
    else
        log_printf("Ничья по стоимости потерь\n");
}

int main(int argc, char **argv) {
    colors_init();
    if (signals_install() < 0) {
        fprintf(stderr, "Не удалось установить обработчики сигналов\n");
        return 1;
    }

    config_t cfg;
    if (config_defaults(&cfg) < 0) {
        fprintf(stderr, "Ошибка инициализации\n");
        return 1;
    }

    int interactive = 0;
    const char *conf_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); config_free(&cfg); return 0;
        } else if (argv[i][0] != '-')
            conf_path = argv[i];
        else {
            fprintf(stderr, "Неизвестный ключ: %s\n", argv[i]);
            print_usage(argv[0]); config_free(&cfg); return 1;
        }
    }

    if (conf_path) {
        if (config_load(&cfg, conf_path) < 0) {
            fprintf(stderr, "Ошибка в конфигурационном файле: %s\n", conf_path);
            config_free(&cfg);
            return 1;
        }
    }

    if (log_open(cfg.log_path) < 0) {
        fprintf(stderr, "Не удалось открыть лог: %s\n", cfg.log_path);
        config_free(&cfg); return 1;
    }

    if (interactive) {
        if (config_read_stdin(&cfg) < 0) {
            log_printf("\n[STOP] Интерактивный ввод прерван сигналом\n");
            log_close(); config_free(&cfg); return 0;
        }
    }

    srand(cfg.seed);
    const int infinite = (cfg.max_ticks <= 0);

    log_printf("\n%s%s========== СТАРТ ==========%s\n",
               C_BOLD, C_GREEN, C_RESET);
    config_print(&cfg);
    if (infinite)
        log_printf("Режим: без ограничения. Для остановки нажмите Ctrl+C.\n");
    else
        log_printf("Режим: лимит тактов = %d.\n", cfg.max_ticks);

    log_printf("\n--- Начальное состояние ---\n");
    grid_print(&cfg.A.grid, "Территория A");
    grid_print(&cfg.B.grid, "Территория B");

    side_check_done(&cfg.A, &cfg.B);
    side_check_done(&cfg.B, &cfg.A);

    int tick = 0;         // номер текущего такта
    int ticks_done = 0;   // сколько тактов реально выполнено
    int reason_stop = -1;

    while (1) {
        if (signals_stop_requested()) {
            log_printf("\n%s[STOP]%s Прерывание (сигнал %d) на такте %d\n",
                       C_RED, C_RESET, signals_last_signal(), tick);
            reason_stop = 2; break;
        }
        if (!cfg.A.active && !cfg.B.active) {
            log_printf("\n%s[STOP]%s Обе стороны прекратили огонь на такте %d\n",
                       C_RED, C_RESET, tick);
            reason_stop = 0; break;
        }
        if (!infinite && tick >= cfg.max_ticks) {
            log_printf("\n%s[STOP]%s Лимит тактов: %d\n",
                       C_RED, C_RESET, cfg.max_ticks);
            reason_stop = 1; break;
        }

        log_printf("\n%s========== ТАКТ %d ==========%s\n",
                   C_BOLD, tick, C_RESET);

        // 1) Пополнение в НАЧАЛЕ такта — до выстрелов и до проверок.
        tick_replenish(&cfg.A, tick);
        tick_replenish(&cfg.B, tick);

        int changed = 0;

        // 2) Ход A — стреляет, только если его таймер дозрел.
        // После выстрела проверяем ОБЕ стороны.
        if (cfg.A.active && tick >= cfg.A.art.next_fire_tick) {
            int use_area = (cfg.A.art.area_every > 0) && (tick > 0)
                           && (tick % cfg.A.art.area_every == 0)
                           && (cfg.A.art.area_ammo > 0)
                           && artillery_can_area_strike(&cfg.A.art);
            if (use_area) changed |= side_area_strike(&cfg.A, &cfg.B, tick);
            else          changed |= side_fire(&cfg.A, &cfg.B, tick);
            side_check_done(&cfg.A, &cfg.B);
            side_check_done(&cfg.B, &cfg.A);
            artillery_reload(&cfg.A, tick, cfg.tick_ms);
        }
        if (signals_stop_requested()) { ticks_done++; continue; }

        // 3) Ход B — независимо от A, по своему таймеру.
        if (cfg.B.active && tick >= cfg.B.art.next_fire_tick) {
            int use_area = (cfg.B.art.area_every > 0) && (tick > 0)
                           && (tick % cfg.B.art.area_every == 0)
                           && (cfg.B.art.area_ammo > 0)
                           && artillery_can_area_strike(&cfg.B.art);
            if (use_area) changed |= side_area_strike(&cfg.B, &cfg.A, tick);
            else          changed |= side_fire(&cfg.B, &cfg.A, tick);
            side_check_done(&cfg.B, &cfg.A);
            side_check_done(&cfg.A, &cfg.B);
            artillery_reload(&cfg.B, tick, cfg.tick_ms);
        }
        if (signals_stop_requested()) { ticks_done++; continue; }

        if (changed) {
            log_printf("\n%s>>> Состояние территорий после изменений:%s\n",
                       C_BOLD, C_RESET);
            grid_print(&cfg.A.grid, "Территория A");
            grid_print(&cfg.B.grid, "Территория B");
        }

        // 4) Одна общая пауза такта + учёт.
        if (cfg.tick_ms > 0) delay_ms(cfg.tick_ms);
        ticks_done++;

        // 5) Защита от зацикливания.
        int a_revivable = (cfg.A.art.replenish_every > 0
                           && cfg.A.art.replenish_amount > 0
                           && cfg.A.grid.alive_targets > 0);
        int b_revivable = (cfg.B.art.replenish_every > 0
                           && cfg.B.art.replenish_amount > 0
                           && cfg.B.grid.alive_targets > 0);

        if (!cfg.A.active && !cfg.B.active
            && !a_revivable && !b_revivable) {
            log_printf("\n%s[STOP]%s Никто не может продолжать огонь.\n",
                       C_RED, C_RESET);
            reason_stop = 0; break;
        }

        tick++;
    }

    log_printf("\n%s%s========== ИТОГИ ==========%s\n",
               C_BOLD, C_CYAN, C_RESET);
    log_printf("Всего тактов: %d\n", ticks_done);
    switch (reason_stop) {
        case 0: log_printf("Причина: обе стороны прекратили огонь\n"); break;
        case 1: log_printf("Причина: лимит тактов\n"); break;
        case 2: log_printf("Причина: сигнал пользователя\n"); break;
    }

    side_print_stats(&cfg.A);
    side_print_stats(&cfg.B);
    print_verdict(&cfg.A, &cfg.B);

    log_printf("\n--- Финальное состояние ---\n");
    grid_print(&cfg.A.grid, "Территория A");
    grid_print(&cfg.B.grid, "Территория B");

    log_printf("\n%s%s========== ЗАВЕРШЕНИЕ ==========%s\n",
               C_BOLD, C_GREEN, C_RESET);

    log_close();
    config_free(&cfg);
    return 0;
}
