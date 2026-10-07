#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "io.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>

// Убрать ведущие и хвостовые пробелы. Возвращает указатель внутрь строки.
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

// Разбор целого числа с проверкой диапазона.
// Возвращает 0 при успехе, -1 при ошибке.
static int parse_int_range(const char *val, int lo, int hi, int *out) {
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0') return -1;
    if (v < (long)lo || v > (long)hi) return -1;
    *out = (int)v;
    return 0;
}

// Разбор строкового имени стратегии.
// При неизвестном значении пишет ошибку и возвращает -1.
static int parse_strategy(const char *v, strategy_t *out) {
    if (strcmp(v, "random") == 0) { *out = STRAT_RANDOM; return 0; }
    if (strcmp(v, "center") == 0) { *out = STRAT_CENTER; return 0; }
    if (strcmp(v, "sector") == 0) { *out = STRAT_SECTOR; return 0; }
    log_printf("[CONFIG] неизвестная стратегия: '%s' "
               "(допустимо: random, center, sector)\n", v);
    return -1;
}

// Разбор строки "r c cost" для цели.
static int parse_target(const char *val, side_t *s) {
    int r, c;
    int64_t cost;
    if (sscanf(val, "%d %d %" SCNd64, &r, &c, &cost) != 3) {
        log_printf("[CONFIG] неверный формат target: '%s' "
                   "(ожидается: строка столбец стоимость)\n", val);
        return -1;
    }
    if (cost < 0) {
        log_printf("[CONFIG] target: стоимость не может быть отрицательной\n");
        return -1;
    }
    if (grid_place_target(&s->grid, r, c, cost) < 0) {
        log_printf("[CONFIG] target (%d,%d) вне поля %dx%d "
                   "(нужно указать rows/cols ДО target)\n",
                   r, c, s->grid.rows, s->grid.cols);
        return -1;
    }
    return 0;
}

int config_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_ticks = 200;
    cfg->tick_ms = 100;
    cfg->seed = 42;
    strncpy(cfg->log_path, "duel.log", sizeof(cfg->log_path) - 1);

    if (side_init(&cfg->A, "Сторона A", 8, 8) < 0) return -1;
    if (side_init(&cfg->B, "Сторона B", 8, 8) < 0) {
        side_free(&cfg->A);
        return -1;
    }

    cfg->A.art.ammo_left=30; cfg->A.art.ammo_cost=2; cfg->A.art.budget_limit=100;
    cfg->A.art.accuracy=70; cfg->A.art.strategy=STRAT_RANDOM;
    cfg->A.art.reaction_ms=200; cfg->A.art.reaction_jitter_ms=100;
    cfg->A.art.area_ammo=2; cfg->A.art.area_cost=20;
    cfg->A.art.area_radius=1; cfg->A.art.area_every=10;

    cfg->B.art.ammo_left=30; cfg->B.art.ammo_cost=2; cfg->B.art.budget_limit=100;
    cfg->B.art.accuracy=70; cfg->B.art.strategy=STRAT_RANDOM;
    cfg->B.art.reaction_ms=200; cfg->B.art.reaction_jitter_ms=100;
    cfg->B.art.area_ammo=2; cfg->B.art.area_cost=20;
    cfg->B.art.area_radius=1; cfg->B.art.area_every=10;

    // Демонстрационные цели — только для режима БЕЗ аргументов.
    // При загрузке из файла config_load() сначала сбрасывает территории.
    grid_place_target(&cfg->A.grid, 1, 1, 10);
    grid_place_target(&cfg->A.grid, 5, 5, 20);
    grid_place_target(&cfg->B.grid, 2, 6, 15);
    grid_place_target(&cfg->B.grid, 6, 2, 25);

    return 0;
}

