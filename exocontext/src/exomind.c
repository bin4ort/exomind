/* exosched -> exomind HTTP client: the durable storage backend. */
#include "exocontext.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define EXO_MAX_BODY (8u * 1024u * 1024u)

int exo_init(exo_t *e, const char *url, char *err, size_t errsz)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) {
        snprintf(err, errsz, "bad exomind url (only http://host[:port])");
        return -1;
    }
    p += 7;
    size_t hostlen = 0;
    while (p[hostlen] && p[hostlen] != ':' && p[hostlen] != '/')
        hostlen++;
    if (hostlen == 0 || hostlen >= sizeof e->host) {
        snprintf(err, errsz, "bad exomind url host");
        return -1;
    }
    memcpy(e->host, p, hostlen);
    e->host[hostlen] = 0;
    p += hostlen;
    e->port = 7654;
    if (*p == ':') {
        char tmp[16];
        p++;
        size_t i = 0;
        while (p[i] && p[i] != '/' && i < sizeof tmp - 1) {
            tmp[i] = p[i];
            i++;
        }
        tmp[i] = 0;
        e->port = atoi(tmp);
        if (e->port <= 0 || e->port > 65535) {
            snprintf(err, errsz, "bad exomind url port");
            return -1;
        }
    }
    return 0;
}

