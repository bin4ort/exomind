/* exosched utilities: memory, buffers, escaping, small hashing. */
#include "exosched.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "exosched: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "exosched: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "exosched: out of memory\n");
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t now_epoch(void)
{
    return (int64_t)time(NULL);
}

int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static uint32_t g_rng = 0;

uint32_t rand32(void)
{
    if (!g_rng)
        g_rng = (uint32_t)(now_ms() ^ (uint64_t)getpid() * 2654435761u);
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

int ci_prefix(const char *line, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    return 1;
}

/* escape \n \t \r \\ so a value fits on one line; returns malloc'd */
char *esc_line(const char *s, size_t n)
{
    char *out = xmalloc(n * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
        case '\t': out[j++] = '\\'; out[j++] = 't'; break;
        case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        default:   out[j++] = (char)c;
        }
    }
    out[j] = 0;
    return out;
}

/* inverse of esc_line */
char *unesc_line(const char *s)
{
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char c = s[++i];
            if (c == 'n')      out[j++] = '\n';
            else if (c == 't') out[j++] = '\t';
            else if (c == 'r') out[j++] = '\r';
            else if (c == '\\') out[j++] = '\\';
            else { out[j++] = '\\'; out[j++] = c; }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = 0;
    return out;
}

static int utf8_decode(const unsigned char *s, size_t remain, uint32_t *cp)
{
    if (remain < 1)
        return 0;
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    if (remain >= 2 && (s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (remain >= 3 && (s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
              (s[2] & 0x3F);
        return 3;
    }
    if (remain >= 4 && (s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;
}

/* full JSON string escape: control chars + non-ASCII as \uXXXX */
char *json_escape(const char *s, size_t n)
{
    char *out = xmalloc(n * 6 + 1);
    size_t j = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            out[j++] = '\\'; out[j++] = '"'; i++;
        } else if (c == '\\') {
            out[j++] = '\\'; out[j++] = '\\'; i++;
        } else if (c == '\n') {
            out[j++] = '\\'; out[j++] = 'n'; i++;
        } else if (c == '\t') {
            out[j++] = '\\'; out[j++] = 't'; i++;
        } else if (c == '\r') {
            out[j++] = '\\'; out[j++] = 'r'; i++;
        } else if (c == '\b') {
            out[j++] = '\\'; out[j++] = 'b'; i++;
        } else if (c == '\f') {
            out[j++] = '\\'; out[j++] = 'f'; i++;
        } else if (c < 0x20) {
            j += (size_t)snprintf(out + j, n * 6 + 1 - j, "\\u%04x", c);
            i++;
        } else if (c < 0x80) {
            out[j++] = (char)c;
            i++;
        } else {
            uint32_t cp = 0;
            int m = utf8_decode((const unsigned char *)s + i, n - i, &cp);
            if (m <= 0) {
                out[j++] = '?';
                i++;
                continue;
            }
            i += (size_t)m;
            if (cp <= 0xFFFF) {
                j += (size_t)snprintf(out + j, n * 6 + 1 - j, "\\u%04x",
                                      (unsigned)cp);
            } else {
                uint32_t v = cp - 0x10000;
                j += (size_t)snprintf(out + j, n * 6 + 1 - j, "\\u%04x\\u%04x",
                                      (unsigned)(0xD800 + (v >> 10)),
                                      (unsigned)(0xDC00 + (v & 0x3FF)));
            }
        }
    }
    out[j] = 0;
    return out;
}

void buf_put(buf_t *b, const void *d, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(buf_t *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0)
        return;
    if (b->len + (size_t)need + 1 > b->cap) {
        b->cap = (b->len + (size_t)need + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
}

void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}
