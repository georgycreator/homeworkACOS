#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "io.h"
#include "signals.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>

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

// Разбор одного целого токена. strtoimax + ERANGE не допускает
// неопределённого поведения scanf при числах за пределами int64_t.
static int parse_i64_token(const char **cursor, int64_t *out) {
    const char *p = *cursor;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return -1;

    errno = 0;
    char *end = NULL;
    intmax_t value = strtoimax(p, &end, 10);
    if (end == p || errno == ERANGE || value < INT64_MIN || value > INT64_MAX)
        return -1;
    if (*end != '\0' && !isspace((unsigned char)*end)) return -1;

    *out = (int64_t)value;
    *cursor = end;
    return 0;
}

// Разбор строки цели: «строка столбец стоимость [тип]». Тип — одно слово.
// Числовые токены разбираются с явной проверкой диапазона, включая числа,
// которые не помещаются даже в int64_t.
static int parse_target_line(const char *val, int64_t *r, int64_t *c,
                             int64_t *cost, char *type, size_t type_cap) {
    const char *p = val;
    if (!type_cap) return -1;
    type[0] = '\0';
    if (parse_i64_token(&p, r) < 0 ||
        parse_i64_token(&p, c) < 0 || parse_i64_token(&p, cost) < 0)
        return -1;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return 0; // Тип не задан: используется значение по умолчанию.

    const char *start = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    size_t len = (size_t)(p - start);
    if (len == 0 || len >= type_cap) return -1;
    memcpy(type, start, len);
    type[len] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;
    return *p == '\0' ? 0 : -1;
}

// Описание причины, по которой цель не может быть размещена (NULL — причина в таблице типов).
static const char *target_reject_reason(side_t *s, int64_t r, int64_t c, int64_t cost,
                                        int check_occupied) {
    if (cost < 0 || cost > GRID_MAX_TARGET_COST) return "стоимость должна быть в диапазоне 0..1000000000";
    if (r < 0 || r >= s->grid.rows || c < 0 || c >= s->grid.cols)
        return "координаты вне поля (нужно указать rows/cols ДО target)";
    cell_t *cell = grid_at(&s->grid, (int)r, (int)c);
    if (check_occupied && cell && cell->state != CELL_EMPTY) return "клетка уже занята";
    return NULL;
}

// Разместить цель с понятным сообщением об ошибке. 0 — успех, -1 — ошибка.
static int place_target_verbose(side_t *s, int64_t r, int64_t c, int64_t cost,
                                const char *type, int to_console) {
    const char *why = target_reject_reason(s, r, c, cost, 1);
    if (!why && grid_place_target(&s->grid, (int)r, (int)c, cost, type) == 0) return 0;
    if (!why) why = "слишком много разных типов целей (максимум 16 на территорию)";
    if (to_console)
        log_console("  Ошибка: цель (%" PRId64 ",%" PRId64 "): %s\n", r, c, why);
    else
        log_printf("[CONFIG] target (%" PRId64 ",%" PRId64 ") на поле %dx%d: %s\n",
                   r, c, s->grid.rows, s->grid.cols, why);
    return -1;
}

