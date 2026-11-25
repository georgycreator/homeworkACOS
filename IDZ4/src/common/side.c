#define _POSIX_C_SOURCE 200809L
#include "side.h"
#include <inttypes.h>
#include <string.h>

int side_init(side_t *s, const char *name, int rows, int cols) {
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    if (grid_init(&s->grid, rows, cols) < 0) return -1;
    artillery_init(&s->art);
    s->active = 1;
    return 0;
}

void side_free(side_t *s) {
    grid_free(&s->grid);
}

void side_render_stats(const side_t *s, sbuf_t *sb) {
    sb_printf(sb, "=== Государство: %s ===\n", s->name);
    sb_printf(sb, "  Своих целей: живых %d, стоимость %" PRId64 "\n",
              s->grid.alive_targets, s->grid.alive_cost);
    sb_printf(sb, "  Потеряно своих целей на сумму: %" PRId64 "\n", s->lost_cost);
    sb_printf(sb, "  Активна: %s\n", s->active ? "да" : "нет");
    artillery_render_stats(&s->art, s->name, sb);
}
