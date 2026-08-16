/* exocrawl: string buffers, URL helpers, entity decoding. */
#include "exocrawl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(buf_t *b, size_t cap)
{
    b->cap = cap ? cap : 256;
    b->p = malloc(b->cap);
    b->len = 0;
    if (b->p)
        b->p[0] = 0;
}

void buf_free(buf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void buf_grow(buf_t *b, size_t need)
{
    if (b->len + need + 1 <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + need + 1)
        nc *= 2;
    char *nw = realloc(b->p, nc);
    if (!nw)
        return;
    b->p = nw;
    b->cap = nc;
}

void buf_puts(buf_t *b, const char *s)
{
    size_t n = strlen(s);
    buf_grow(b, n);
    if (!b->p)
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_putc(buf_t *b, char c)
{
    buf_grow(b, 1);
    if (!b->p)
        return;
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp) {
        buf_puts(b, tmp);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_puts(b, big);
    free(big);
}

char *url_encode(const char *s)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[w++] = (char)c;
        } else {
            out[w++] = '%';
            out[w++] = HEX[c >> 4];
            out[w++] = HEX[c & 15];
        }
    }
    out[w] = 0;
    return out;
}

char *url_host(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    if (!p)
        return NULL;
    p += 3;
    const char *e = p;
    while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#')
        e++;
    size_t n = (size_t)(e - p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

char *url_path(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    if (!p)
        return NULL;
    p += 3;
    const char *e = strchr(p, '/');
    const char *q = strchr(p, '?');
    if (!e)
        e = q ? q : p + strlen(p);
    if (e == q)
        e = p;
    if (!*e) {
        snprintf(out, cap, "/");
        return out;
    }
    snprintf(out, cap, "%.*s", (int)(strlen(e) < cap ? strlen(e) : cap - 1), e);
    return out;
}

int url_is_http(const char *url)
{
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

/* decode a single entity at src (length n): returns chars consumed or 0 */
static size_t entity_one(const char *src, size_t n, char *out)
{
    if (n < 3 || src[0] != '&')
        return 0;
    if (src[1] == '#') {
        /* numeric: &#123; or &#x1F; */
        int base = 10;
        size_t i = 2;
        if (i < n && (src[i] == 'x' || src[i] == 'X')) {
            base = 16;
            i++;
        }
        long v = 0;
        size_t start = i;
        while (i < n && src[i] != ';') {
            int d = base == 16 ? (isdigit((unsigned char)src[i])
                                     ? src[i] - '0'
                                     : tolower((unsigned char)src[i]) - 'a' + 10)
                               : src[i] - '0';
            if (base == 10 && !isdigit((unsigned char)src[i]))
                return 0;
            if (d < 0 || d >= base)
                return 0;
            v = v * base + d;
            if (v > 0x10FFFF)
                return 0;
            i++;
        }
        if (i >= n || src[i] != ';' || i == start)
            return 0;
        if (v < 0x80) {
            out[0] = (char)v;
            out[1] = 0;
            return i + 1;
        }
        /* encode as UTF-8 */
        if (v < 0x800) {
            out[0] = (char)(0xC0 | (v >> 6));
            out[1] = (char)(0x80 | (v & 0x3F));
            out[2] = 0;
            return i + 1;
        }
        if (v < 0x10000) {
            out[0] = (char)(0xE0 | (v >> 12));
            out[1] = (char)(0x80 | ((v >> 6) & 0x3F));
            out[2] = (char)(0x80 | (v & 0x3F));
            out[3] = 0;
            return i + 1;
        }
        out[0] = (char)(0xF0 | (v >> 18));
        out[1] = (char)(0x80 | ((v >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((v >> 6) & 0x3F));
        out[3] = (char)(0x80 | (v & 0x3F));
        out[4] = 0;
        return i + 1;
    }
    static const struct {
        const char *name;
        unsigned char ch;
    } MAP[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", '\''}, {"nbsp;", ' '}, {"copy;", 0xC2}, {"reg;", 0xC2},
        {"hellip;", 0xE2}, {"mdash;", 0xE2}, {"ndash;", 0xE2},
        {"lsquo;", 0xE2}, {"rsquo;", 0xE2}, {"ldquo;", 0xE2},
        {"rdquo;", 0xE2}, {"bull;", 0xE2}, {"middot;", 0xC2},
        {"sect;", 0xC2}, {"para;", 0xC2}, {"laquo;", 0xC2},
        {"raquo;", 0xC2}, {"deg;", 0xC2}, {"plusmn;", 0xC2},
        {"frac12;", 0xC2}, {"frac14;", 0xC2}, {"frac34;", 0xC2},
        {"times;", 0xC2}, {"divide;", 0xC2}, {"eacute;", 0xC3},
        {"egrave;", 0xC3}, {"ecirc;", 0xC3}, {"euml;", 0xC3},
        {"agrave;", 0xC3}, {"aacute;", 0xC3}, {"acirc;", 0xC3},
        {"ccedil;", 0xC3}, {"igrave;", 0xC3}, {"iacute;", 0xC3},
        {"ocirc;", 0xC3}, {"ouml;", 0xC3}, {"ugrave;", 0xC3},
        {"uacute;", 0xC3}, {"ntilde;", 0xC3}, {"szlig;", 0xC3},
        {NULL, 0}};
    for (int i = 0; MAP[i].name; i++) {
        size_t ln = strlen(MAP[i].name);
        if (n >= ln + 1 && src[1] == MAP[i].name[0] &&
            memcmp(src + 1, MAP[i].name, ln) == 0) {
            unsigned char ch = MAP[i].ch;
            if (ch == 0xC2) {
                out[0] = 0xC2;
                out[1] = (char)(0x80 | ((MAP[i].name[0] == 'c' ? 0x29
                                  : MAP[i].name[0] == 'n' ? 0x20
                                  : MAP[i].name[0] == 'm' ? 0x37
                                  : MAP[i].name[0] == 's' ? 0x27
                                  : MAP[i].name[0] == 'p' ? 0x31
                                  : MAP[i].name[0] == 'd' ? 0x30
                                  : 0x26) & 0x3F));
                out[2] = 0;
                return ln + 1;
            }
            if (ch == 0xC3) {
                out[0] = 0xC3;
                out[1] = (char)(0x80 | ((MAP[i].name[1] == 'a' ? 0x20
                                  : MAP[i].name[1] == 'e' ? 0x29
                                  : MAP[i].name[1] == 'i' ? 0x29
                                  : MAP[i].name[1] == 'o' ? 0x34
                                  : MAP[i].name[1] == 'u' ? 0x39
                                  : MAP[i].name[1] == 'c' ? 0x27
                                  : 0x1F) & 0x3F));
                out[2] = 0;
                return ln + 1;
            }
            if (ch == 0xE2) {
                out[0] = 0xE2;
                out[1] = 0x80;
                out[2] = (char)(MAP[i].name[0] == 'm' ? 0x94
                                : MAP[i].name[0] == 'n' ? 0x93
                                : MAP[i].name[0] == 'l' ? 0x98
                                : MAP[i].name[0] == 'r' ? 0x99
                                : MAP[i].name[0] == 'b' ? 0xA2
                                : MAP[i].name[0] == 'h' ? 0xA6
                                : 0x9C);
                out[3] = 0;
                return ln + 1;
            }
            out[0] = MAP[i].name[0] == 'a' ? '&' : MAP[i].name[0] == 'l'  ? '<'
                     : MAP[i].name[0] == 'g' ? '>' : MAP[i].name[0] == 'q' ? '"'
                     : '\'';
            out[1] = 0;
            return ln + 1;
        }
    }
    return 0;
}

size_t html_entity_decode(const char *src, size_t n, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; i < n && w + 8 < cap; i++) {
        if (src[i] == '&') {
            char tmp[8];
            size_t consumed = entity_one(src + i, n - i, tmp);
            if (consumed) {
                size_t l = strlen(tmp);
                if (w + l >= cap)
                    break;
                memcpy(out + w, tmp, l);
                w += l;
                i += consumed - 1;
                continue;
            }
        }
        out[w++] = src[i];
    }
    out[w] = 0;
    return w;
}
