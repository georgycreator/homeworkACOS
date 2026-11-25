#define _POSIX_C_SOURCE 200809L
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int net_set_nonblock(int fd) {
    const int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Параметры сокета, общие для обеих сторон: без задержки Нейгла (сообщения
// короткие, важна отзывчивость) и с проверкой «живости» соединения.
static void tune_socket(int fd) {
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

void conn_init(conn_t *c, int fd, size_t wmax) {
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->wmax = wmax;
}

void conn_close(conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    free(c->wbuf);
    c->wbuf = NULL;
    c->woff = c->wlen = c->wcap = 0;
    c->rlen = 0;
}

int conn_fill(conn_t *c) {
    if (c->fd < 0 || c->dead) return -1;
    for (;;) {
        if (c->rlen >= sizeof(c->rbuf)) return 0;        // буфер полон: сначала разберите строки
        const ssize_t n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen, 0);
        if (n > 0) { c->rlen += (size_t)n; continue; }
        if (n == 0) { c->eof = 1; return 0; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        c->dead = 1;
        return -1;
    }
}

int conn_getline(conn_t *c, char *out, size_t cap) {
    char *nl = memchr(c->rbuf, '\n', c->rlen);
    if (!nl) {
        // Нет перевода строки, а буфер уже больше допустимой строки — протокол нарушен.
        if (c->rlen >= PROTO_MAX_LINE) return -1;
        return 0;
    }
    size_t len = (size_t)(nl - c->rbuf);
    if (len >= PROTO_MAX_LINE) return -1;
    const size_t consumed = len + 1;
    if (len > 0 && c->rbuf[len - 1] == '\r') len--;
    if (len >= cap) len = cap - 1;
    memcpy(out, c->rbuf, len);
    out[len] = '\0';
    memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
    c->rlen -= consumed;
    return 1;
}

int conn_vsend(conn_t *c, const char *fmt, va_list ap) {
    if (c->fd < 0 || c->dead) return -1;
    va_list ap2;
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0, fmt, ap);
    if (n < 0) { va_end(ap2); return -1; }

    const size_t need = (size_t)n + 1;                    // текст + '\n'
    if (c->woff == c->wlen) c->woff = c->wlen = 0;        // очередь пуста — начинаем с начала
    if ((c->wlen - c->woff) + need > c->wmax) {           // слишком медленный получатель
        c->dead = 1;
        va_end(ap2);
        return -1;
    }
    if (c->wlen + need > c->wcap) {
        if (c->woff > 0) {                                // сначала уплотняем очередь
            memmove(c->wbuf, c->wbuf + c->woff, c->wlen - c->woff);
            c->wlen -= c->woff;
            c->woff = 0;
        }
        if (c->wlen + need > c->wcap) {
            size_t ncap = c->wcap ? c->wcap : 4096;
            while (ncap < c->wlen + need) ncap *= 2;
            char *p = (char *)realloc(c->wbuf, ncap);
            if (!p) { c->dead = 1; va_end(ap2); return -1; }
            c->wbuf = p;
            c->wcap = ncap;
        }
    }
    vsnprintf(c->wbuf + c->wlen, need + 1, fmt, ap2);
    va_end(ap2);
    c->wlen += (size_t)n;
    c->wbuf[c->wlen++] = '\n';
    return 0;
}

int conn_send(conn_t *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = conn_vsend(c, fmt, ap);
    va_end(ap);
    return r;
}

int conn_flush(conn_t *c) {
    if (c->fd < 0) return -1;
    while (c->woff < c->wlen) {
        // MSG_NOSIGNAL: запись в закрытый сокет вернёт EPIPE, а не убьёт процесс по SIGPIPE.
        const ssize_t n = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff, MSG_NOSIGNAL);
        if (n > 0) { c->woff += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        c->dead = 1;
        return -1;
    }
    c->woff = c->wlen = 0;
    return 0;
}

int conn_has_output(const conn_t *c) { return c->woff < c->wlen; }

int net_listen(const char *host, int port, int *bound_port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo((host && *host) ? host : NULL, ps, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 64) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    if (net_set_nonblock(fd) < 0) { close(fd); return -1; }

    if (bound_port) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        *bound_port = port;
        if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0) *bound_port = ntohs(sa.sin_port);
    }
    return fd;
}

int net_connect(const char *host, int port, int timeout_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (net_set_nonblock(fd) < 0) { close(fd); fd = -1; continue; }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = { fd, POLLOUT, 0 };
            int pr;
            do { pr = poll(&p, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
            if (pr > 0) {
                int err = 0;
                socklen_t el = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) rc = 0;
            }
        }
        if (rc == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) tune_socket(fd);
    return fd;
}

int net_accept(int listen_fd, char *peer, size_t peer_cap) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    const int fd = accept(listen_fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) return -1;
    if (net_set_nonblock(fd) < 0) { close(fd); return -1; }
    tune_socket(fd);
    if (peer && peer_cap) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
        snprintf(peer, peer_cap, "%s:%d", ip, ntohs(sa.sin_port));
    }
    return fd;
}
