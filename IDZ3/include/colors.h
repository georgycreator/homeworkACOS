// colors.h — ANSI-цвета для вывода в терминал.
// Отключаются автоматически, если stdout не является терминалом.
#ifndef COLORS_H
#define COLORS_H

// Инициализация: вызывается один раз в начале main.
// Проверяет isatty(STDOUT_FILENO) и включает/выключает цвета.
void colors_init(void);

// Внешние строки с ANSI-последовательностями.
// Если цвета выключены — содержат пустые строки "".
extern const char *C_RESET;
extern const char *C_BOLD;
extern const char *C_DIM;
extern const char *C_RED;
extern const char *C_GREEN;
extern const char *C_YELLOW;
extern const char *C_BLUE;
extern const char *C_MAGENTA;
extern const char *C_CYAN;
extern const char *C_WHITE;

#endif // COLORS_H
