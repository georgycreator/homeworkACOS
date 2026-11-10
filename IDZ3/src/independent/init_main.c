#include "process_simulation.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Использование: %s <session> [config.conf]\n", argv[0]);
        return 1;
    }
    return independent_init_main(argv[1], argc == 3 ? argv[2] : NULL);
}
