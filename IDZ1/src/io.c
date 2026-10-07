#define _POSIX_C_SOURCE 200809L
#include "io.h"
#include "signals.h"
#include <unistd.h>
#include <errno.h>

// Построчное чтение через read().
// Длинные строки не разбиваются на части: излишек проглатывается
// до '\n', функция возвращает IO_LINE_TOO_LONG. Парсер конфигурации
// увидит это как ошибку строки, а не как две разные строки.
int io_read_line(int fd, char *buf, size_t cap) {
    if (cap == 0) return IO_LINE_EOF;
    size_t i = 0;
    int overflow = 0;
    while (1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) {
                if (signals_stop_requested()) return IO_LINE_EOF;
                continue;
            }
            return IO_LINE_EOF;
        }
        if (r == 0) {
            if (i == 0 && !overflow) return IO_LINE_EOF;
            break;
        }
        if (c == '\n') break;
        if (i + 1 < cap) {
            buf[i++] = c;
        } else {
            overflow = 1;   // буфер полон, но дочитываем строку до конца
        }
    }
    buf[i] = '\0';
    return overflow ? IO_LINE_TOO_LONG : (int)i;
}
