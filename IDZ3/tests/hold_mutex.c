#define _POSIX_C_SOURCE 200809L
#include "ipc.h"

#include <stdio.h>
#include <unistd.h>

// Вспомогательная программа только для интеграционного теста robust mutex.
// Подключается к независимой сессии, захватывает общий mutex и удерживает его.
// Тест убивает её через SIGKILL, проверяя fail-stop обработку EOWNERDEAD.
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Использование: %s SESSION\n", argv[0]);
        return 2;
    }

    ipc_handle_t ipc;
    if (ipc_attach(&ipc, argv[1]) < 0) {
        perror("ipc_attach");
        return 1;
    }
    if (ipc_lock(ipc.shared) < 0) {
        perror("ipc_lock");
        ipc_close(&ipc);
        return 1;
    }

    puts("LOCKED");
    fflush(stdout);
    for (;;) pause();
}
