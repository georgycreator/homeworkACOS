#define _POSIX_C_SOURCE 200809L
#include "client_common.h"
#include "log.h"
#include "proto.h"
#include "signals.h"
#include "util.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_log_path = NULL;

int client_send(client_app_t *app, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = conn_vsend(&app->conn, fmt, ap);
    va_end(ap);
    return r;
}

int client_parse_common(client_app_t *app, int argc, char **argv,
                        const char *extra_short, int (*extra)(int opt, const char *arg),
                        const struct option *extra_long) {
    app->host = "127.0.0.1";
    app->port = 5000;
    app->retries = 20;
    app->retry_ms = 250;

    // Общие длинные ключи + ключи конкретного клиента.
    struct option opts[24];
    int n = 0;
    static const struct option common[] = {
        {"server", required_argument, 0, 's'}, {"port", required_argument, 0, 'p'},
        {"retries", required_argument, 0, 2000}, {"retry-ms", required_argument, 0, 2001},
        {"log", required_argument, 0, 2002}, {"quiet", no_argument, 0, 2003},
        {"help", no_argument, 0, 'h'},
    };
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) opts[n++] = common[i];
    for (const struct option *o = extra_long; o && o->name; o++) opts[n++] = *o;
    opts[n] = (struct option){0, 0, 0, 0};

    char shortopts[64];
    snprintf(shortopts, sizeof(shortopts), "s:p:h%s", extra_short ? extra_short : "");

    optind = 1;
    long v;
    int opt;
    while ((opt = getopt_long(argc, argv, shortopts, opts, NULL)) != -1) {
        switch (opt) {
            case 's': app->host = optarg; break;
            case 'p':
                if (util_parse_long(optarg, 1, 65535, &v) < 0) { fprintf(stderr, "Неверный порт: %s\n", optarg); return -1; }
                app->port = (int)v;
                break;
            case 2000:
                if (util_parse_long(optarg, 0, 100000, &v) < 0) { fprintf(stderr, "Неверное --retries\n"); return -1; }
                app->retries = (int)v;
                break;
            case 2001:
                if (util_parse_long(optarg, 10, 60000, &v) < 0) { fprintf(stderr, "Неверное --retry-ms\n"); return -1; }
                app->retry_ms = (int)v;
                break;
            case 2002: g_log_path = optarg; break;
            case 2003: log_set_console(0); break;
            case 'h': return 1;
            case '?': return -1;
            default:
                if (!extra || extra(opt, optarg) < 0) return -1;
        }
    }
    if (optind < argc) { fprintf(stderr, "Лишние аргументы: %s\n", argv[optind]); return -1; }
    return 0;
}

static int connect_with_retries(client_app_t *app) {
    for (int attempt = 0; ; attempt++) {
        const int fd = net_connect(app->host, app->port, 3000);
        if (fd >= 0) return fd;
        if (attempt >= app->retries || signals_stop_requested()) return -1;
        if (attempt == 0)
            log_printf("[КЛИЕНТ] Сервер %s:%d пока недоступен, повторяю попытки...\n", app->host, app->port);
        sleep_ms(app->retry_ms);
    }
}

int client_run(client_app_t *app) {
    if (signals_install() < 0) { fprintf(stderr, "Не удалось установить обработчики сигналов\n"); return 1; }
    if (g_log_path && log_open(g_log_path) < 0) { fprintf(stderr, "Не удалось открыть лог: %s\n", g_log_path); return 1; }

    const int fd = connect_with_retries(app);
    if (fd < 0) {
        log_printf("[КЛИЕНТ] Не удалось подключиться к %s:%d\n", app->host, app->port);
        log_close();
        return 2;
    }
    conn_init(&app->conn, fd, 4u * 1024u * 1024u);
    log_printf("[КЛИЕНТ] Подключён к %s:%d, роль %s\n", app->host, app->port, app->role);
    client_send(app, "HELLO %s", app->role);

    int finished = 0;
    app->exit_code = 0;
    while (!finished) {
        if (signals_stop_requested()) {
            log_printf("[КЛИЕНТ] Получен сигнал завершения, отключаюсь от сервера\n");
            client_send(app, "BYE");
            conn_flush(&app->conn);
            break;
        }
        struct pollfd p = { app->conn.fd, (short)(POLLIN | (conn_has_output(&app->conn) ? POLLOUT : 0)), 0 };
        long wait = app->timer_ms ? app->timer_ms(app) : -1;
        if (wait < 0 || wait > 200) wait = 200;      // просыпаемся и для проверки сигналов
        const int rc = poll(&p, 1, (int)wait);
        if (rc < 0 && errno != EINTR) { log_printf("[КЛИЕНТ] poll: %s\n", strerror(errno)); app->exit_code = 2; break; }

        if (rc > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) conn_fill(&app->conn);
        char line[PROTO_MAX_LINE];
        int lr;
        while (!finished && (lr = conn_getline(&app->conn, line, sizeof(line))) != 0) {
            if (lr < 0) { log_printf("[КЛИЕНТ] Нарушение протокола: слишком длинная строка\n"); app->exit_code = 2; finished = 1; break; }
            char *rest = strchr(line, ' ');
            if (rest) *rest++ = '\0'; else rest = line + strlen(line);
            if (strcmp(line, "PING") == 0) {                 // проверка «живости» от сервера
                client_send(app, "PONG");
                continue;
            }
            if (strcmp(line, "ERR") == 0) {
                log_printf("[СЕРВЕР] Отказ: %s\n", rest);
                app->exit_code = 1;
                finished = 1;
                break;
            }
            if (app->on_line(app, line, rest)) finished = 1;
        }
        if (!finished && app->on_timer && app->timer_ms && app->timer_ms(app) == 0) app->on_timer(app);
        if (!finished) conn_flush(&app->conn);
        if (!finished && (app->conn.dead || (app->conn.eof && app->conn.rlen == 0))) {
            log_printf("[КЛИЕНТ] Сервер закрыл соединение без сообщения о завершении\n");
            app->exit_code = 2;
            break;
        }
    }
    // Дослать накопленное (например, BYE) и корректно закрыть соединение.
    conn_flush(&app->conn);
    conn_close(&app->conn);
    log_printf("[КЛИЕНТ] Работа завершена (код %d)\n", app->exit_code);
    log_close();
    return app->exit_code;
}
