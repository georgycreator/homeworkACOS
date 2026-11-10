#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "grid.h"
#include "side.h"
#include "log.h"
#include "signals.h"
#include "colors.h"
#include "process_simulation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DEFAULT_IPC_MODE
#define DEFAULT_IPC_MODE IPC_SEM_NAMED
#endif

static void print_usage(const char *prog) {
    log_console("Использование: %s [ключи] [файл.conf]\n", prog);
    log_console("  %s [file.conf]       — именованные POSIX-семафоры (4–5 баллов)\n", prog);
    log_console("  %s -i                — интерактивный ввод\n", prog);
    log_console("  Сборка IDZ3_unnamed — неименованные семафоры в Shared Memory (6 баллов)\n");
    log_console("  %s -h                — справка\n", prog);
}

int main(int argc, char **argv) {
    colors_init();
    if (signals_install() < 0) {
        fprintf(stderr, "Не удалось установить обработчики сигналов\n");
        return 1;
    }

    config_t cfg;
    if (config_defaults(&cfg) < 0) {
        fprintf(stderr, "Ошибка инициализации конфигурации\n");
        return 1;
    }
    int interactive = 0;
    const char *conf_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
            interactive = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); config_free(&cfg); return 0;
        } else if (argv[i][0] != '-') conf_path = argv[i];
        else {
            fprintf(stderr, "Неизвестный ключ: %s\n", argv[i]);
            print_usage(argv[0]); config_free(&cfg); return 1;
        }
    }
    if (conf_path && config_load(&cfg, conf_path) < 0) {
        fprintf(stderr, "Ошибка в конфигурационном файле: %s\n", conf_path);
        config_free(&cfg); return 1;
    }
    if (log_open(cfg.log_path) < 0) {
        fprintf(stderr, "Не удалось открыть лог: %s\n", cfg.log_path);
        config_free(&cfg); return 1;
    }
    if (interactive && config_read_stdin(&cfg) < 0) {
        int by_signal = signals_stop_requested();
        log_printf("\n[STOP] Интерактивный ввод прерван%s\n",
                   by_signal ? " сигналом" : ": конец входного потока");
        log_close(); config_free(&cfg); return by_signal ? 0 : 1;
    }

    artillery_seed(&cfg.A.art, cfg.seed ^ 0x9E3779B9u);
    artillery_seed(&cfg.B.art, cfg.seed ^ 0x85EBCA6Bu);
    const int infinite = cfg.max_ticks <= 0;
    log_printf("\n%s%s========== СТАРТ IDZ3 ==========%s\n", C_BOLD, C_GREEN, C_RESET);
    config_print(&cfg);
    log_printf("Реализация: %s POSIX Shared Memory + %s POSIX-семафоры.\n",
               DEFAULT_IPC_MODE == IPC_SEM_NAMED ? "родительский процесс и дочерние процессы" :
               "родительский процесс и дочерние процессы",
               DEFAULT_IPC_MODE == IPC_SEM_NAMED ? "именованные" : "неименованные (pshared=1)");
    if (infinite) log_printf("Режим: без ограничения тактов. Для остановки нажмите Ctrl+C.\n");
    else log_printf("Режим: лимит тактов = %d.\n", cfg.max_ticks);
    log_printf("\n--- Начальное состояние ---\n");
    grid_print(&cfg.A.grid, "Территория A");
    grid_print(&cfg.B.grid, "Территория B");
    side_check_done(&cfg.A, &cfg.B);
    side_check_done(&cfg.B, &cfg.A);

    int ticks_done = 0;
    simulation_stop_reason_t reason = simulation_run_processes(&cfg, &ticks_done,
                                                               (ipc_mode_t)DEFAULT_IPC_MODE);
    log_printf("\n[MAIN] Процессная модель завершена. Тактов: %d.\n", ticks_done);
    log_close();
    config_free(&cfg);
    return reason == SIM_STOP_ERROR ? 1 : 0;
}