// Применить один параметр key=value. 0 — успех, -1 — ошибка.
static int apply_kv(config_t *cfg, const char *key, const char *val) {
    int v;

    // ---- Глобальные параметры ----
    if (strcmp(key, "max_ticks") == 0) {
        if (parse_int_range(val, 0, 1000000, &v) < 0) {
            log_printf("[CONFIG] max_ticks: ожидается 0..1000000, получено '%s'\n", val);
            return -1;
        }
        cfg->max_ticks = v;
        return 0;
    }
    if (strcmp(key, "tick_ms") == 0) {
        if (parse_int_range(val, 0, 60000, &v) < 0) {
            log_printf("[CONFIG] tick_ms: ожидается 0..60000, получено '%s'\n", val);
            return -1;
        }
        cfg->tick_ms = v;
        return 0;
    }
    if (strcmp(key, "seed") == 0) {
        if (parse_int_range(val, 0, INT_MAX, &v) < 0) {
            log_printf("[CONFIG] seed: ожидается 0..%d, получено '%s'\n", INT_MAX, val);
            return -1;
        }
        cfg->seed = (unsigned)v;
        return 0;
    }
    if (strcmp(key, "log") == 0) {
        strncpy(cfg->log_path, val, sizeof(cfg->log_path) - 1);
        cfg->log_path[sizeof(cfg->log_path) - 1] = '\0';
        return 0;
    }

    // ---- Параметры стороны (A.xxx или B.xxx) ----
    if (strlen(key) < 3 || key[1] != '.') {
        log_printf("[CONFIG] неизвестный ключ: '%s'\n", key);
        return -1;
    }
    char side_ch = key[0];
    const char *sub = key + 2;
    side_t *s = NULL;
    if (side_ch == 'A' || side_ch == 'a') s = &cfg->A;
    else if (side_ch == 'B' || side_ch == 'b') s = &cfg->B;
    else {
        log_printf("[CONFIG] неизвестная сторона '%c' в ключе '%s'\n", side_ch, key);
        return -1;
    }

    if (strcmp(sub, "name") == 0) {
        strncpy(s->name, val, sizeof(s->name) - 1);
        s->name[sizeof(s->name) - 1] = '\0';
        return 0;
    }
    if (strcmp(sub, "rows") == 0 || strcmp(sub, "cols") == 0) {
        if (s->grid.initial_targets > 0) {
            log_printf("[CONFIG] %s нельзя изменить после добавления целей "
                       "(нужно указывать rows/cols ДО target)\n", key);
            return -1;
        }
        if (parse_int_range(val, 1, 1000, &v) < 0) {
            log_printf("[CONFIG] %s: ожидается 1..1000, получено '%s'\n", key, val);
            return -1;
        }
        if (strcmp(sub, "rows") == 0 && v != s->grid.rows) {
            int c = s->grid.cols;
            grid_free(&s->grid);
            if (grid_init(&s->grid, v, c) < 0) return -1;
        } else if (strcmp(sub, "cols") == 0 && v != s->grid.cols) {
            int r = s->grid.rows;
            grid_free(&s->grid);
            if (grid_init(&s->grid, r, v) < 0) return -1;
        }
        return 0;
    }
    if (strcmp(sub, "ammo") == 0) {
        char *end = NULL;
        long long vv = strtoll(val, &end, 10);
        if (end == val || *end != '\0' || vv < 0 || vv > 100000000LL) {
            log_printf("[CONFIG] ammo: 0..100000000, получено '%s'\n", val);
            return -1;
        }
        s->art.ammo_left = (int64_t)vv;
        return 0;
    }
    if (strcmp(sub, "ammo_cost") == 0) {
        char *end = NULL;
        long long vv = strtoll(val, &end, 10);
        if (end == val || *end != '\0' || vv < 0 || vv > 100000000LL) {
            log_printf("[CONFIG] ammo_cost: 0..100000000, получено '%s'\n", val);
            return -1;
        }
        s->art.ammo_cost = (int64_t)vv;
        return 0;
    }
    if (strcmp(sub, "budget") == 0) {
        char *end = NULL;
        long long vv = strtoll(val, &end, 10);
        if (end == val || *end != '\0' || vv < 0 || vv > 1000000000000LL) {
            log_printf("[CONFIG] budget: 0..1000000000000, получено '%s'\n", val);
            return -1;
        }
        s->art.budget_limit = (int64_t)vv;
        return 0;
    }
    if (strcmp(sub, "accuracy") == 0) {
        if (parse_int_range(val, 0, 100, &v) < 0) {
            log_printf("[CONFIG] accuracy: 0..100, получено '%s'\n", val);
            return -1;
        }
        s->art.accuracy = v;
        return 0;
    }
    if (strcmp(sub, "strategy") == 0) {
        return parse_strategy(val, &s->art.strategy);
    }
    if (strcmp(sub, "replenish_every") == 0) {
        if (parse_int_range(val, 0, 1000000, &v) < 0) {
            log_printf("[CONFIG] replenish_every: 0..1000000, получено '%s'\n", val);
            return -1;
        }
        s->art.replenish_every = v;
        return 0;
    }
    if (strcmp(sub, "replenish_amount") == 0) {
        if (parse_int_range(val, 0, 100000, &v) < 0) {
            log_printf("[CONFIG] replenish_amount: 0..100000, получено '%s'\n", val);
            return -1;
        }
        s->art.replenish_amount = v;
        return 0;
    }
    if (strcmp(sub, "reaction_ms") == 0) {
        if (parse_int_range(val, 0, 60000, &v) < 0) {
            log_printf("[CONFIG] reaction_ms: 0..60000, получено '%s'\n", val);
            return -1;
        }
        s->art.reaction_ms = v;
        return 0;
    }
    if (strcmp(sub, "reaction_jitter_ms") == 0) {
        if (parse_int_range(val, 0, 60000, &v) < 0) {
            log_printf("[CONFIG] reaction_jitter_ms: 0..60000, получено '%s'\n", val);
            return -1;
        }
        s->art.reaction_jitter_ms = v;
        return 0;
    }
    if (strcmp(sub, "area_ammo") == 0) {
        if (parse_int_range(val, 0, 100000, &v) < 0) {
            log_printf("[CONFIG] area_ammo: 0..100000, получено '%s'\n", val);
            return -1;
        }
        s->art.area_ammo = v;
        return 0;
    }
    if (strcmp(sub, "area_cost") == 0) {
        if (parse_int_range(val, 0, 100000, &v) < 0) {
            log_printf("[CONFIG] area_cost: 0..100000, получено '%s'\n", val);
            return -1;
        }
        s->art.area_cost = v;
        return 0;
    }
    if (strcmp(sub, "area_radius") == 0) {
        if (parse_int_range(val, 0, 100, &v) < 0) {
            log_printf("[CONFIG] area_radius: 0..100, получено '%s'\n", val);
            return -1;
        }
        s->art.area_radius = v;
        return 0;
    }
    if (strcmp(sub, "area_every") == 0) {
        if (parse_int_range(val, 0, 1000000, &v) < 0) {
            log_printf("[CONFIG] area_every: 0..1000000, получено '%s'\n", val);
            return -1;
        }
        s->art.area_every = v;
        return 0;
    }
    if (strcmp(sub, "target") == 0) {
        return parse_target(val, s);
    }

    log_printf("[CONFIG] неизвестный параметр: '%s'\n", key);
    return -1;
}

