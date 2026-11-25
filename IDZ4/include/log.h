// log.h — вывод на консоль и в журнал через файловые дескрипторы.
// В отличие от IDZ3 модуль однопоточный (потоков в программе нет),
// зато умеет передавать каждую выведенную строку «приёмнику» —
// сервер использует его, чтобы транслировать журнал наблюдателям.
#ifndef LOG_H
#define LOG_H

#include <stddef.h>

typedef void (*log_sink_fn)(const char *text, size_t len, void *ctx);

int  log_open(const char *path);          // создать/усечь журнал
int  log_open_append(const char *path);   // дописывать в существующий
void log_close(void);

// Консоль + журнал + приёмник.
int  log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Только консоль.
int  log_console(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Только журнал-файл (например, подробные карты, которые не нужны на консоли).
int  log_file(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Подписать приёмник на всё, что выводит log_printf (NULL — отписать).
void log_set_sink(log_sink_fn fn, void *ctx);
// Отключить вывод на консоль (режим --quiet).
void log_set_console(int enabled);

#endif // LOG_H
