#include "process_simulation.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Использование: %s <session>\n", argv[0]);
        return 1;
    }
    return independent_coordinator_main(argv[1]);
}