int config_load(config_t *cfg, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_printf("[CONFIG] Не удалось открыть: %s\n", path);
        return -1;
    }

    // Сбрасываем территории обеих сторон, чтобы дефолтные
    // демонстрационные цели не попали в пользовательский сценарий.
    // Размер по умолчанию — 8x8; если конфиг задаст rows/cols,
    // они переопределят его до добавления целей.
    grid_free(&cfg->A.grid);
    if (grid_init(&cfg->A.grid, 8, 8) < 0) { close(fd); return -1; }
    grid_free(&cfg->B.grid);
    if (grid_init(&cfg->B.grid, 8, 8) < 0) { close(fd); return -1; }

    char line[512];
    int  line_no = 0;
    int  had_error = 0;

    while (1) {
        int rc = io_read_line(fd, line, sizeof(line));
        if (rc == IO_LINE_EOF) break;
        line_no++;
        if (rc == IO_LINE_TOO_LONG) {
            log_printf("[CONFIG] Строка %d: слишком длинная (>%zu символов)\n",
                       line_no, sizeof(line) - 1);
            had_error = 1;
            continue;
        }
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            log_printf("[CONFIG] Строка %d: нет '='\n", line_no);
            had_error = 1;
            continue;
        }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (apply_kv(cfg, key, val) < 0) {
            log_printf("[CONFIG] Строка %d: ошибка в '%s'\n", line_no, key);
            had_error = 1;
        }
    }
    close(fd);
    return had_error ? -1 : 0;
}

