/* exodoc utilities: memory, buffers, escaping, version-token scanning. */
#include "exodoc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "exodoc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "exodoc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "exodoc: out of memory\n");
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

/* full JSON string escape; non-ASCII and control chars as \uXXXX */
char *json_escape(const char *s, size_t n)
{
    char *out = xmalloc(n * 6 + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            out[j++] = '\\'; out[j++] = '"';
        } else if (c == '\\') {
            out[j++] = '\\'; out[j++] = '\\';
        } else if (c == '\n') {
            out[j++] = '\\'; out[j++] = 'n';
        } else if (c == '\t') {
            out[j++] = '\\'; out[j++] = 't';
        } else if (c == '\r') {
            out[j++] = '\\'; out[j++] = 'r';
        } else if (c == '\b') {
            out[j++] = '\\'; out[j++] = 'b';
        } else if (c == '\f') {
            out[j++] = '\\'; out[j++] = 'f';
        } else if (c < 0x20) {
            j += (size_t)snprintf(out + j, n * 6 + 1 - j, "\\u%04x", c);
        } else if (c < 0x80) {
            out[j++] = (char)c;
        } else {
            j += (size_t)snprintf(out + j, n * 6 + 1 - j, "\\u%04x", c);
        }
    }
    out[j] = 0;
    return out;
}

/* case-insensitive prefix compare of a line against a literal prefix */
int ci_prefix(const char *line, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    return 1;
}

void lc(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* is c a word character (alnum)? non-ASCII bytes are not words */
static int word_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* Scan s[0..n) for the first version token v?X.Y.Z. Skips IP-shaped
 * collocations (X.Y.Z.D), dates (YYYY.MM.DD) and tokens glued to
 * alphanumerics. Returns token length (0 if none); stores the token in
 * out (outsz >= 32). */
size_t scan_version(const char *s, size_t n, char *out, size_t outsz)
{
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        if (j < n && (s[j] == 'v' || s[j] == 'V')) {
            if (j > 0 && word_char((unsigned char)s[j - 1]))
                continue;
            j++;
            if (!is_digit((unsigned char)s[j]))
                continue;
        } else if (is_digit((unsigned char)s[j])) {
            if (j > 0 && word_char((unsigned char)s[j - 1]))
                continue;
        } else {
            continue;
        }
        size_t start = i;
        int parts = 0;
        size_t lens[3] = {0, 0, 0};
        while (parts < 3 && j < n) {
            if (!is_digit((unsigned char)s[j]))
                break;
            while (j < n && is_digit((unsigned char)s[j])) {
                lens[parts]++;
                j++;
            }
            if (parts < 2) {
                if (j >= n || s[j] != '.')
                    break;
                j++;
            }
            parts++;
        }
        if (parts != 3)
            continue;
        if (j < n && word_char((unsigned char)s[j]))
            continue;
        if (j < n && s[j] == '.' && j + 1 < n && is_digit((unsigned char)s[j + 1])) {
            i = j; /* IP-shaped X.Y.Z.D — skip the whole run */
            continue;
        }
        if (lens[0] == 4 && lens[1] == 2 && lens[2] == 2) {
            i = j; /* YYYY.MM.DD date shape — skip the whole run */
            continue;
        }
        size_t len = j - start;
        if (len + 1 > outsz)
            len = outsz - 1;
        memcpy(out, s + start, len);
        out[len] = 0;
        return len;
    }
    return 0;
}
