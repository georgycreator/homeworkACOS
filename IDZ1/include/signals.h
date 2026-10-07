// signals.h — установка и опрос обработчиков сигналов.
// Гарантирует корректное завершение при SIGINT (Ctrl+C) и SIGTERM.
#ifndef SIGNALS_H
#define SIGNALS_H

// Установить обработчики. Возвращает 0 при успехе, -1 при ошибке.
int signals_install(void);

// 1, если был получен сигнал завершения.
int signals_stop_requested(void);

// Номер последнего полученного сигнала (0 — не было).
int signals_last_signal(void);

#endif // SIGNALS_H
