// util.h — мелкие общие утилиты: динамическая строка, время, разбор чисел.
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

// Динамический строковый буфер. Нужен, чтобы формировать многострочные
// сообщения (карты, статистику) один раз и затем и выводить, и отправлять.
typedef struct {
    char  *d;     // данные (всегда завершены '\0' после первой записи)
    size_t len;   // длина без '\0'
    size_t cap;   // выделено байт
} sbuf_t;

void sb_init(sbuf_t *sb);
void sb_free(sbuf_t *sb);
void sb_clear(sbuf_t *sb);
// Добавить отформатированный текст. Возвращает число добавленных байт или -1.
int  sb_printf(sbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Содержимое буфера как C-строка (никогда не NULL).
const char *sb_str(const sbuf_t *sb);

// Монотонное время в миллисекундах.
long now_ms(void);
// Сон с прерыванием по сигналу остановки (проверяется каждые ~10 мс).
void sleep_ms(long ms);

// Строгий разбор целого в диапазоне lo..hi. 0 — успех, -1 — ошибка.
int  util_parse_long(const char *s, long lo, long hi, long *out);
// Разбить строку на слова по пробелам (портит исходную строку).
// Возвращает число слов (не более max).
int  util_split(char *line, char **argv, int max);

#endif // UTIL_H
