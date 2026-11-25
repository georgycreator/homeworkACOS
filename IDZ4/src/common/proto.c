#define _POSIX_C_SOURCE 200809L
#include "proto.h"
#include <string.h>

static const char *const role_names[] = {
    "side-A", "side-B", "gun-A", "gun-B", "observer"
};

const char *role_name(int role) {
    if (role < 0 || role > ROLE_OBSERVER) return "?";
    return role_names[role];
}

int role_parse(const char *name) {
    if (!name) return ROLE_NONE;
    for (int i = 0; i <= ROLE_OBSERVER; i++)
        if (strcmp(name, role_names[i]) == 0) return i;
    return ROLE_NONE;
}

const char *stop_reason_text(int code) {
    switch (code) {
        case STOP_NATURAL: return "обе стороны прекратили огонь";
        case STOP_LIMIT:   return "достигнут лимит тактов";
        case STOP_SIGNAL:  return "прерывание оператором сервера";
        case STOP_ERROR:   return "внутренняя ошибка сервера";
        case STOP_ABSENT:  return "участник не вернулся за отведённое время";
        default:           return "неизвестная причина";
    }
}

static const char *const outcome_tokens[] = {
    "miss-accuracy", "miss-destroyed", "miss-empty", "hit", "area-none", "area-hit"
};

const char *outcome_token(int o) {
    if (o < 0 || o > OUT_AREA_HIT) return "?";
    return outcome_tokens[o];
}

int outcome_parse(const char *s) {
    if (!s) return -1;
    for (int i = 0; i <= OUT_AREA_HIT; i++)
        if (strcmp(s, outcome_tokens[i]) == 0) return i;
    return -1;
}

static const char *const reject_tokens[] = {
    "ok", "late", "dup", "ceasefire", "no-ammo", "budget",
    "bad-args", "bad-coord", "bad-state", "not-due"
};

const char *reject_token(int r) {
    if (r < 0 || r > REJ_NOTDUE) return "?";
    return reject_tokens[r];
}

int reject_parse(const char *s) {
    if (!s) return -1;
    for (int i = 0; i <= REJ_NOTDUE; i++)
        if (strcmp(s, reject_tokens[i]) == 0) return i;
    return -1;
}

const char *reject_text(int r) {
    switch (r) {
        case REJ_LATE:      return "опоздание: такт уже завершён";
        case REJ_DUP:       return "второй выстрел за один такт";
        case REJ_CEASEFIRE: return "огонь уже прекращён";
        case REJ_NOAMMO:    return "нет снарядов этого типа";
        case REJ_BUDGET:    return "не хватает бюджета";
        case REJ_BADARGS:   return "неверные аргументы выстрела";
        case REJ_BADCOORD:  return "координаты вне территории противника";
        case REJ_BADSTATE:  return "выстрел вне фазы стрельбы";
        case REJ_NOTDUE:    return "тяжёлый залп раньше срока по расписанию";
        default:            return "неизвестная причина";
    }
}
