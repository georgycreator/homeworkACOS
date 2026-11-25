// client_common.h — общий каркас всех клиентских программ: подключение
// с повторными попытками, приветствие HELLO и цикл обработки сообщений.
#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include <getopt.h>
#include "net.h"

struct client_app;

typedef struct client_app {
    const char *role;         // строка роли для HELLO: "side-A", "gun-B", "observer"
    const char *host;
    int         port;
    int         retries;      // сколько раз повторять неудачное подключение
    int         retry_ms;     // пауза между попытками
    conn_t      conn;
    int         exit_code;    // код возврата программы

    // Обработчик строки сервера. cmd — первое слово, rest — остаток строки
    // (без ведущего пробела). Возврат 1 — работа завершена (получен END).
    int  (*on_line)(struct client_app *app, const char *cmd, const char *rest);
    // Через сколько мс нужен вызов on_timer (-1 — таймер не нужен).
    long (*timer_ms)(struct client_app *app);
    void (*on_timer)(struct client_app *app);
    void *ctx;                // данные конкретного клиента
} client_app_t;

// Разобрать общие ключи -s/--server, -p/--port, --retries, --retry-ms, --log, --quiet.
// Возвращает 0 при успехе, -1 при ошибке (сообщение уже выведено), 1 — запрошена справка.
int  client_parse_common(client_app_t *app, int argc, char **argv,
                         const char *extra_short, int (*extra)(int opt, const char *arg),
                         const struct option *extra_long);

// Отправить команду серверу.
int  client_send(client_app_t *app, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Подключиться, представиться и обрабатывать сообщения до END/EOF/сигнала.
// Возвращает код возврата процесса.
int  client_run(client_app_t *app);

#endif // CLIENT_COMMON_H