// Прочитать строку и разобрать как int в диапазоне lo..hi.
// Пустая строка — оставить значение как есть.
// При ошибке пишет сообщение и НЕ меняет значение.
// Возвращает -1, если ввод прерван сигналом или EOF.
static int read_int_field(const char *prompt, int lo, int hi, int *out) {
    char line[128];
    log_console("%s [%d]: ", prompt, *out);
    int rc = io_read_line(0, line, sizeof(line));
    if (rc == IO_LINE_EOF) return -1;
    if (rc == IO_LINE_TOO_LONG) {
        log_console("  Строка слишком длинная, значение не изменено\n");
        return 0;
    }
    if (line[0] == '\0') return 0;   // Enter — оставить как есть
    int v;
    if (parse_int_range(line, lo, hi, &v) < 0) {
        log_console("  Ошибка: значение должно быть в диапазоне %d..%d, "
                    "оставлено %d\n", lo, hi, *out);
        return 0;
    }
    *out = v;
    return 0;
}

// Интерактивный ввод размеров территории стороны.
// При изменении размера сетка пересоздаётся, прежние цели теряются.
static int read_side_size(side_t *s) {
    char prompt[96];
    int rows = s->grid.rows, cols = s->grid.cols;

    snprintf(prompt, sizeof(prompt), "%s: число строк (1..1000)", s->name);
    if (read_int_field(prompt, 1, 1000, &rows) < 0) return -1;
    snprintf(prompt, sizeof(prompt), "%s: число столбцов (1..1000)", s->name);
    if (read_int_field(prompt, 1, 1000, &cols) < 0) return -1;

    if (rows != s->grid.rows || cols != s->grid.cols) {
        grid_free(&s->grid);
        if (grid_init(&s->grid, rows, cols) < 0) return -1;
        log_console("  Размер изменён: прежние цели сброшены.\n");
    }
    return 0;
}

// Интерактивный ввод целей: «строка столбец стоимость», одна в строке.
// Пустая строка — конец. Если сразу Enter — текущие цели остаются.
// Если введена хоть одна — прежний набор заменяется новым.
static int read_side_targets(side_t *s) {
    char line[128];
    int entered = 0;

    log_console("%s: целей сейчас %d. Введите цели «строка столбец стоимость»\n"
                "  (строки 0..%d, столбцы 0..%d, стоимость 0..1000000000).\n"
                "  Пустая строка — закончить (Enter — оставить текущие).\n",
                s->name, s->grid.initial_targets,
                s->grid.rows - 1, s->grid.cols - 1);

    while (1) {
        log_console("  цель %d: ", entered + 1);
        int rc = io_read_line(0, line, sizeof(line));
        if (rc == IO_LINE_EOF) return -1;
        if (rc == IO_LINE_TOO_LONG) {
            log_console("  Строка слишком длинная, цель не добавлена\n");
            continue;
        }
        if (line[0] == '\0') break;

        int r, c, n = 0;
        int64_t cost = 0;
        if (sscanf(line, "%d %d %" SCNd64 " %n", &r, &c, &cost, &n) != 3
            || line[n] != '\0') {
            log_console("  Ошибка: ожидается «строка столбец стоимость»\n");
            continue;
        }
        if (cost < 0 || cost > 1000000000LL) {
            log_console("  Ошибка: стоимость 0..1000000000\n");
            continue;
        }
        if (!grid_at(&s->grid, r, c)) {
            log_console("  Ошибка: (%d,%d) вне поля %dx%d\n",
                        r, c, s->grid.rows, s->grid.cols);
            continue;
        }
        if (entered == 0 && s->grid.initial_targets > 0) {
            int rows = s->grid.rows, cols = s->grid.cols;
            grid_free(&s->grid);
            if (grid_init(&s->grid, rows, cols) < 0) return -1;
        }
        if (grid_place_target(&s->grid, r, c, (int64_t)cost) < 0) {
            log_console("  Ошибка: клетка (%d,%d) уже занята\n", r, c);
            continue;
        }
        entered++;
    }

    if (entered > 0) log_console("  Задано целей: %d\n", entered);
    if (s->grid.initial_targets == 0)
        log_console("  Внимание: целей нет — сторона сразу прекратит огонь\n");
    return 0;
}

