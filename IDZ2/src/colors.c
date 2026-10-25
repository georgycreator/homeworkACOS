#define _POSIX_C_SOURCE 200809L
#include "colors.h"
#include <unistd.h>

// Значения по умолчанию — все пустые, включаются в colors_init().
const char *C_RESET   = "";
const char *C_BOLD    = "";
const char *C_DIM     = "";
const char *C_RED     = "";
const char *C_GREEN   = "";
const char *C_YELLOW  = "";
const char *C_BLUE    = "";
const char *C_MAGENTA = "";
const char *C_CYAN    = "";
const char *C_WHITE   = "";

// Включить/выключить цвета. Устанавливает указатели на ANSI-коды
// или на пустые строки в зависимости от параметра.
static void colors_set_enabled(int enabled) {
    if (enabled) {
        C_RESET   = "\033[0m";
        C_BOLD    = "\033[1m";
        C_DIM     = "\033[2m";
        C_RED     = "\033[31m";
        C_GREEN   = "\033[32m";
        C_YELLOW  = "\033[33m";
        C_BLUE    = "\033[34m";
        C_MAGENTA = "\033[35m";
        C_CYAN    = "\033[36m";
        C_WHITE   = "\033[37m";
    } else {
        C_RESET = C_BOLD = C_DIM = "";
        C_RED = C_GREEN = C_YELLOW = C_BLUE = "";
        C_MAGENTA = C_CYAN = C_WHITE = "";
    }
}

// Определяем, идёт ли вывод в терминал.
// Если stdout перенаправлен в файл или pipe — цвета не нужны.
void colors_init(void) {
    colors_set_enabled(isatty(STDOUT_FILENO) ? 1 : 0);
}
