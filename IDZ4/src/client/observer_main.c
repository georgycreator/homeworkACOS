// observer_main.c — клиент «наблюдатель».
//
// Показывает комплексную картину: журнал всех событий обеих сторон, карты
// территорий и итоги. Наблюдателей может быть сколько угодно; их можно
// подключать и отключать в любой момент, не нарушая работу моделирования.
#define _POSIX_C_SOURCE 200809L
#include "client_common.h"
#include "colors.h"
#include "log.h"
#include "proto.h"
#include "signals.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int on_line(client_app_t *app, const char *cmd, const char *rest) {
    (void)app;
    if (!strcmp(cmd, "EVT")) {
        // Лёгкая подсветка ключевых событий (журнал приходит без цвета).
        const char *col = "";
        if (strstr(rest, "ПОПАДАНИЕ")) col = C_GREEN;
        else if (strstr(rest, "ПРОМАХ")) col = C_YELLOW;
        else if (strstr(rest, "ПРЕКРАЩАЕТ") || strstr(rest, "ПАУЗА")) col = C_MAGENTA;
        else if (strstr(rest, "=== ") || strstr(rest, "ТАКТ ")) col = C_BOLD;
        else if (strstr(rest, "ОТКЛОН")) col = C_RED;
        log_printf("%s%s%s\n", col, rest, *col ? C_RESET : "");
        return 0;
    }
    if (!strcmp(cmd, "TXT") || !strcmp(cmd, "PAUSE") || !strcmp(cmd, "RESUME")) { log_printf("[СЕРВЕР] %s\n", rest); return 0; }
    if (!strcmp(cmd, "WELCOME")) { log_printf("[НАБЛЮДАТЕЛЬ] Подключение принято, получаю журнал и текущее состояние\n"); return 0; }
    if (!strcmp(cmd, "END")) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", rest);
        log_printf("[НАБЛЮДАТЕЛЬ] Сервер завершил моделирование: %s\n", stop_reason_text(atoi(tmp)));
        return 1;
    }
    return 0;   // CFG и прочие служебные строки наблюдателю не нужны
}

int main(int argc, char **argv) {
    static client_app_t app;
    colors_init();
    const int rc = client_parse_common(&app, argc, argv, NULL, NULL, NULL);
    if (rc != 0) {
        fprintf(rc < 0 ? stderr : stdout,
                "Использование: %s -s IP -p PORT [--retries N] [--retry-ms MS] [--log FILE] [--quiet]\n"
                "  Наблюдатель: показывает полный ход моделирования. Можно запускать много копий.\n", argv[0]);
        return rc == 1 ? 0 : 1;
    }
    app.role = "observer";
    app.on_line = on_line;
    return client_run(&app);
}
