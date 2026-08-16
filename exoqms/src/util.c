/* exoqms utilities: memory, buffers, escaping (pattern: exosched util.c). */
#include "exoqms.h"

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
        fprintf(stderr, "exoqms: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "exoqms: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "exoqms: out of memory\n");
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

int64_t now_epoch(void)
{
    return (int64_t)time(NULL);
}

static uint32_t g_rng = 0;

uint32_t rand32(void)
{
    if (!g_rng)
        g_rng = (uint32_t)((uint64_t)now_epoch() ^
                           (uint64_t)getpid() * 2654435761u);
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

int ci_prefix(const char *line, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)line[i]) !=
            tolower((unsigned char)prefix[i]))
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
    if (remain >= 2 && (s[0] & 0xE0) == 0xC0 && (s[1] & 0x80) == 0x80) {
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
              ((uint32_t)(s[2] & 0x3F) << 6) | ((uint32_t)(s[3] & 0x3F));
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

/* strip trailing \r and \n in place */
void trim_crlf(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\r' || s[l - 1] == '\n'))
        s[--l] = 0;
}

/* split s on tabs in place; returns field count (<= maxf, excess ignored) */
int tab_split(char *s, char **f, int maxf)
{
    int n = 0;
    char *p = s;
    for (;;) {
        if (n < maxf)
            f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t)
            break;
        *t = 0;
        p = t + 1;
    }
    return n;
}

/* ---- minimal JSON (pattern: exomind src/util.c) ---- */

static int json_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int json_utf8_encode(uint32_t cp, char out[4])
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

/* p points at the opening quote; returns pointer past closing quote */
static const char *json_parse_string(const char *p, const char *end, char **out)
{
    size_t cap = 32, len = 0;
    char *buf = xmalloc(cap);
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            p++;
            break;
        }
        int esc = 0;
        if (c == '\\') {
            esc = 1;
            p++;
            if (p >= end)
                goto fail;
            char e = *p++;
            switch (e) {
            case '"': case '\\': case '/':
                c = (unsigned char)e;
                break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                if (p + 4 > end)
                    goto fail;
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = json_hexval((unsigned char)p[i]);
                    if (h < 0)
                        goto fail;
                    cp = cp * 16 + (uint32_t)h;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end &&
                    p[0] == '\\' && p[1] == 'u') {
                    uint32_t lo = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = json_hexval((unsigned char)p[2 + i]);
                        if (h < 0) {
                            lo = 0;
                            break;
                        }
                        lo = lo * 16 + (uint32_t)h;
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                char tmp[4];
                int m = json_utf8_encode(cp, tmp);
                for (int i = 0; i < m; i++) {
                    if (len + 1 >= cap) {
                        cap *= 2;
                        buf = xrealloc(buf, cap);
                    }
                    buf[len++] = tmp[i];
                }
                continue;
            }
            default:
                goto fail;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        buf[len++] = (char)c;
        if (!esc)
            p++;
    }
    if (len + 1 >= cap) {
        cap *= 2;
        buf = xrealloc(buf, cap);
    }
    buf[len] = 0;
    *out = buf;
    return p;
fail:
    free(buf);
    return NULL;
}

/* skip a complete JSON value starting at p; NULL on malformed input */
static const char *json_skip_value(const char *p, const char *end)
{
    p = json_skip_ws(p, end);
    if (p >= end)
        return NULL;
    char c = *p;
    if (c == '"') {
        char *tmp = NULL;
        p = json_parse_string(p, end, &tmp);
        free(tmp);
        return p;
    }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (p < end) {
            c = *p;
            if (c == '"') {
                char *tmp = NULL;
                p = json_parse_string(p, end, &tmp);
                free(tmp);
                if (!p)
                    return NULL;
                continue;
            }
            if (c == open)
                depth++;
            else if (c == close) {
                depth--;
                p++;
                if (depth == 0)
                    return p;
                continue;
            }
            p++;
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

char *json_field(const char *json, size_t len, const char *field)
{
    const char *end = json + len;
    const char *p = json_skip_ws(json, end);
    if (p >= end || *p != '{')
        return NULL;
    p++;
    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end || *p == '}')
            break;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"')
            return NULL;
        char *k = NULL;
        p = json_parse_string(p, end, &k);
        if (!p)
            return NULL;
        int match = (k && strcmp(k, field) == 0);
        p = json_skip_ws(p, end);
        if (p >= end || *p != ':') {
            free(k);
            return NULL;
        }
        p = json_skip_ws(p + 1, end);
        if (p >= end) {
            free(k);
            return NULL;
        }
        if (match) {
            free(k);
            if (*p == '"') {
                char *v = NULL;
                p = json_parse_string(p, end, &v);
                return v; /* NULL on malformed */
            }
            /* raw scalar or array/object: copy the token */
            const char *s = p;
            if (*p == '[' || *p == '{') {
                int depth = 0;
                while (p < end) {
                    if (*p == '[' || *p == '{')
                        depth++;
                    else if (*p == ']' || *p == '}') {
                        depth--;
                        if (depth == 0) {
                            p++;
                            break;
                        }
                    }
                    p++;
                }
            } else {
                while (p < end && *p != ',' && *p != '}' && *p != ']')
                    p++;
            }
            return xstrndup(s, (size_t)(p - s));
        }
        p = json_skip_value(p, end);
        free(k);
        if (!p)
            return NULL;
    }
    return NULL;
}

int json_array_each(const char *json, size_t len, size_t *pos,
                    const char **elem, size_t *elen)
{
    const char *end = json + len;
    const char *p;
    if (*pos == 0) {
        p = json_skip_ws(json, end);
        if (p >= end || *p != '[')
            return 0;
        p++;
    } else {
        p = json + *pos;
    }
    for (;;) {
        p = json_skip_ws(p, end);
        if (p >= end)
            return 0;
        if (*p == ']') {
            *pos = (size_t)(p - json);
            return 0;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }
    const char *s = p;
    p = json_skip_value(p, end);
    if (!p)
        return 0;
    *elem = s;
    *elen = (size_t)(p - s);
    *pos = (size_t)(p - json);
    return 1;
}

char **json_arr_strings(const char *elem, size_t elen, size_t *n)
{
    size_t pos = 0;
    char **arr = NULL;
    size_t cap = 0, cnt = 0;
    const char *e;
    size_t l;
    while (json_array_each(elem, elen, &pos, &e, &l)) {
        const char *p = json_skip_ws(e, e + l);
        if (!p || *p != '"')
            continue;
        char *s = NULL;
        p = json_parse_string(p, e + l, &s);
        if (p) {
            if (cnt == cap) {
                cap = cap ? cap * 2 : 8;
                arr = xrealloc(arr, cap * sizeof(char *));
            }
            arr[cnt++] = s;
        }
    }
    *n = cnt;
    return arr;
}
