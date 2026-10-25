#define _POSIX_C_SOURCE 200809L
#include "signals.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

// Флаги остановки. Атомарные типы C11 (lock-free, поэтому допустимы в обработчике
// сигнала) исключают гонку данных между обработчиком и потоками, которые читают
// флаг; ThreadSanitizer считает гонкой обычный volatile sig_atomic_t.
static atomic_int g_stop = 0;
static atomic_int g_sig  = 0;

// Обработчик: только async-signal-safe действия.
// write() и sizeof строкового литерала безопасны.
// strlen() формально не входит в список async-signal-safe, поэтому
// используем sizeof(msg) - 1 (известно на этапе компиляции).
// ANSI-код здесь не используем, чтобы перенаправленный в файл вывод
// оставался чистым.
// errno сохраняется и восстанавливается: write() может его изменить, а
// прерванный сигналом код проверяет errno после возврата из обработчика
// (например, sem_wait/nanosleep различают EINTR по errno).
static void handler(int sig) {
    const int saved_errno = errno;
    atomic_store(&g_sig, sig);   // номер сигнала записывается раньше флага остановки
    atomic_store(&g_stop, 1);
    const char msg[] = "\n[SIGNAL] Получен сигнал завершения, выходим...\n";
    ssize_t unused = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)unused;
    errno = saved_errno;
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

    // SIGPIPE игнорируется: при закрытом получателе вывода (`prog | head`)
    // write() вернёт EPIPE, консольный вывод отключится, а моделирование
    // и запись лог-файла завершатся штатно. Без этого процесс молча убивался
    // сигналом, и лог обрывался на середине.
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    if (sigaction(SIGPIPE, &ign, NULL) < 0) return -1;
    return 0;
}

int signals_stop_requested(void) { return atomic_load(&g_stop); }
int signals_last_signal(void)    { return atomic_load(&g_sig); }