static int connect_exo(const exo_t *e, char *err, size_t errsz)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errsz, "socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)e->port);
    if (!inet_pton(AF_INET, e->host, &addr.sin_addr)) {
        snprintf(err, errsz, "bad exomind host %s", e->host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        snprintf(err, errsz, "connect %s:%d: %s", e->host, e->port,
                 strerror(errno));
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static int read_response(int fd, char **body, size_t *blen, int *status,
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
        size_t from = h.len > (size_t)got + 4 ? h.len - (size_t)got - 4 : 0;
        for (size_t i = from; i + 4 <= h.len; i++) {
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
        snprintf(err, errsz, "exomind: bad response");
        return -1;
    }
    char *sp = h.p;
    char *eol = memchr(sp, '\n', hdr_end);
    if (!eol) {
        buf_free(&h);
        snprintf(err, errsz, "exomind: bad status line");
        return -1;
    }
    *status = 500;
    if ((size_t)(eol - sp) > 9 && memcmp(sp, "HTTP/1.1 ", 9) == 0)
        *status = atoi(sp + 9);
    long clen = -1;
    char *hl = sp;
    while (hl < h.p + hdr_end) {
        char *he = memchr(hl, '\n', (size_t)(h.p + hdr_end - hl));
        if (!he)
            break;
        *he = 0;
        if (he > hl && he[-1] == '\r')
            he[-1] = 0;
        if (ci_prefix(hl, "content-length:")) {
            clen = strtol(hl + 15, NULL, 10);
        }
        hl = he + 1;
    }
    buf_t b = {0};
    if (h.len > hdr_end)
        buf_put(&b, h.p + hdr_end, h.len - hdr_end);
    if (clen > 0) {
        long need = clen - (long)(b.len);
        while (need > 0) {
            size_t take = (size_t)need < sizeof tmp ? (size_t)need
                                                    : sizeof tmp;
            ssize_t got = read(fd, tmp, take);
            if (got <= 0)
                break;
            buf_put(&b, tmp, (size_t)got);
            need -= (long)got;
        }
    } else if (clen < 0 && b.len < EXO_MAX_BODY) {
        for (;;) {
            ssize_t got = read(fd, tmp, sizeof tmp);
            if (got <= 0)
                break;
            if (b.len + (size_t)got > EXO_MAX_BODY)
                break;
            buf_put(&b, tmp, (size_t)got);
        }
    }
    buf_free(&h);
    if (b.len > EXO_MAX_BODY) {
        buf_free(&b);
        snprintf(err, errsz, "exomind: response too large");
        return -1;
    }
    *body = b.p ? b.p : xstrdup("");
    *blen = b.len;
    return 0;
}

int exo_request(exo_t *e, const char *method, const char *target,
                const char *body, size_t blen, int json_ct,
                char **out, size_t *outlen, int *status,
                char *err, size_t errsz)
{
    int fd = connect_exo(e, err, errsz);
    if (fd < 0)
        return -1;
    buf_t req = {0};
    buf_printf(&req, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n",
               method, target, e->host, e->port);
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
        snprintf(err, errsz, "exomind: write failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    char *resp = NULL;
    size_t rlen = 0;
    int st = 0;
    if (read_response(fd, &resp, &rlen, &st, err, errsz) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    *out = resp;
    *outlen = rlen;
    *status = st;
    return 0;
}

/* stores key with a TTL and a JSON body (avoids exomind's form-parsing
 * when the value contains '=') */
int exo_persist(exo_t *e, const char *key, const char *value, long ttl,
                char *err, size_t errsz)
{
    char *je = json_escape(value, strlen(value));
    char target[512];
    snprintf(target, sizeof target, "/set?key=%s", key);
    buf_t body = {0};
    buf_printf(&body, "{\"key\":\"%s\",\"value\":\"%s\",\"ttl\":%ld}", key,
               je, ttl);
    free(je);
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    int rc = exo_request(e, "POST", target, body.p, body.len, 1, &resp, &rlen,
                         &status, err, errsz);
    buf_free(&body);
    if (rc != 0)
        return -1;
    if (status != 200) {
        snprintf(err, errsz, "exomind set %s failed (status %d: %s)", key,
                 status, resp);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

int exo_del(exo_t *e, const char *key, int *existed, char *err, size_t errsz)
{
    char target[512];
    snprintf(target, sizeof target, "/del?key=%s", key);
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    int rc = exo_request(e, "DELETE", target, NULL, 0, 0, &resp, &rlen,
                         &status, err, errsz);
    if (rc != 0)
        return -1;
    if (existed)
        *existed = status == 200;
    if (status != 200 && status != 404) {
        snprintf(err, errsz, "exomind del %s failed (status %d)", key, status);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

int exo_note(exo_t *e, const char *text, char *err, size_t errsz)
{
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    int rc = exo_request(e, "POST", "/note", text, strlen(text), 0, &resp,
                         &rlen, &status, err, errsz);
    if (rc != 0)
        return -1;
    if (status != 200) {
        snprintf(err, errsz, "exomind note failed (status %d: %s)", status,
                 resp);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

int exo_list(exo_t *e, const char *prefix, char ***keys, size_t *n,
             char *err, size_t errsz)
{
    char target[1024];
    snprintf(target, sizeof target, "/list?prefix=%s&limit=10000", prefix);
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    if (exo_request(e, "GET", target, NULL, 0, 0, &resp, &rlen, &status, err,
                    errsz) != 0)
        return -1;
    if (status != 200) {
        snprintf(err, errsz, "exomind list failed (status %d)", status);
        free(resp);
        return -1;
    }
    buf_t keysb = {0};
    size_t cnt = 0;
    char *save = NULL;
    for (char *line = strtok_r(resp, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n'))
            line[--l] = 0;
        if (l > 0) {
            keysb.p = xrealloc(keysb.p, (cnt + 1) * sizeof(char *));
            ((char **)keysb.p)[cnt++] = xstrdup(line);
        }
    }
    free(resp);
    *keys = (char **)keysb.p;
    *n = cnt;
    return 0;
}

/* fetches values for keys in one /batch round trip; returned strings are
 * unescaped back to raw values */
int exo_batch_get(exo_t *e, char **keys, size_t n, char ***vals,
                  char *err, size_t errsz)
{
    buf_t body = {0};
    buf_puts(&body, "[");
    for (size_t i = 0; i < n; i++) {
        if (i)
            buf_puts(&body, ",");
        buf_printf(&body, "{\"get\":\"%s\"}", keys[i]);
    }
    buf_puts(&body, "]");
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    int rc = exo_request(e, "POST", "/batch", body.p, body.len, 1, &resp,
                         &rlen, &status, err, errsz);
    buf_free(&body);
    if (rc != 0)
        return -1;
    if (status != 200) {
        snprintf(err, errsz, "exomind batch failed (status %d)", status);
        free(resp);
        return -1;
    }
    char **out = xcalloc(n ? n : 1, sizeof(char *));
    size_t got = 0;
    char *save = NULL;
    for (char *line = strtok_r(resp, "\n", &save); line && got < n;
         line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n'))
            line[--l] = 0;
        if (strncmp(line, "get ", 4) != 0)
            continue;
        char *v = line + 4;
        char *sp = strchr(v, ' ');
        if (!sp)
            continue;
        *sp = 0;
        if (strcmp(v, keys[got]) != 0)
            continue;
        out[got] = unesc_line(sp + 1);
        got++;
    }
    free(resp);
    if (got != n) {
        snprintf(err, errsz, "exomind batch get: got %zu of %zu", got, n);
        for (size_t i = 0; i < got; i++)
            free(out[i]);
        free(out);
        return -1;
    }
    *vals = out;
    return 0;
}
