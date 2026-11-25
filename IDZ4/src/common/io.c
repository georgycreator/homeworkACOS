#define _POSIX_C_SOURCE 200809L
#include "io.h"
#include "signals.h"

#include <errno.h>
#include <unistd.h>

// Построчное чтение через read(). Ошибка системного вызова не маскируется
// под EOF: вызывающий код должен отличать повреждённый/недоступный ввод
// от штатного конца файла.
int io_read_line(int fd, char *buf, size_t cap) {
    if (!buf || cap == 0) {
        errno = EINVAL;
        return IO_LINE_ERROR;
    }

    size_t i = 0;
    int overflow = 0;

    for (;;) {
        char c;
        const ssize_t r = read(fd, &c, 1);

        if (r < 0) {
            if (errno == EINTR) {
                if (signals_stop_requested()) {
                    buf[i] = '\0';
                    return IO_LINE_EOF;
                }
                continue;
            }
            buf[i] = '\0';
            return IO_LINE_ERROR;
        }

        if (r == 0) {
            if (i == 0 && !overflow)
                return IO_LINE_EOF;
            break;
        }

        if (c == '\n')
            break;

        if (i + 1 < cap)
            buf[i++] = c;
        else
            overflow = 1; // дочитываем остаток, чтобы следующая строка не сместилась
    }

    buf[i] = '\0';
    return overflow ? IO_LINE_TOO_LONG : (int)i;
}
