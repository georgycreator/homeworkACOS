#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

// Дескриптор лог-файла; -1, если файл не открыт.
static int g_log_fd = -1;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        pthread_mutex_unlock(&g_log_mutex);
        return -1;
    }
    g_log_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    const int result = (g_log_fd >= 0) ? 0 : -1;
    pthread_mutex_unlock(&g_log_mutex);
    return result;
}

void log_close(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

// Общий движок: mode 0 — в оба, 1 — только в файл, 2 — только в консоль.
// Короткие сообщения форматируются в стеке, длинные (например, строка карты
// большой территории) — в динамическом буфере, поэтому ничего не обрезается.
// Если консоль недоступна (закрытый канал, EPIPE), вывод в неё отключается,
// а запись в лог-файл продолжается: лог должен быть полным в любом случае.
static int g_console_dead = 0;

static int emit(int mode, const char *fmt, va_list ap) {
    pthread_mutex_lock(&g_log_mutex);
    char sbuf[4096];
    char cbuf[4096];
    char *buf = sbuf, *heap = NULL, *clean = cbuf, *clean_heap = NULL;

    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(sbuf, sizeof(sbuf), fmt, ap);
    if (n < 0) { va_end(ap2); pthread_mutex_unlock(&g_log_mutex); return -1; }
    size_t len = (size_t)n;
    if (len >= sizeof(sbuf)) {
        heap = (char *)malloc(len + 1);
        if (heap) {
            vsnprintf(heap, len + 1, fmt, ap2);
            buf = heap;
        } else {
            len = sizeof(sbuf) - 1;   // нет памяти: пишем усечённый вариант
        }
    }
    va_end(ap2);

    int write_rc = 0;
    if ((mode == 0 || mode == 1) && g_log_fd >= 0) {
        size_t ccap = sizeof(cbuf);
        if (len + 1 > ccap) {
            clean_heap = (char *)malloc(len + 1);
            if (clean_heap) { clean = clean_heap; ccap = len + 1; }
        }
        size_t clen = strip_ansi(buf, len, clean, ccap);
        if (write_all(g_log_fd, clean, clen) < 0) write_rc = -1;
    }
    if ((mode == 0 || mode == 2) && !g_console_dead) {
        if (write_all(STDOUT_FILENO, buf, len) < 0) g_console_dead = 1;
    }
    free(heap);
    free(clean_heap);
    const int result = write_rc < 0 ? write_rc : n;
    pthread_mutex_unlock(&g_log_mutex);
    return result;
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
