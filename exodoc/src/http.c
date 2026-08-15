/* exodoc HTTP client: minimal zero-dep GET/POST, exosched-proven pattern. */
#include "exodoc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int connect_host(const char *host, int port, char *err, size_t errsz)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errsz, "socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!inet_pton(AF_INET, host, &addr.sin_addr)) {
        snprintf(err, errsz, "bad host %s", host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        snprintf(err, errsz, "connect %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = HTTP_TIMEOUT_S, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static int read_body(int fd, char **body, size_t *blen, int *status,
                     char *err, size_t errsz)
{
    buf_t h = {0};
    char tmp[4096];
    size_t hdr_end = 0;
    while (h.len < 65536) {
        ssize_t got = read(fd, tmp, sizeof tmp);
        if (got <= 0)
            break;
        buf_put(&h, tmp, (size_t)got);
        for (size_t i = 0; i + 4 <= h.len; i++) {
            if (memcmp(h.p + i, "\r\n\r\n", 4) == 0) {
                hdr_end = i + 4;
                break;
            }
        }
        if (hdr_end)
            break;
    }
    if (!hdr_end || h.len < 16) {
        buf_free(&h);
        snprintf(err, errsz, "bad response");
        return -1;
    }
    *status = 500;
    if (h.len > 9 && memcmp(h.p, "HTTP/1.1 ", 9) == 0)
        *status = atoi(h.p + 9);
    long clen = -1;
    char *hl = h.p;
    while (hl < h.p + hdr_end) {
        char *he = memchr(hl, '\n', (size_t)(h.p + hdr_end - hl));
        if (!he)
            break;
        *he = 0;
        if (he > hl && he[-1] == '\r')
            he[-1] = 0;
        if (ci_prefix(hl, "content-length:"))
            clen = strtol(hl + 15, NULL, 10);
        hl = he + 1;
    }
    buf_t b = {0};
    if (h.len > hdr_end)
        buf_put(&b, h.p + hdr_end, h.len - hdr_end);
    if (clen > 0) {
        long need = clen - (long)b.len;
        while (need > 0 && b.len < HTTP_MAX) {
            size_t take = (size_t)need < sizeof tmp ? (size_t)need : sizeof tmp;
            ssize_t got = read(fd, tmp, take);
            if (got <= 0)
                break;
            buf_put(&b, tmp, (size_t)got);
            need -= (long)got;
        }
    } else if (clen < 0 && b.len < HTTP_MAX) {
        while (b.len < HTTP_MAX) {
            ssize_t got = read(fd, tmp, sizeof tmp);
            if (got <= 0)
                break;
            if (b.len + (size_t)got > HTTP_MAX)
                break;
            buf_put(&b, tmp, (size_t)got);
        }
    }
    buf_free(&h);
    if (b.len > HTTP_MAX) {
        buf_free(&b);
        snprintf(err, errsz, "response too large");
        return -1;
    }
    *body = b.p ? b.p : xstrdup("");
    *blen = b.len;
    return 0;
}

static int http_req(const char *host, int port, const char *method,
                    const char *path, const char *body, size_t blen,
                    int json_ct, char **out, size_t *outlen, int *status,
                    char *err, size_t errsz)
{
    int fd = connect_host(host, port, err, errsz);
    if (fd < 0)
        return -1;
    buf_t req = {0};
    buf_printf(&req, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n",
               method, path, host, port);
    if (body && blen > 0) {
        buf_printf(&req, "Content-Length: %zu\r\n", blen);
        if (json_ct)
            buf_puts(&req, "Content-Type: application/json\r\n");
    }
    buf_puts(&req, "\r\n");
    if (body && blen > 0)
        buf_put(&req, body, blen);
    size_t reqlen = req.len;
    ssize_t sent = write(fd, req.p, req.len);
    buf_free(&req);
    if (sent < 0 || (size_t)sent != reqlen) {
        snprintf(err, errsz, "write failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    char *resp = NULL;
    size_t rlen = 0;
    int st = 0;
    if (read_body(fd, &resp, &rlen, &st, err, errsz) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    *out = resp;
    *outlen = rlen;
    *status = st;
    return 0;
}

int http_get(const char *host, int port, const char *path, char **body,
             size_t *blen, int *status, char *err, size_t errsz)
{
    return http_req(host, port, "GET", path, NULL, 0, 0, body, blen,
                    status, err, errsz);
}

int http_post_json(const char *host, int port, const char *path,
                   const char *body, size_t blen, char **resp, size_t *rlen,
                   int *status, char *err, size_t errsz)
{
    return http_req(host, port, "POST", path, body, blen, 1, resp, rlen,
                    status, err, errsz);
}
