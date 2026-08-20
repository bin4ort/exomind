#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "exomind: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "exomind: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "exomind: out of memory\n");
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---------------- crc32 (IEEE 802.3) ---------------- */

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_make_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

uint32_t crc32_init(void)
{
    return 0xFFFFFFFFu;
}

void crc32_update(uint32_t *crc, const void *data, size_t len)
{
    pthread_once(&crc_once, crc_make_table);
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++)
        *crc = crc_table[(*crc ^ p[i]) & 0xFF] ^ (*crc >> 8);
}

uint32_t crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32_bytes(const void *data, size_t len)
{
    uint32_t c = crc32_init();
    crc32_update(&c, data, len);
    return crc32_final(c);
}

/* ---------------- time ---------------- */

int64_t now_epoch(void)
{
    return (int64_t)time(NULL);
}

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- url decode ---------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && hexval((unsigned char)r[1]) >= 0 &&
            hexval((unsigned char)r[2]) >= 0) {
            *w++ = (char)(hexval((unsigned char)r[1]) * 16 +
                          hexval((unsigned char)r[2]));
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    return (size_t)(w - s);
}

/* ---------------- strings ---------------- */

int stristr(const char *hay, size_t hlen, const char *needle, size_t nlen)
{
    if (nlen == 0)
        return 1;
    if (hlen < nlen)
        return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)hay[i + j]) ==
                   tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen)
            return 1;
    }
    return 0;
}

/* ---------------- minimal JSON ---------------- */

static const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int utf8_encode(uint32_t cp, char out[4])
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
                    int h = hexval((unsigned char)p[i]);
                    if (h < 0)
                        goto fail;
                    cp = cp * 16 + (uint32_t)h;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end &&
                    p[0] == '\\' && p[1] == 'u') {
                    uint32_t lo = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hexval((unsigned char)p[2 + i]);
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
                int m = utf8_encode(cp, tmp);
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
                const char *q = json_parse_string(p, end, &v);
                if (!q)
                    return NULL; /* malformed string: not found */
                p = q;
                return v;
            }
            /* raw scalar: copy the token */
            const char *s = p;
            while (p < end && *p != ',' && *p != '}' && *p != ']')
                p++;
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

/* ---------------- escaping ---------------- */

char *escape_line(const char *s, size_t n)
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

char *json_escape(const char *s, size_t n)
{
    char *out = xmalloc(n * 6 + 3);
    size_t j = 1;
    out[0] = '"';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        default:
            if (c < 0x20) {
                out[j++] = '\\';
                out[j++] = 'u';
                out[j++] = '0';
                out[j++] = '0';
                out[j++] = "0123456789abcdef"[c >> 4];
                out[j++] = "0123456789abcdef"[c & 0xF];
            } else {
                out[j++] = (char)c;
            }
        }
    }
    out[j++] = '"';
    out[j] = 0;
    return out;
}

/* ---------------- vectors (exovec) ---------------- */

static uint64_t vec_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

void vec_embed(const char *text, size_t len, uint8_t *idx, uint8_t *val,
               uint8_t *nnz_out)
{
    uint32_t counts[EXO_VEC_DIM] = {0};
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (!isalnum(c)) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < len && isalnum((unsigned char)text[i]))
            i++;
        size_t wlen = i - start;
        if (wlen < 3) {
            unsigned char wb[2];
            for (size_t p = 0; p < wlen; p++)
                wb[p] = (unsigned char)tolower((unsigned char)text[start + p]);
            counts[vec_hash(wb, wlen) & (EXO_VEC_DIM - 1)]++;
        } else {
            unsigned char tb[3];
            for (size_t p = start; p + 3 <= i; p++) {
                tb[0] = (unsigned char)tolower((unsigned char)text[p]);
                tb[1] = (unsigned char)tolower((unsigned char)text[p + 1]);
                tb[2] = (unsigned char)tolower((unsigned char)text[p + 2]);
                counts[vec_hash(tb, 3) & (EXO_VEC_DIM - 1)]++;
            }
        }
    }
    uint8_t n = 0;
    for (size_t d = 0; d < EXO_VEC_DIM; d++) {
        if (counts[d]) {
            idx[n] = (uint8_t)d;
            val[n] = counts[d] > 255 ? 255 : (uint8_t)counts[d];
            n++;
        }
    }
    *nnz_out = n;
}

int vec_encode(uint8_t nnz, const uint8_t *idx, const uint8_t *val,
               char **out, size_t *outlen)
{
    size_t len = 7 + (size_t)nnz * 2;
    char *p = xmalloc(len);
    p[0] = 'V';
    p[1] = (char)(EXO_VEC_DIM & 0xFF);
    p[2] = (char)((EXO_VEC_DIM >> 8) & 0xFF);
    p[3] = (char)(nnz & 0xFF);
    p[4] = (char)((nnz >> 8) & 0xFF);
    p[5] = (char)((nnz >> 16) & 0xFF);
    p[6] = (char)((nnz >> 24) & 0xFF);
    for (size_t i = 0; i < nnz; i++) {
        p[7 + i * 2] = (char)idx[i];
        p[8 + i * 2] = (char)val[i];
    }
    *out = p;
    *outlen = len;
    return 0;
}

