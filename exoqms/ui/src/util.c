/* util.c — small helpers: buffers, vectors, strings, UTF-8, file IO. */
#include "exoqms.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdarg.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        abort();
    return p;
}

void *xcalloc(size_t n)
{
    void *p = calloc(n ? n : 1, 1);
    if (!p)
        abort();
    return p;
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

void buf_append(buf_t *b, const void *d, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1)
            nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p)
            abort();
        b->cap = nc;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp)
        buf_append(b, tmp, (size_t)n);
    else {
        size_t need = (size_t)n + 1;
        char *big = xmalloc(need);
        va_start(ap, fmt);
        vsnprintf(big, need, fmt, ap);
        va_end(ap);
        buf_append(b, big, (size_t)n);
        free(big);
    }
}

void buf_free(buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

void vec_push(vec_t *v, void *p)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->it = realloc(v->it, v->cap * sizeof *v->it);
        if (!v->it)
            abort();
    }
    v->it[v->len++] = p;
}

void str_trim(char *s)
{
    char *start = s;
    char *e;
    while (*s && ascii_space((unsigned char)*s))
        s++;
    if (s != start)
        memmove(start, s, strlen(s) + 1);
    s = start;
    e = s + strlen(s);
    while (e > s && ascii_space((unsigned char)e[-1]))
        e--;
    *e = 0;
}

int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ascii_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

int ascii_digit(int c)
{
    return c >= '0' && c <= '9';
}

void lc_ascii(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s += 32;
}

int parse_num(const char *s, double *out)
{
    int neg = 0;
    double v = 0;
    int have = 0;
    if (!s)
        return 0;
    if (*s == '-') { neg = 1; s++; }
    while (ascii_digit((unsigned char)*s)) {
        v = v * 10 + (*s - '0');
        have = 1;
        s++;
    }
    if (*s == '.') {
        double f = 0.1;
        s++;
        while (ascii_digit((unsigned char)*s)) {
            v += (*s - '0') * f;
            f /= 10;
            have = 1;
            s++;
        }
    }
    if (!have)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

char *json_escape(const char *s, size_t n)
{
    buf_t b = {0};
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf_puts(&b, "\\\""); break;
        case '\\': buf_puts(&b, "\\\\"); break;
        case '\n': buf_puts(&b, "\\n"); break;
        case '\t': buf_puts(&b, "\\t"); break;
        case '\r': buf_puts(&b, "\\r"); break;
        default:
            if (c < 0x20) {
                char t[8];
                snprintf(t, sizeof t, "\\u%04x", c);
                buf_puts(&b, t);
            } else {
                buf_append(&b, &s[i], 1);
            }
        }
    }
    return b.p;
}

uint32_t utf8_next(const char *s, size_t *i)
{
    size_t j = *i;
    uint32_t cp;
    if ((unsigned char)s[j] < 0x80) {
        *i = j + 1;
        return (uint32_t)s[j];
    }
    if ((unsigned char)s[j] >= 0xC2 && (unsigned char)s[j] <= 0xDF &&
        ((unsigned char)s[j + 1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[j] & 0x1F) << 6) | (uint32_t)(s[j + 1] & 0x3F);
        *i = j + 2;
        return cp;
    }
    if ((unsigned char)s[j] >= 0xE0 && (unsigned char)s[j] <= 0xEF &&
        ((unsigned char)s[j + 1] & 0xC0) == 0x80 &&
        ((unsigned char)s[j + 2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[j] & 0x0F) << 12) |
             ((uint32_t)(s[j + 1] & 0x3F) << 6) |
             (uint32_t)(s[j + 2] & 0x3F);
        *i = j + 3;
        return cp;
    }
    if ((unsigned char)s[j] >= 0xF0 && (unsigned char)s[j] <= 0xF4 &&
        ((unsigned char)s[j + 1] & 0xC0) == 0x80 &&
        ((unsigned char)s[j + 2] & 0xC0) == 0x80 &&
        ((unsigned char)s[j + 3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[j] & 0x07) << 18) |
             ((uint32_t)(s[j + 1] & 0x3F) << 12) |
             ((uint32_t)(s[j + 2] & 0x3F) << 6) |
             (uint32_t)(s[j + 3] & 0x3F);
        *i = j + 4;
        return cp;
    }
    *i = j + 1;
    return 0xFFFDu;
}

int utf8_is_emoji(uint32_t cp)
{
    if (cp >= 0x1F000 && cp <= 0x1FAFF)
        return 1;                     /* symbols, pictographs, regional flags */
    if (cp >= 0x2600 && cp <= 0x27BF)
        return 1;                     /* misc symbols + dingbats */
    if (cp >= 0x2B00 && cp <= 0x2BFF)
        return 1;                     /* arrows, misc symbols */
    if (cp == 0xFE0F || cp == 0x20E3)
        return 1;                     /* variation selector-16, keycap */
    return 0;
}

size_t utf8_write(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *file_read(const char *path, size_t *len, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *p;
    size_t n;
    if (!f) {
        snprintf(err, errsz, "cannot open %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(err, errsz, "cannot seek %s", path);
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > 256u * 1024u * 1024u) {
        snprintf(err, errsz, "%s: file too large or unreadable", path);
        fclose(f);
        return NULL;
    }
    rewind(f);
    p = xmalloc((size_t)sz + 1);
    n = fread(p, 1, (size_t)sz, f);
    fclose(f);
    p[n] = 0;
    *len = n;
    return p;
}

static int walk_one(const char *dir, vec_t *out, int depth)
{
    DIR *d;
    struct dirent *e;
    char path[4096];
    if (depth > 12 || out->len > 512)
        return 0;
    d = opendir(dir);
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        size_t dl;
        if (e->d_name[0] == '.')
            continue;
        dl = strlen(e->d_name);
        if (dl + strlen(dir) + 2 > sizeof path)
            continue;
        snprintf(path, sizeof path, "%.*s/%.*s", (int)strlen(dir), dir,
                 (int)dl, e->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk_one(path, out, depth + 1);
        else if (S_ISREG(st.st_mode) && dl > 5 &&
                 ci_eq(e->d_name + dl - 5, ".html"))
            vec_push(out, xstrdup(path));
    }
    closedir(d);
    return 0;
}

int dir_walk_html(const char *dir, vec_t *out)
{
    return walk_one(dir, out, 0);
}
