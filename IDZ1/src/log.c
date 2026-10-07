#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// Дескриптор лог-файла; -1, если файл не открыт.
static int g_log_fd = -1;

// Полная запись буфера в дескриптор — обрабатывает частичные записи и EINTR.
static int write_all(int fd, const char *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) {
            // write() вернул 0 на непустом буфере — нештатная
            // ситуация; выходим, чтобы не зациклиться.
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

// Вырезает ANSI escape-последовательности (\033[...m) из src.
// Нужно, чтобы лог-файл оставался чистым текстом без мусора.
static size_t strip_ansi(const char *src, size_t len, char *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < len && o + 1 < cap) {
        if ((unsigned char)src[i] == 0x1B && i + 1 < len && src[i+1] == '[') {
            i += 2;
            while (i < len && !(src[i] >= '@' && src[i] <= '~')) i++;
            if (i < len) i++;
        } else out[o++] = src[i++];
    }
    out[o] = '\0';
    return o;
}

int log_open(const char *path) {
    if (g_log_fd >= 0) return -1;
    g_log_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    return (g_log_fd >= 0) ? 0 : -1;
}

void log_close(void) {
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

// Общий движок: mode 0 — в оба, 1 — только в файл, 2 — только в консоль.
static int emit(int mode, const char *fmt, va_list ap) {
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return -1;

    size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;

    int write_rc = 0;
    if ((mode == 0 || mode == 1) && g_log_fd >= 0) {
        char clean[4096];
        size_t clen = strip_ansi(buf, len, clean, sizeof(clean));
        if (write_all(g_log_fd, clean, clen) < 0) write_rc = -1;
    }
    if (mode == 0 || mode == 2) {
        if (write_all(STDOUT_FILENO, buf, len) < 0) write_rc = -1;
    }
    return write_rc < 0 ? write_rc : n;
}

int log_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = emit(0, fmt, ap);
    va_end(ap);
    return r;
}

int log_console(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = emit(2, fmt, ap);
    va_end(ap);
    return r;
}
