/* exogit: buffer + HTML-text + small string helpers. */
#include "exogit.h"

#include <ctype.h>
#include <stdarg.h>
#include <strings.h>

/* ---------------- growable buffer ---------------- */

void buf_init(buf_t *b, size_t cap)
{
    b->len = 0;
    b->cap = cap < 64 ? 64 : cap;
    b->p = (char *)malloc(b->cap);
    if (b->p)
        b->p[0] = 0;
}

void buf_free(buf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void buf_reserve(buf_t *b, size_t add)
{
    if (b->len + add + 1 <= b->cap)
        return;
    size_t ncap = b->cap ? b->cap * 2 : 128;
    while (ncap < b->len + add + 1)
        ncap *= 2;
    char *np = (char *)realloc(b->p, ncap);
    if (!np)
        return;
    b->p = np;
    b->cap = ncap;
}

void buf_putsn(buf_t *b, const char *s, size_t n)
{
    if (!s || !n)
        return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void buf_puts(buf_t *b, const char *s)
{
    buf_putsn(b, s, s ? strlen(s) : 0);
}

void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof tmp) {
        /* overflow: best-effort truncation */
        buf_putsn(b, tmp, sizeof tmp - 1);
        return;
    }
    buf_putsn(b, tmp, (size_t)n);
}

/* ---------------- HTML -> text ---------------- */

static size_t html_decode_entity(const char *s, size_t n, char *out)
{
    /* returns bytes written to out (0..4). s points at '&'. */
    if (n < 2 || s[0] != '&')
        return 0;
    if (n >= 4 && strncmp(s, "&lt;", 4) == 0) { *out = '<'; return 4; }
    if (n >= 4 && strncmp(s, "&gt;", 4) == 0) { *out = '>'; return 4; }
    if (n >= 5 && strncmp(s, "&amp;", 5) == 0) { *out = '&'; return 5; }
    if (n >= 6 && strncmp(s, "&quot;", 6) == 0) { *out = '"'; return 6; }
    if (n >= 6 && strncmp(s, "&apos;", 6) == 0) { *out = '\''; return 6; }
    if (n >= 3 && s[1] == '#') {
        /* numeric entity &#NN; or &#xHH; */
        const char *p = s + 2;
        int hex = 0;
        if (*p == 'x' || *p == 'X') { hex = 1; p++; }
        unsigned long v = 0;
        while (*p && *p != ';') {
            if (hex) {
                if (isxdigit((unsigned char)*p))
                    v = v * 16 + (unsigned long)(*p >= 'a'
                            ? *p - 'a' + 10 : *p >= 'A'
                            ? *p - 'A' + 10 : *p - '0');
            } else if (isdigit((unsigned char)*p)) {
                v = v * 10 + (unsigned long)(*p - '0');
            } else
                break;
            p++;
        }
        if (*p == ';' && v <= 0x10FFFF) {
            /* emit as UTF-8 (best effort, ASCII + latin1) */
            if (v < 0x80) { *out = (char)v; return (size_t)(p + 1 - s); }
            if (v < 0x800) {
                out[0] = (char)(0xC0 | (v >> 6));
                out[1] = (char)(0x80 | (v & 0x3F));
                return (size_t)(p + 1 - s);
            }
            out[0] = (char)(0xE0 | (v >> 12));
            out[1] = (char)(0x80 | ((v >> 6) & 0x3F));
            out[2] = (char)(0x80 | (v & 0x3F));
            return (size_t)(p + 1 - s);
        }
    }
    return 0; /* unknown entity: caller copies '&' literally */
}

size_t html_to_text(const char *html, size_t hlen, char *out, size_t cap)
{
    size_t w = 0;
    int in_tag = 0;
    int in_script = 0; /* inside <script> or <style> */
    for (size_t i = 0; i < hlen; i++) {
        char c = html[i];
        if (c == '<') {
            /* detect script/style open */
            if (i + 6 < hlen &&
                (strncasecmp(html + i, "<script", 7) == 0 ||
                 strncasecmp(html + i, "<style", 6) == 0))
                in_script = 1;
            if (in_script && i + 7 < hlen &&
                (strncasecmp(html + i, "</script", 8) == 0 ||
                 strncasecmp(html + i, "</style", 7) == 0)) {
                in_script = 0;
                /* consume the rest of this close tag */
                while (i < hlen && html[i] != '>')
                    i++;
                continue;
            }
            if (in_script) {
                continue; /* skip script/style contents */
            }
            in_tag = 1;
            continue;
        }
        if (c == '>') {
            in_tag = 0;
            /* treat block-level close as a newline opportunity */
            if (i >= 1 && html[i - 1] == '/') {
                if (w + 1 < cap) out[w++] = '\n';
            }
            continue;
        }
        if (in_tag || in_script)
            continue;
        if (c == '&') {
            char dec[4] = {0};
            size_t used = html_decode_entity(html + i, hlen - i, dec);
            if (used) {
                size_t dl = strlen(dec);
                for (size_t k = 0; k < dl && w + 1 < cap; k++)
                    out[w++] = dec[k];
                i += used - 1;
                continue;
            }
        }
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        /* collapse runs of spaces */
        if (c == ' ') {
            if (w > 0 && out[w - 1] == ' ')
                continue;
        }
        if (w + 1 < cap)
            out[w++] = c;
    }
    if (w > 0 && out[w - 1] == ' ')
        w--;
    if (w + 1 < cap)
        out[w] = 0;
    else if (cap)
        out[cap - 1] = 0;
    return w;
}