int vec_parse(const char *v, size_t vlen, uint8_t *idx, uint8_t *val,
              uint8_t *nnz_out)
{
    if (!v || vlen < 7 || (unsigned char)v[0] != 'V')
        return -1;
    uint32_t dim = (uint32_t)(unsigned char)v[1] |
                   ((uint32_t)(unsigned char)v[2] << 8);
    if (dim != EXO_VEC_DIM)
        return -1;
    uint32_t nnz = (uint32_t)(unsigned char)v[3] |
                   ((uint32_t)(unsigned char)v[4] << 8) |
                   ((uint32_t)(unsigned char)v[5] << 16) |
                   ((uint32_t)(unsigned char)v[6] << 24);
    if (nnz > EXO_VEC_DIM || vlen != 7 + (size_t)nnz * 2)
        return -1;
    for (size_t i = 0; i < nnz; i++) {
        idx[i] = (uint8_t)v[7 + i * 2];
        val[i] = (uint8_t)v[8 + i * 2];
    }
    *nnz_out = (uint8_t)nnz;
    return 0;
}

/* ---------------- replication helpers ---------------- */

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const void *data, size_t len)
{
    const unsigned char *in = data;
    size_t olen = ((len + 2) / 3) * 4;
    char *out = xmalloc(olen + 1);
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     in[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (i + 2 == len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

static int b64tab_dtab[256];
static pthread_once_t b64_once = PTHREAD_ONCE_INIT;

static void b64_make_dtab(void)
{
    for (int i = 0; i < 256; i++)
        b64tab_dtab[i] = -1;
    for (int i = 0; b64tab[i]; i++)
        b64tab_dtab[(unsigned char)b64tab[i]] = i;
}

unsigned char *b64_decode(const char *s, size_t *outlen)
{
    pthread_once(&b64_once, b64_make_dtab);
    size_t n = strlen(s);
    if (n == 0 || n % 4 != 0)
        return NULL;
    unsigned char *out = xmalloc(n / 4 * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int c0 = b64tab_dtab[(unsigned char)s[i]];
        int c1 = b64tab_dtab[(unsigned char)s[i + 1]];
        int c2 = s[i + 2] == '=' ? 0 : b64tab_dtab[(unsigned char)s[i + 2]];
        int c3 = s[i + 3] == '=' ? 0 : b64tab_dtab[(unsigned char)s[i + 3]];
        if (c0 < 0 || c1 < 0 || (s[i + 2] != '=' && c2 < 0) ||
            (s[i + 3] != '=' && c3 < 0) ||
            (s[i + 2] == '=' && s[i + 3] != '=')) {
            free(out);
            return NULL;
        }
        uint32_t v = ((uint32_t)c0 << 18) | ((uint32_t)c1 << 12) |
                     ((uint32_t)c2 << 6) | (uint32_t)c3;
        out[o++] = (unsigned char)(v >> 16);
        if (s[i + 2] != '=')
            out[o++] = (unsigned char)(v >> 8);
        if (s[i + 3] != '=')
            out[o++] = (unsigned char)v;
    }
    out[o] = 0;
    if (outlen)
        *outlen = o;
    return out;
}

/*
 * Outbound HTTP GET, HTTP/1.0, one-shot connection. Parses the status line
 * and splits the body after the blank line. Intentionally tiny: socket +
 * "GET <path> HTTP/1.0" + read-all + status check.
 */
int http_get(const char *host, int port, const char *path,
             char **body, size_t *blen)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    char req[256 + 2048];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s:%d\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, port);
    if (n < 0 || n >= (int)sizeof req || write(fd, req, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    size_t cap = 1 << 16, len = 0;
    char *buf = xmalloc(cap);
    ssize_t got;
    while ((got = read(fd, buf + len, cap - len - 1)) > 0) {
        len += (size_t)got;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    close(fd);
    if (len < 12 || memcmp(buf, "HTTP/", 5) != 0) {
        free(buf);
        return -1;
    }
    int status = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) {
        free(buf);
        return -1;
    }
    const char *sep = strstr(buf, "\r\n\r\n");
    size_t bl = 0;
    if (sep) {
        bl = len - ((size_t)(sep - buf) + 4);
        if (body) {
            *body = xmalloc(bl + 1);
            memcpy(*body, sep + 4, bl);
            (*body)[bl] = 0;
        }
    } else if (body) {
        *body = xstrdup("");
    }
    if (blen)
        *blen = bl;
    free(buf);
    return status;
}
