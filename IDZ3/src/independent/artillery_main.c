#include "process_simulation.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[2], "A") != 0 && strcmp(argv[2], "B") != 0)) {
        fprintf(stderr, "Использование: %s <session> A|B\n", argv[0]);
        return 1;
    }
    return independent_worker_main(argv[1], 1, argv[2][0] == 'A' ? 0 : 1);
}
