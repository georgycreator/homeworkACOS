// config.h — параметры моделирования и их загрузка.
#ifndef CONFIG_H
#define CONFIG_H

#include "side.h"

typedef struct {
    int          max_ticks;     // 0 = без ограничения по тактам
    int          tick_ms;       // базовая задержка между тактами, мс
    unsigned int seed;          // seed генератора случайных чисел
    char         log_path[256]; // имя лог-файла
    side_t       A;             // государство A
    side_t       B;             // государство B
} config_t;

// Заполнить значениями по умолчанию.
int  config_defaults(config_t *cfg);

// Загрузить из файла. Возвращает 0 при успехе, -1 при ошибке.
int  config_load(config_t *cfg, const char *path);

// Интерактивный ввод с клавиатуры (fd 0).
int  config_read_stdin(config_t *cfg);

// Освободить ресурсы.
void config_free(config_t *cfg);

// Напечатать текущие параметры.
void config_print(const config_t *cfg);

#endif // CONFIG_H
