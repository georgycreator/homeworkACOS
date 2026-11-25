#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_log_fd = -1;          // дескриптор журнала; -1 — не открыт
static int g_console_on = 1;       // выводить ли на stdout
static int g_console_dead = 0;     // stdout закрыт (EPIPE) — больше не пишем
static log_sink_fn g_sink = NULL;
static void *g_sink_ctx = NULL;

enum { MODE_ALL = 0, MODE_FILE = 1, MODE_CONSOLE = 2 };

// Полная запись буфера в дескриптор: учитывает частичные записи и EINTR.
static int write_all(int fd, const char *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        const ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

// Вырезать ANSI-последовательности (\033[...m), чтобы журнал был чистым текстом.
static size_t strip_ansi(const char *src, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i < len) {
        if ((unsigned char)src[i] == 0x1B && i + 1 < len && src[i + 1] == '[') {
            i += 2;
            while (i < len && !(src[i] >= '@' && src[i] <= '~')) i++;
            if (i < len) i++;
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = '\0';
    return o;
}

static int log_open_flags(const char *path, int flags) {
    if (g_log_fd >= 0) return -1;
    g_log_fd = open(path, flags, 0644);
    return g_log_fd >= 0 ? 0 : -1;
}

int log_open(const char *path) { return log_open_flags(path, O_CREAT | O_WRONLY | O_TRUNC); }
int log_open_append(const char *path) { return log_open_flags(path, O_CREAT | O_WRONLY | O_APPEND); }

void log_close(void) {
    if (g_log_fd >= 0) close(g_log_fd);
    g_log_fd = -1;
}

void log_set_sink(log_sink_fn fn, void *ctx) { g_sink = fn; g_sink_ctx = ctx; }
void log_set_console(int enabled) { g_console_on = enabled; }

static int emit(int mode, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    if (n < 0) { va_end(ap2); return -1; }

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { va_end(ap2); return -1; }
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);

    const size_t len = (size_t)n;
    int rc = n;

    if ((mode == MODE_ALL || mode == MODE_FILE) && g_log_fd >= 0) {
        char *clean = (char *)malloc(len + 1);
        if (clean) {
            const size_t clen = strip_ansi(buf, len, clean);
            if (write_all(g_log_fd, clean, clen) < 0) rc = -1;
            free(clean);
        }
    }
    // Если консоль недоступна (закрытый канал), вывод в неё отключается,
    // а запись в журнал продолжается: журнал должен быть полным в любом случае.
    if ((mode == MODE_ALL || mode == MODE_CONSOLE) && g_console_on && !g_console_dead) {
        if (write_all(STDOUT_FILENO, buf, len) < 0) g_console_dead = 1;
    }
    if (mode == MODE_ALL && g_sink) g_sink(buf, len, g_sink_ctx);

    free(buf);
    return rc;
}

int log_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = emit(MODE_ALL, fmt, ap);
    va_end(ap);
    return r;
}

int log_console(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = emit(MODE_CONSOLE, fmt, ap);
    va_end(ap);
    return r;
}

int log_file(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const int r = emit(MODE_FILE, fmt, ap);
    va_end(ap);
    return r;
}
