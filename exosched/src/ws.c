/* exosched WebSocket layer (RFC 6455), implemented by hand: SHA-1 +
 * base64 accept key, text frames on fire, one thread per client, close
 * handling and dead-client purge. */
#include "exosched.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WS_MAX_FRAME (64u * 1024u * 1024u)

typedef struct wsclient {
    int fd;
    struct wsclient *next;
} wsclient_t;

static pthread_mutex_t ws_mu = PTHREAD_MUTEX_INITIALIZER;
static wsclient_t *g_clients = NULL;

/* ---- SHA-1 (RFC 3174) ---- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    size_t buflen;
} sha1_ctx;

static uint32_t sha1_rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3],
             e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = sha1_rol(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = sha1_rol(b, 30);
        b = a;
        a = tmp;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->buflen = 0;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n)
            take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, unsigned char out[20])
{
    uint64_t bits = c->len * 8;
    unsigned char one = 0x80, zero = 0;
    sha1_update(c, &one, 1);
    while (c->buflen != 56)
        sha1_update(c, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

/* ---- base64 (RFC 4648) ---- */

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t i = 0, o = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     in[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if (i + 1 == n) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (i + 2 == n) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
}

/* computes Sec-WebSocket-Accept from the client's Sec-WebSocket-Key */
int ws_make_accept(const char *key, char *accept, size_t cap)
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, key, strlen(key));
    sha1_update(&c, WS_GUID, strlen(WS_GUID));
    unsigned char digest[20];
    sha1_final(&c, digest);
    char b64[64];
    b64_encode(digest, 20, b64);
    if (strlen(b64) >= cap)
        return -1;
    snprintf(accept, cap, "%s", b64);
    return 0;
}

/* ---- frames ---- */

static int send_frame(int fd, int opcode, const void *payload, size_t n)
{
    unsigned char hdr[14];
    size_t hlen = 0;
    hdr[hlen++] = (unsigned char)(0x80 | opcode);
    if (n < 126) {
        hdr[hlen++] = (unsigned char)n;
    } else if (n <= 0xFFFF) {
        hdr[hlen++] = 126;
        hdr[hlen++] = (unsigned char)(n >> 8);
        hdr[hlen++] = (unsigned char)n;
    } else {
        hdr[hlen++] = 127;
        uint64_t v = n;
        for (int i = 7; i >= 0; i--)
            hdr[hlen++] = (unsigned char)(v >> (8 * i));
    }
    size_t off = 0;
    while (off < hlen) {
        ssize_t w = send(fd, hdr + off, hlen - off, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        off += (size_t)w;
    }
    off = 0;
    while (off < n) {
        ssize_t w = send(fd, (const char *)payload + off, n - off, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

/* reads exactly n bytes; returns 0 ok, -1 eof/error */
static int recv_full(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, (char *)buf + off, n - off, 0);
        if (r <= 0)
            return -1;
        off += (size_t)r;
    }
    return 0;
}

/* per-client read loop: handles close/ping frames, self-removes on exit.
 * returns 1 if it removed itself from the list, 0 if the broadcaster
 * already purged it (in which case the fd is closed and must not be
 * closed again by us). */
static int ws_client_loop(int fd)
{
    for (;;) {
        unsigned char hdr[2];
        if (recv_full(fd, hdr, 2) != 0)
            break;
        int opcode = hdr[0] & 0x0F;
        int masked = (hdr[1] & 0x80) != 0;
        uint64_t n = hdr[1] & 0x7F;
        if (n == 126) {
            unsigned char ext[2];
            if (recv_full(fd, ext, 2) != 0)
                break;
            n = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (n == 127) {
            unsigned char ext[8];
            if (recv_full(fd, ext, 8) != 0)
                break;
            n = 0;
            for (int i = 0; i < 8; i++)
                n = (n << 8) | ext[i];
        }
        if (n > WS_MAX_FRAME)
            break;
        unsigned char mask[4];
        if (masked && recv_full(fd, mask, 4) != 0)
            break;
        unsigned char *payload = n ? xmalloc((size_t)n) : NULL;
        if (n && recv_full(fd, payload, (size_t)n) != 0) {
            free(payload);
            break;
        }
        if (masked) {
            for (uint64_t i = 0; i < n; i++)
                payload[i] ^= mask[i % 4];
        }
        switch (opcode) {
        case 0x8: /* close: echo close and drop */
            send_frame(fd, 0x8, payload, (size_t)n);
            free(payload);
            goto out;
        case 0x9: /* ping -> pong */
            send_frame(fd, 0xA, payload, (size_t)n);
            break;
        case 0xA: /* pong */
        case 0x1: /* text from client: ignored, we only push */
        case 0x0: /* continuation of an ignored stream */
            break;
        default: /* reserved opcodes are a protocol error */
            free(payload);
            goto out;
        }
        free(payload);
    }
out:
    pthread_mutex_lock(&ws_mu);
    wsclient_t **pp = &g_clients;
    while (*pp) {
        if ((*pp)->fd == fd) {
            wsclient_t *c = *pp;
            *pp = c->next;
            free(c);
            pthread_mutex_unlock(&ws_mu);
            return 1;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&ws_mu);
    return 0;
}

void ws_handle_conn(int fd)
{
    wsclient_t *c = xmalloc(sizeof *c);
    c->fd = fd;
    c->next = NULL;
    pthread_mutex_lock(&ws_mu);
    c->next = g_clients;
    g_clients = c;
    pthread_mutex_unlock(&ws_mu);
    if (ws_client_loop(fd))
        close(fd);
}

/* pushes one text frame to every live client; purges dead ones */
void ws_broadcast(const char *id, int64_t epoch, const char *msg)
{
    buf_t line = {0};
    buf_printf(&line, "timer %s %lld %s", id, (long long)epoch, msg);
    pthread_mutex_lock(&ws_mu);
    wsclient_t **pp = &g_clients;
    while (*pp) {
        wsclient_t *c = *pp;
        if (send_frame(c->fd, 0x1, line.p, line.len) != 0) {
            *pp = c->next;
            close(c->fd);
            free(c);
            continue;
        }
        pp = &c->next;
    }
    pthread_mutex_unlock(&ws_mu);
    buf_free(&line);
}

void ws_init(void)
{
}
