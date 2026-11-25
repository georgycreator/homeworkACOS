#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include "signals.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void sb_init(sbuf_t *sb) { sb->d = NULL; sb->len = sb->cap = 0; }

void sb_free(sbuf_t *sb) { free(sb->d); sb_init(sb); }

void sb_clear(sbuf_t *sb) { sb->len = 0; if (sb->d) sb->d[0] = '\0'; }

const char *sb_str(const sbuf_t *sb) { return sb->d ? sb->d : ""; }

int sb_printf(sbuf_t *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return -1; }

    const size_t need = sb->len + (size_t)n + 1;
    if (need > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 256;
        while (ncap < need) ncap *= 2;
        char *p = (char *)realloc(sb->d, ncap);
        if (!p) { va_end(ap2); return -1; }
        sb->d = p;
        sb->cap = ncap;
    }
    vsnprintf(sb->d + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
    return n;
}

long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void sleep_ms(long ms) {
    while (ms > 0) {
        const long part = ms > 10 ? 10 : ms;
        struct timespec req = { part / 1000, (part % 1000) * 1000000L };
        while (nanosleep(&req, &req) < 0 && errno == EINTR) {
            if (signals_stop_requested()) return;
        }
        ms -= part;
        if (signals_stop_requested()) return;
    }
}

int util_parse_long(const char *s, long lo, long hi, long *out) {
    if (!s || !*s) return -1;
    errno = 0;
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return -1;
    if (v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int util_split(char *line, char **argv, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}