int config_read_stdin(config_t *cfg) {
    log_console("\n=== Интерактивный ввод (Enter — по умолчанию) ===\n");
    if (read_int_field("Максимум тактов (0 = без ограничения)",
                       0, 1000000, &cfg->max_ticks) < 0) return -1;
    if (read_int_field("Задержка, мс", 0, 60000, &cfg->tick_ms) < 0) return -1;
    if (read_int_field("Точность A, %", 0, 100, &cfg->A.art.accuracy) < 0) return -1;
    if (read_int_field("Точность B, %", 0, 100, &cfg->B.art.accuracy) < 0) return -1;
    // Размеры территорий и цели (размер — раньше целей).
    if (read_side_size(&cfg->A) < 0) return -1;
    if (read_side_targets(&cfg->A) < 0) return -1;
    if (read_side_size(&cfg->B) < 0) return -1;
    if (read_side_targets(&cfg->B) < 0) return -1;
    return 0;
}

void config_free(config_t *cfg) {
    side_free(&cfg->A);
    side_free(&cfg->B);
}

static const char *strategy_name(strategy_t s) {
    switch (s) {
        case STRAT_RANDOM: return "random";
        case STRAT_CENTER: return "center";
        case STRAT_SECTOR: return "sector";
    }
    return "?";
}

static void print_side_params(const char *label, const side_t *s) {
    log_printf("%s:\n", label);
    log_printf("  Поле: %dx%d, целей %d (стоимость %" PRId64 "), живых %d\n",
               s->grid.rows, s->grid.cols,
               s->grid.initial_targets, s->grid.initial_cost,
               s->grid.alive_targets);
    log_printf("  Обычные снаряды: %" PRId64 ", цена выстрела: %" PRId64 "\n",
               s->art.ammo_left, s->art.ammo_cost);
    log_printf("  Бюджет: %" PRId64 " (0 = без лимита), израсходовано: %" PRId64 "\n",
               s->art.budget_limit, s->art.spent_cost);
    log_printf("  Точность: %d%%, стратегия: %s\n",
               s->art.accuracy, strategy_name(s->art.strategy));
    log_printf("  Реакция: %d±%d мс\n",
               s->art.reaction_ms, s->art.reaction_jitter_ms);
    if (s->art.replenish_every > 0)
        log_printf("  Пополнение: +%d каждые %d тактов\n",
                   s->art.replenish_amount, s->art.replenish_every);
    else
        log_printf("  Пополнение: не настроено\n");
    log_printf("  Тяжёлые: %d, стоимость %d, радиус %d, каждые %d тактов\n",
               s->art.area_ammo, s->art.area_cost,
               s->art.area_radius, s->art.area_every);
}

void config_print(const config_t *cfg) {
    log_printf("=== Параметры ===\n");
    log_printf("Тактов: %d, задержка такта: %d мс, seed: %u\n",
               cfg->max_ticks, cfg->tick_ms, cfg->seed);
    log_printf("Лог-файл: %s\n", cfg->log_path);
    print_side_params("Сторона A", &cfg->A);
    print_side_params("Сторона B", &cfg->B);
}
