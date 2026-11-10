// log.h — логирование через файловые дескрипторы.
#ifndef LOG_H
#define LOG_H
int log_open(const char *path);
int log_open_append(const char *path);
void log_close(void);
int log_printf(const char *fmt, ...);
int log_console(const char *fmt, ...);
#endif
