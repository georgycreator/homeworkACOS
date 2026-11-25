// net.h — тонкая обёртка над сокетами BSD: подключение, прослушивание и
// построчный буферизованный обмен через неблокирующие дескрипторы.
#ifndef NET_H
#define NET_H

#include <stdarg.h>
#include <stddef.h>
#include "proto.h"

// Соединение с построчной буферизацией чтения и записи.
typedef struct {
    int    fd;                          // сокет (-1, если закрыт)
    char   rbuf[PROTO_MAX_LINE * 2];    // накопленные входящие байты
    size_t rlen;
    char  *wbuf;                        // очередь исходящих байтов
    size_t woff, wlen, wcap, wmax;      // wmax — предел очереди (защита от медленных клиентов)
    int    eof;                         // собеседник закрыл соединение
    int    dead;                        // ошибка ввода-вывода или переполнение очереди
} conn_t;

void conn_init(conn_t *c, int fd, size_t wmax);
void conn_close(conn_t *c);

// Прочитать всё доступное из сокета в буфер. 0 — успех, -1 — ошибка (dead).
int  conn_fill(conn_t *c);
// Достать следующую строку без '\n' (и без '\r').
// 1 — строка получена, 0 — полной строки пока нет, -1 — строка слишком длинная.
int  conn_getline(conn_t *c, char *out, size_t cap);

// Поставить строку в очередь отправки ('\n' добавляется автоматически).
int  conn_send(conn_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  conn_vsend(conn_t *c, const char *fmt, va_list ap);
// Отправить сколько удаётся без блокировки. 0 — успех, -1 — ошибка.
int  conn_flush(conn_t *c);
int  conn_has_output(const conn_t *c);

// Создать слушающий сокет. port == 0 — система выберет свободный порт,
// фактический порт возвращается через *bound_port. Возвращает fd или -1.
int  net_listen(const char *host, int port, int *bound_port);
// Подключиться с таймаутом. Возвращает неблокирующий fd или -1.
int  net_connect(const char *host, int port, int timeout_ms);
// Принять соединение (неблокирующе). Возвращает fd или -1.
int  net_accept(int listen_fd, char *peer, size_t peer_cap);

int  net_set_nonblock(int fd);

#endif // NET_H