// Разбор значения ключа target из конфигурационного файла.
static int parse_target(const char *val, side_t *s) {
    int64_t r, c, cost;
    char type[GRID_TYPE_NAME_LEN];
    if (parse_target_line(val, &r, &c, &cost, type, sizeof(type)) < 0) {
        log_printf("[CONFIG] неверный формат target: '%s' "
                   "(ожидается: строка столбец стоимость [тип])\n", val);
        return -1;
    }
    return place_target_verbose(s, r, c, cost, type, 0);
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
    grid_place_target(&cfg->A.grid, 1, 1, 10, "штаб");
    grid_place_target(&cfg->A.grid, 5, 5, 20, "склад");
    grid_place_target(&cfg->B.grid, 2, 6, 15, "батарея");
    grid_place_target(&cfg->B.grid, 6, 2, 25, "склад");

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

// Интерактивный ввод целей: «строка столбец стоимость [тип]», одна в строке.
// Пустая строка — конец. Если сразу Enter — текущие цели остаются.
// Если введена хоть одна — прежний набор заменяется новым.
static int read_side_targets(side_t *s) {
    char line[128];
    int entered = 0;

    log_console("%s: целей сейчас %d. Введите цели «строка столбец стоимость [тип]»\n"
                "  (строки 0..%d, столбцы 0..%d, стоимость 0..%d, тип — одно слово, например: штаб).\n"
                "  Пустая строка — закончить (Enter — оставить текущие).\n",
                s->name, s->grid.initial_targets,
                s->grid.rows - 1, s->grid.cols - 1, GRID_MAX_TARGET_COST);

    while (1) {
        log_console("  цель %d: ", entered + 1);
        int rc = io_read_line(0, line, sizeof(line));
        if (rc == IO_LINE_EOF) return -1;
        if (rc == IO_LINE_TOO_LONG) {
            log_console("  Строка слишком длинная, цель не добавлена\n");
            continue;
        }
        if (line[0] == '\0') break;

        int64_t r, c, cost;
        char type[GRID_TYPE_NAME_LEN];
        if (parse_target_line(line, &r, &c, &cost, type, sizeof(type)) < 0) {
            log_console("  Ошибка: ожидается «строка столбец стоимость [тип]»\n");
            continue;
        }
        if (entered == 0 && s->grid.initial_targets > 0) {
            // Первая новая цель заменяет прежний набор. Корректность проверяется
            // ДО сброса, чтобы неверный ввод не стёр прежние цели; занятость клетки
            // старыми целями не учитывается — они будут сброшены.
            const char *why = target_reject_reason(s, r, c, cost, 0);
            if (why) {
                log_console("  Ошибка: цель (%" PRId64 ",%" PRId64 "): %s\n", r, c, why);
                continue;
            }
            int rows = s->grid.rows, cols = s->grid.cols;
            grid_free(&s->grid);
            if (grid_init(&s->grid, rows, cols) < 0) return -1;
        }
        if (place_target_verbose(s, r, c, cost, type, 1) < 0) continue;
        entered++;
    }

    if (entered > 0) log_console("  Задано целей: %d\n", entered);
    if (s->grid.initial_targets == 0)
        log_console("  Внимание: целей нет — сторона сразу прекратит огонь\n");
    return 0;
}

// Ввод одного параметра в формате «ключ=значение» с повторным использованием
// apply_kv(): правила проверки диапазонов одни и те же для файла и клавиатуры.
// Пустая строка — оставить текущее значение. -1 — конец ввода или сигнал.
static int read_kv_field(config_t *cfg, const char *key, const char *prompt,
                         const char *current) {
    char line[128];
    log_console("%s [%s]: ", prompt, current);
    int rc = io_read_line(0, line, sizeof(line));
    if (rc == IO_LINE_EOF) return -1;
    if (rc == IO_LINE_TOO_LONG) {
        log_console("  Строка слишком длинная, значение не изменено\n");
        return 0;
    }
    char *v = trim(line);
    if (*v == '\0') return 0;
    (void)apply_kv(cfg, key, v);   // при ошибке apply_kv сам сообщает причину и ничего не меняет
    return 0;
}

static const char *strategy_name(strategy_t s);

// Расширенные параметры стороны: боезапас, бюджет, стратегия, пополнение, тяжёлые снаряды.
static int read_side_extended(config_t *cfg, char tag) {
    side_t *s = tag == 'A' ? &cfg->A : &cfg->B;
    char key[32], prompt[96], cur[48];
    struct { const char *name; const char *title; } fields[] = {
        {"ammo", "число обычных снарядов"}, {"ammo_cost", "цена обычного снаряда"},
        {"budget", "бюджет (0 = без лимита)"}, {"strategy", "стратегия (random/center/sector)"},
        {"replenish_every", "пополнение: каждые N тактов (0 = нет)"},
        {"replenish_amount", "пополнение: сколько снарядов"},
        {"area_ammo", "тяжёлых снарядов"}, {"area_cost", "цена тяжёлого снаряда"},
        {"area_radius", "радиус тяжёлого залпа"}, {"area_every", "период тяжёлых залпов, тактов (0 = нет)"},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const char *f = fields[i].name;
        if (strcmp(f, "ammo") == 0) snprintf(cur, sizeof(cur), "%" PRId64, s->art.ammo_left);
        else if (strcmp(f, "ammo_cost") == 0) snprintf(cur, sizeof(cur), "%" PRId64, s->art.ammo_cost);
        else if (strcmp(f, "budget") == 0) snprintf(cur, sizeof(cur), "%" PRId64, s->art.budget_limit);
        else if (strcmp(f, "strategy") == 0) snprintf(cur, sizeof(cur), "%s", strategy_name(s->art.strategy));
        else if (strcmp(f, "replenish_every") == 0) snprintf(cur, sizeof(cur), "%d", s->art.replenish_every);
        else if (strcmp(f, "replenish_amount") == 0) snprintf(cur, sizeof(cur), "%d", s->art.replenish_amount);
        else if (strcmp(f, "area_ammo") == 0) snprintf(cur, sizeof(cur), "%d", s->art.area_ammo);
        else if (strcmp(f, "area_cost") == 0) snprintf(cur, sizeof(cur), "%d", s->art.area_cost);
        else if (strcmp(f, "area_radius") == 0) snprintf(cur, sizeof(cur), "%d", s->art.area_radius);
        else snprintf(cur, sizeof(cur), "%d", s->art.area_every);
        snprintf(key, sizeof(key), "%c.%s", tag, f);
        snprintf(prompt, sizeof(prompt), "%s: %s", s->name, fields[i].title);
        if (read_kv_field(cfg, key, prompt, cur) < 0) return -1;
    }
    return 0;
}

// Вопрос «да/нет». Enter и конец ввода трактуются как «нет»; сигнал — как прерывание.
static int ask_yes_no(const char *prompt, int *answer) {
    char line[64];
    *answer = 0;
    log_console("%s [н]: ", prompt);
    int rc = io_read_line(0, line, sizeof(line));
    if (rc == IO_LINE_EOF) return signals_stop_requested() ? -1 : 0;
    if (rc > 0 && (line[0] == 'y' || line[0] == 'Y' ||
                   strncmp(line, "д", 2) == 0 || strncmp(line, "Д", 2) == 0))
        *answer = 1;
    return 0;
}

int config_read_stdin(config_t *cfg) {
    log_console("\n=== Интерактивный ввод (Enter — по умолчанию) ===\n");
    if (read_int_field("Максимум тактов (0 = без ограничения)",
                       0, 1000000, &cfg->max_ticks) < 0) return -1;
    if (read_int_field("Задержка, мс", 0, 60000, &cfg->tick_ms) < 0) return -1;
    if (read_int_field("Точность A, %", 0, 100, &cfg->A.art.accuracy) < 0) return -1;
    if (read_int_field("Точность B, %", 0, 100, &cfg->B.art.accuracy) < 0) return -1;
    /* Скорость артиллерии задаётся отдельно от скорости тактов. */
    if (read_int_field("Реакция A, мс", 0, 60000, &cfg->A.art.reaction_ms) < 0) return -1;
    if (read_int_field("Разброс реакции A, мс", 0, 60000,
                       &cfg->A.art.reaction_jitter_ms) < 0) return -1;
    if (read_int_field("Реакция B, мс", 0, 60000, &cfg->B.art.reaction_ms) < 0) return -1;
    if (read_int_field("Разброс реакции B, мс", 0, 60000,
                       &cfg->B.art.reaction_jitter_ms) < 0) return -1;
    // Размеры территорий и цели (размер — раньше целей).
    if (read_side_size(&cfg->A) < 0) return -1;
    if (read_side_targets(&cfg->A) < 0) return -1;
    if (read_side_size(&cfg->B) < 0) return -1;
    if (read_side_targets(&cfg->B) < 0) return -1;

    // Остальные параметры модели — по желанию (Enter — пропустить).
    int more = 0;
    if (ask_yes_no("Изменить расширенные параметры (seed, боезапас, бюджет, стратегия, "
                   "пополнение, тяжёлые снаряды)? (д/н)", &more) < 0) return -1;
    if (more) {
        char cur[32];
        snprintf(cur, sizeof(cur), "%u", cfg->seed);
        if (read_kv_field(cfg, "seed", "Начальное значение ГСЧ (seed)", cur) < 0) return -1;
        if (read_side_extended(cfg, 'A') < 0) return -1;
        if (read_side_extended(cfg, 'B') < 0) return -1;
    }
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
    log_printf("  Цели:\n");
    grid_print_targets(&s->grid);
}

void config_print(const config_t *cfg) {
    log_printf("=== Параметры ===\n");
    log_printf("Тактов: %d, задержка такта: %d мс, seed: %u\n",
               cfg->max_ticks, cfg->tick_ms, cfg->seed);
    log_printf("Лог-файл: %s\n", cfg->log_path);
    if (cfg->tick_ms <= 0)
        log_printf("Режим tick_ms=0: быстрое моделирование — каждая сторона стреляет "
                   "не более одного раза за такт, задержки реакции не применяются\n");
    print_side_params("Сторона A", &cfg->A);
    print_side_params("Сторона B", &cfg->B);
}
