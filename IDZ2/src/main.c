#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "grid.h"
#include "side.h"
#include "log.h"
#include "signals.h"
#include "colors.h"
#include "simulation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

static void print_usage(const char *prog) {
    log_console("Использование: %s [ключи] [файл.conf]\n", prog);
    log_console("  %s                    - параметры по умолчанию\n", prog);
    log_console("  %s my.conf            - загрузить конфиг\n", prog);
    log_console("  %s -i | --interactive - интерактивный ввод\n", prog);
    log_console("  %s -h | --help        - справка\n", prog);
    log_console("Параметры, не указанные в файле, берутся из значений по умолчанию.\n");
    log_console("Формат цели в файле: A.target = строка столбец стоимость [тип]\n");
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
            // Сигнал — штатная остановка пользователем (код 0);
            // конец входного потока до завершения ввода — ошибка запуска (код 1).
            const int by_signal = signals_stop_requested();
            if (by_signal)
                log_printf("\n[STOP] Интерактивный ввод прерван сигналом %d\n",
                           signals_last_signal());
            else
                log_printf("\n[STOP] Интерактивный ввод прерван: достигнут конец входного потока\n");
            log_close(); config_free(&cfg);
            return by_signal ? 0 : 1;
        }
    }

    // Каждое огневое подразделение получает собственный воспроизводимый seed.
    artillery_seed(&cfg.A.art, cfg.seed ^ 0x9E3779B9u);
    artillery_seed(&cfg.B.art, cfg.seed ^ 0x85EBCA6Bu);
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

    int ticks_done = 0;
    simulation_stop_reason_t reason_stop = simulation_run(&cfg, &ticks_done);

    log_printf("\n%s%s========== ИТОГИ ==========%s\n",
               C_BOLD, C_CYAN, C_RESET);
    log_printf("Всего тактов: %d\n", ticks_done);
    switch (reason_stop) {
        case SIM_STOP_NATURAL: log_printf("Причина: обе стороны прекратили огонь\n"); break;
        case SIM_STOP_LIMIT:   log_printf("Причина: лимит тактов\n"); break;
        case SIM_STOP_SIGNAL:  log_printf("Причина: сигнал пользователя\n"); break;
        case SIM_STOP_ERROR:   log_printf("Причина: ошибка многопоточной модели\n"); break;
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
    return reason_stop == SIM_STOP_ERROR ? 1 : 0;
}
