// io.h — обёртки над системными вызовами ввода-вывода.
// Используются через файловые дескрипторы (POSIX).
#ifndef IO_H
#define IO_H

#include <stddef.h>

// Специальные коды возврата io_read_line.
#define IO_LINE_EOF       (-1)   // конец файла / ошибка чтения
#define IO_LINE_TOO_LONG  (-2)   // строка не влезла в буфер, остаток проглочен

// Прочитать одну строку (до '\n' или EOF) из дескриптора fd.
// Возвращает длину строки (без '\n'), IO_LINE_EOF при EOF/ошибке,
// IO_LINE_TOO_LONG если строка не влезла в буфер.
// Буфер всегда завершается '\0'.
int io_read_line(int fd, char *buf, size_t cap);

#endif // IO_H
