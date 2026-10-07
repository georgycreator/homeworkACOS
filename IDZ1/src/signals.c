#define _POSIX_C_SOURCE 200809L
#include "signals.h"
#include <signal.h>
#include <string.h>
#include <unistd.h>

// Флаги остановки. volatile + sig_atomic_t — стандартный безопасный
// способ общения с обработчиком сигнала.
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_sig  = 0;

// Обработчик: только async-signal-safe действия.
// write() и sizeof строкового литерала безопасны.
// strlen() формально не входит в список async-signal-safe, поэтому
// используем sizeof(msg) - 1 (известно на этапе компиляции).
// ANSI-код здесь не используем, чтобы перенаправленный в файл вывод
// оставался чистым.
static void handler(int sig) {
    g_stop = 1;
    g_sig = sig;
    const char msg[] = "\n[SIGNAL] Получен сигнал завершения, выходим...\n";
    ssize_t unused = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)unused;
}

int signals_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // без SA_RESTART: nanosleep/read должны прерываться

    // Только "пользовательские" сигналы завершения.
    // SIGSEGV/SIGABRT не обрабатываем: если программа реально падает
    // из-за бага — пусть падает, а не притворяется, что "пользователь
    // нажал Ctrl+C".
    if (sigaction(SIGINT,  &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    return 0;
}

int signals_stop_requested(void) { return (int)g_stop; }
int signals_last_signal(void)    { return (int)g_sig;  }
