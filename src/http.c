/*
 * exomind HTTP layer: a deliberately tiny HTTP/1.1 server whose responses
 * are plain text shaped for LLM token efficiency. No templates, no JSON
 * ceremony unless asked (json=1), one-line results wherever possible.
 */
#include "http.h"
#include "store.h"
#include "util.h"
#include "version.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADERS (16 * 1024)
#define MAX_BODY (16u * 1024u * 1024u)
#define MAX_KEY 4096
#define MAX_VAL (8u * 1024u * 1024u)
#define SNIPPET_MAX 120

static char g_token[128];

void http_set_token(const char *token)
{
    snprintf(g_token, sizeof g_token, "%s", token);
}

typedef struct {
    char method[16];
    char path[2048];
    char query[8192];
    char *body;
    size_t body_len;
    char auth[512];
    int has_ct_json;
} req_t;

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

static void buf_put(buf_t *b, const void *d, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_puts(buf_t *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

static void buf_printf(buf_t *b, const char *fmt, ...)
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

static int ci_prefix(const char *line, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)prefix[i]))
            return 0;
    return 1;
}

/* returns 0 ok, -1 malformed, -2 body too large */
static int read_request(int fd, req_t *r)
{
    char buf[MAX_HEADERS];
    size_t n = 0;
    size_t hdr_end = 0; /* offset just past the header terminator */
    while (n < sizeof buf - 1 && !hdr_end) {
        ssize_t got = read(fd, buf + n, sizeof buf - 1 - n);
        if (got <= 0)
            return -1;
        n += (size_t)got;
        /* scan the newly arrived region for "\r\n\r\n" or "\n\n" */
        size_t from = n > (size_t)got + 4 ? n - (size_t)got - 4 : 0;
        for (size_t i = from; i + 4 <= n; i++) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                hdr_end = i + 4;
                break;
            }
            if (i + 2 <= n && memcmp(buf + i, "\n\n", 2) == 0) {
                hdr_end = i + 2;
                break;
            }
        }
    }
    if (!hdr_end)
        return -1;
    buf[hdr_end - 2] = 0; /* strip the terminator for line parsing */

    char *line = buf;
    char *eol = strstr(line, "\r\n");
    if (!eol)
        eol = strchr(line, '\n');
    if (!eol)
        return -1;
    *eol = 0;

    char *p = line;
    while (*p == ' ')
        p++;
    char *mstart = p;
    while (*p && *p != ' ')
        p++;
    size_t mlen = (size_t)(p - mstart);
    if (mlen >= sizeof r->method)
        mlen = sizeof r->method - 1;
    memcpy(r->method, mstart, mlen);
    r->method[mlen] = 0;
    while (*p == ' ')
        p++;
    char *ustart = p;
    while (*p && *p != ' ')
        p++;
    *p = 0;

    char *qmark = strchr(ustart, '?');
    if (qmark) {
        *qmark = 0;
        size_t pl = strlen(ustart), ql = strlen(qmark + 1);
        if (pl >= sizeof r->path)
            pl = sizeof r->path - 1;
        memcpy(r->path, ustart, pl);
        r->path[pl] = 0;
        if (ql >= sizeof r->query)
            ql = sizeof r->query - 1;
        memcpy(r->query, qmark + 1, ql);
        r->query[ql] = 0;
    } else {
        size_t pl = strlen(ustart);
        if (pl >= sizeof r->path)
            pl = sizeof r->path - 1;
        memcpy(r->path, ustart, pl);
        r->path[pl] = 0;
        r->query[0] = 0;
    }

    char *hdrline = eol + (eol[1] == '\n' ? 2 : 1);
    size_t content_len = 0;
    int have_cl = 0;
    while (*hdrline) {
        char *heol = strstr(hdrline, "\r\n");
        int last = 0;
        if (!heol) {
            heol = hdrline + strlen(hdrline);
            last = 1;
        }
        *heol = 0;
        if (ci_prefix(hdrline, "content-length:")) {
            have_cl = 1;
            content_len = (size_t)strtoull(hdrline + 15, NULL, 10);
        } else if (ci_prefix(hdrline, "authorization:")) {
            const char *v = hdrline + 14;
            while (*v == ' ')
                v++;
            snprintf(r->auth, sizeof r->auth, "%s", v);
        } else if (ci_prefix(hdrline, "content-type:")) {
            if (strstr(hdrline + 13, "json"))
                r->has_ct_json = 1;
        }
        if (last)
            break;
        hdrline = heol + 2;
    }

    if (have_cl && content_len > 0) {
        if (content_len > MAX_BODY)
            return -2;
        r->body = xmalloc(content_len + 1);
        size_t got = 0;
        size_t buffered = n - hdr_end; /* body bytes already read with headers */
        if (buffered > 0) {
            size_t take = buffered < content_len ? buffered : content_len;
            memcpy(r->body, buf + hdr_end, take);
            got = take;
        }
        while (got < content_len) {
            ssize_t g = read(fd, r->body + got, content_len - got);
            if (g <= 0) {
                free(r->body);
                r->body = NULL;
                return -1;
            }
            got += (size_t)g;
        }
        r->body[content_len] = 0;
        r->body_len = content_len;
    }
    return 0;
}

static const char *status_text(int st)
{
    switch (st) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default:  return "Internal Server Error";
    }
}

static void send_response(int fd, int status, const char *ctype,
                          const char *body, size_t blen, int head_only)
{
    char hdr[640];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
                     "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
                     "\r\n",
                     status, status_text(status), ctype, blen);
    if (n > 0)
        (void)!write(fd, hdr, (size_t)n);
    if (!head_only && blen > 0)
        (void)!write(fd, body, blen);
}

static int qp_str(const char *qs, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen > nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
            size_t vlen = seglen - nl - 1;
            if (vlen >= cap)
                vlen = cap - 1;
            memcpy(out, p + nl + 1, vlen);
            out[vlen] = 0;
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static long qp_int(const char *qs, const char *name, long def)
{
    char tmp[64];
    if (!qp_str(qs, name, tmp, sizeof tmp))
        return def;
    char *endp = NULL;
    long v = strtol(tmp, &endp, 10);
    if (!endp || *endp)
        return def;
    return v;
}

static int auth_ok(const req_t *r)
{
    if (!g_token[0])
        return 1;
    const char *got = r->auth;
    if (!ci_prefix(got, "Bearer "))
        return 0;
    got += 7;
    size_t l1 = strlen(g_token), l2 = strlen(got);
    int diff = (int)(l1 ^ l2);
    for (size_t i = 0; i < l1 && i < l2; i++)
        diff |= g_token[i] ^ got[i];
    return diff == 0;
}

static uint32_t rand16(void)
{
    static uint32_t st = 0;
    if (!st)
        st = (uint32_t)(now_ms() ^ (uint64_t)getpid() * 2654435761u);
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st & 0xFFFF;
}

static char *snippet(const char *v, size_t n)
{
    int trunc = n > SNIPPET_MAX;
    size_t take = trunc ? SNIPPET_MAX : n;
    char *e = escape_line(v, take);
    if (trunc) {
        size_t l = strlen(e);
        e = xrealloc(e, l + 4);
        strcpy(e + l, "...");
    }
    return e;
}

static const char *help_text(void)
{
    return
        "# exomind v" EXOMIND_VERSION "\n"
        "\n"
        "External long-term memory for AI agents. Every endpoint answers in\n"
        "plain text optimized for LLM consumption; add `json=1` to any listing\n"
        "endpoint for machine-readable JSON. Errors are `error: <reason>`.\n"
        "\n"
        "## endpoints\n"
        "\n"
        "| method | path          | purpose                                  |\n"
        "|--------|---------------|------------------------------------------|\n"
        "| GET    | /             | this help (the API describes itself)     |\n"
        "| GET    | /ping         | liveness: answers `pong`                 |\n"
        "| POST   | /set          | store `key` -> `value`                   |\n"
        "| GET    | /get?key=k    | read raw value (404 body: `missing`)     |\n"
        "| POST   | /append?key=k | append body to value, newline-separated  |\n"
        "| DELETE | /del?key=k    | delete key                               |\n"
        "| GET    | /list         | keys (prefix=, limit=, offset=, sort=)   |\n"
        "| GET    | /search?q=t   | ranked substring search over keys+values |\n"
        "| POST   | /note        | store body as a timestamped note         |\n"
        "| GET    | /notes       | notes, newest first (q=, limit=, offset=)|\n"
        "| POST   | /batch       | JSON array of ops; one result line each  |\n"
        "| GET    | /stats       | counters and health                      |\n"
        "\n"
        "## writing values\n"
        "\n"
        "`POST /set` accepts three body shapes:\n"
        "\n"
        "1. raw text, key in the URL:\n"
        "   `curl -X POST 'localhost:7654/set?key=greeting' -d 'hello world'`\n"
        "2. urlencoded form: `key=greeting&value=hello+world&ttl=60`\n"
        "3. JSON object: `{\"key\":\"greeting\",\"value\":\"hello\",\"ttl\":60}`\n"
        "\n"
        "`ttl` is seconds (0 = forever). `append:true` appends instead of\n"
        "overwriting.\n"
        "\n"
        "## batch\n"
        "\n"
        "One round-trip for many operations. Elements are arrays\n"
        "`[\"set\",\"k\",\"v\"]`, `[\"get\",\"k\"]`, `[\"del\",\"k\"]`,\n"
        "`[\"append\",\"k\",\"v\"]` or objects\n"
        "`{\"set\":\"k\",\"value\":\"v\",\"ttl\":60}`. Result lines look like\n"
        "`set k ok`, `get k <value>`, `del k ok|missing`.\n"
        "\n"
        "    curl -X POST localhost:7654/batch \\\n"
        "      -d '[{\"set\":\"a\",\"value\":\"1\"},{\"get\":\"a\"},{\"del\":\"a\"}]'\n"
        "\n"
        "## notes\n"
        "\n"
        "`POST /note` accepts raw text and answers `ok <key>`. `GET /notes`\n"
        "lists newest-first; `GET /notes?q=term` filters by content.\n"
        "\n"
        "## auth\n"
        "\n"
        "Start with `--token secret` (or env EXOMIND_TOKEN), then send\n"
        "`Authorization: Bearer secret`. Binds to 127.0.0.1 by default.\n"
        "\n"
        "## durability\n"
        "\n"
        "Append-only log with CRC32 integrity checks, crash-safe truncation\n"
        "recovery, TTL expiry and automatic compaction. Interactive writes are\n"
        "fsynced before acknowledgement; batch ops share one fsync.\n";
}

static void route(req_t *r, store_t *s, buf_t *out, int *status,
                  const char **ctype)
{
    const char *path = r->path;
    char key[MAX_KEY + 1];
    char tmp[4096];

    if (!strcmp(path, "/") || !strcmp(path, "/help") ||
        !strcmp(path, "/spec")) {
        *ctype = "text/markdown; charset=utf-8";
        buf_puts(out, help_text());
        return;
    }
    if (!strcmp(path, "/ping")) {
        buf_puts(out, "pong");
        return;
    }

    if (!strcmp(path, "/stats")) {
        uint64_t entries, log_bytes, dead_bytes, reads, writes, deletes, misses;
        int64_t opened;
        store_stats(s, &entries, &log_bytes, &dead_bytes, &reads, &writes,
                    &deletes, &misses, &opened);
        int64_t uptime = (now_ms() - opened) / 1000;
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_printf(out,
                       "{\"version\":\"%s\",\"uptime_s\":%lld,\"entries\":%llu,"
                       "\"log_bytes\":%llu,\"dead_bytes\":%llu,\"reads\":%llu,"
                       "\"writes\":%llu,\"deletes\":%llu,\"misses\":%llu}",
                       EXOMIND_VERSION, (long long)uptime,
                       (unsigned long long)entries,
                       (unsigned long long)log_bytes,
                       (unsigned long long)dead_bytes,
                       (unsigned long long)reads,
                       (unsigned long long)writes,
                       (unsigned long long)deletes,
                       (unsigned long long)misses);
        } else {
            buf_printf(out,
                       "version: %s\nuptime_s: %lld\nentries: %llu\n"
                       "log_bytes: %llu\ndead_bytes: %llu\nreads: %llu\n"
                       "writes: %llu\ndeletes: %llu\nmisses: %llu\n",
                       EXOMIND_VERSION, (long long)uptime,
                       (unsigned long long)entries,
                       (unsigned long long)log_bytes,
                       (unsigned long long)dead_bytes,
                       (unsigned long long)reads,
                       (unsigned long long)writes,
                       (unsigned long long)deletes,
                       (unsigned long long)misses);
        }
        return;
    }

    if (!strcmp(path, "/get")) {
        if (!qp_str(r->query, "key", key, sizeof key)) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        size_t vlen = 0;
        int64_t ts = 0;
        char *v = store_get(s, key, strlen(key), &vlen, &ts);
        (void)ts;
        if (!v) {
            *status = 404;
            buf_puts(out, "missing");
            return;
        }
        buf_put(out, v, vlen);
        free(v);
        return;
    }

    if (!strcmp(path, "/set")) {
        char *v = NULL;
        size_t vlen = 0;
        long ttl = 0;
        int append = 0;
        int have_key = 0;

        if (r->body_len > 0 &&
            (r->body[0] == '{' || r->has_ct_json)) {
            char *jk = json_field(r->body, r->body_len, "key");
            char *jv = json_field(r->body, r->body_len, "value");
            char *jt = json_field(r->body, r->body_len, "ttl");
            char *ja = json_field(r->body, r->body_len, "append");
            if (jk) {
                snprintf(key, sizeof key, "%s", jk);
                have_key = 1;
                free(jk);
            }
            if (jv) {
                v = jv;
                vlen = strlen(jv);
            }
            if (jt) {
                ttl = strtol(jt, NULL, 10);
                free(jt);
            }
            if (ja) {
                append = (strtol(ja, NULL, 10) != 0) || !strcmp(ja, "true");
                free(ja);
            }
        } else if (r->body_len > 0 && strstr(r->body, "=") != NULL) {
            char *b = xstrdup(r->body);
            char *save = NULL;
            for (char *pair = strtok_r(b, "&", &save); pair;
                 pair = strtok_r(NULL, "&", &save)) {
                char *eq = strchr(pair, '=');
                if (!eq)
                    continue;
                *eq = 0;
                url_decode(pair);
                url_decode(eq + 1);
                if (!strcmp(pair, "key")) {
                    snprintf(key, sizeof key, "%s", eq + 1);
                    have_key = 1;
                } else if (!strcmp(pair, "value")) {
                    v = xstrdup(eq + 1);
                    vlen = strlen(v);
                } else if (!strcmp(pair, "ttl")) {
                    ttl = strtol(eq + 1, NULL, 10);
                } else if (!strcmp(pair, "append")) {
                    append = (strtol(eq + 1, NULL, 10) != 0) ||
                             !strcmp(eq + 1, "true");
                }
            }
            free(b);
        } else {
            if (qp_str(r->query, "key", key, sizeof key))
                have_key = 1;
            ttl = qp_int(r->query, "ttl", 0);
            if (qp_str(r->query, "append", tmp, sizeof tmp))
                append = !strcmp(tmp, "1") || !strcmp(tmp, "true");
            v = xmalloc(r->body_len + 1);
            memcpy(v, r->body, r->body_len);
            v[r->body_len] = 0;
            vlen = r->body_len;
        }

        if (!have_key) {
            *status = 400;
            buf_puts(out, "error: missing key");
            free(v);
            return;
        }
        if (strlen(key) > MAX_KEY) {
            *status = 400;
            buf_puts(out, "error: key too long");
            free(v);
            return;
        }
        if (vlen > MAX_VAL) {
            *status = 413;
            buf_puts(out, "error: value too large");
            free(v);
            return;
        }
        if (store_set(s, key, strlen(key), v, vlen, ttl, append) != 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            free(v);
            return;
        }
        store_sync(s);
        free(v);
        buf_puts(out, "ok");
        return;
    }

    if (!strcmp(path, "/append")) {
        if (!qp_str(r->query, "key", key, sizeof key)) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        if (store_set(s, key, strlen(key), r->body, r->body_len, 0, 1) != 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            return;
        }
        store_sync(s);
        buf_puts(out, "ok");
        return;
    }

    if (!strcmp(path, "/del")) {
        if (strcmp(r->method, "DELETE") && strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use DELETE");
            return;
        }
        if (!qp_str(r->query, "key", key, sizeof key)) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        int existed = store_del(s, key, strlen(key));
        store_sync(s);
        if (existed < 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
        } else if (!existed) {
            *status = 404;
            buf_puts(out, "missing");
        } else {
            buf_puts(out, "ok");
        }
        return;
    }

    if (!strcmp(path, "/list")) {
        char prefix[4096] = "";
        int has_prefix = qp_str(r->query, "prefix", prefix, sizeof prefix);
        long lim = qp_int(r->query, "limit", 100);
        if (lim < 0 || lim > 10000)
            lim = 100;
        long off = qp_int(r->query, "offset", 0);
        if (off < 0)
            off = 0;
        int desc = qp_str(r->query, "sort", tmp, sizeof tmp) &&
                   !strcmp(tmp, "desc");
        kv_t *kvs = NULL;
        size_t n = 0;
        store_query(s, Q_LIST, has_prefix ? prefix : NULL, NULL, desc, &kvs,
                    &n);
        size_t total = n;
        size_t start = (size_t)off, end = start + (size_t)lim;
        if (end > n)
            end = n;
        if (start > end)
            start = end;
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "{\"keys\":[");
            for (size_t i = start; i < end; i++) {
                if (i > start)
                    buf_puts(out, ",");
                char *je = json_escape(kvs[i].key, kvs[i].klen);
                buf_puts(out, je);
                free(je);
            }
            buf_printf(out, "],\"count\":%zu}", total);
        } else {
            for (size_t i = start; i < end; i++)
                buf_printf(out, "%s\n", kvs[i].key);
        }
        kv_free(kvs, n);
        return;
    }

    if (!strcmp(path, "/search")) {
        char q[4096];
        if (!qp_str(r->query, "q", q, sizeof q) || !q[0]) {
            *status = 400;
            buf_puts(out, "error: missing q");
            return;
        }
        long lim = qp_int(r->query, "limit", 10);
        if (lim < 0 || lim > 200)
            lim = 10;
        kv_t *kvs = NULL;
        size_t n = 0;
        store_query(s, Q_SEARCH, NULL, q, 0, &kvs, &n);
        size_t m = (size_t)lim < n ? (size_t)lim : n;
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "{\"results\":[");
            for (size_t i = 0; i < m; i++) {
                if (i)
                    buf_puts(out, ",");
                char *je = json_escape(kvs[i].key, kvs[i].klen);
                buf_printf(out, "{\"key\":%s,\"score\":%lld,\"ts\":%lld}", je,
                           (long long)kvs[i].score, (long long)kvs[i].ts);
                free(je);
            }
            buf_puts(out, "]}");
        } else {
            for (size_t i = 0; i < m; i++) {
                char *sn = snippet(kvs[i].val, kvs[i].vlen);
                buf_printf(out, "%s\t%s\n", kvs[i].key, sn);
                free(sn);
            }
        }
        kv_free(kvs, n);
        return;
    }

    if (!strcmp(path, "/note")) {
        if (r->body_len == 0) {
            *status = 400;
            buf_puts(out, "error: empty note");
            return;
        }
        char nkey[64];
        snprintf(nkey, sizeof nkey, "note:%lld:%08x", (long long)now_ms(),
                 (unsigned)rand16());
        if (store_set(s, nkey, strlen(nkey), r->body, r->body_len, 0, 0) != 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            return;
        }
        store_sync(s);
        buf_printf(out, "ok %s", nkey);
        return;
    }

    if (!strcmp(path, "/notes")) {
        char q[4096];
        int has_q = qp_str(r->query, "q", q, sizeof q) && q[0];
        long lim = qp_int(r->query, "limit", 50);
        if (lim < 0 || lim > 1000)
            lim = 50;
        long off = qp_int(r->query, "offset", 0);
        if (off < 0)
            off = 0;
        kv_t *kvs = NULL;
        size_t n = 0;
        store_query(s, Q_NOTES, NULL, has_q ? q : NULL, 0, &kvs, &n);
        size_t total = n;
        size_t start = (size_t)off, end = start + (size_t)lim;
        if (end > n)
            end = n;
        if (start > end)
            start = end;
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "{\"notes\":[");
            for (size_t i = start; i < end; i++) {
                if (i > start)
                    buf_puts(out, ",");
                char *jk = json_escape(kvs[i].key, kvs[i].klen);
                char *jv = json_escape(kvs[i].val, kvs[i].vlen);
                buf_printf(out, "{\"key\":%s,\"value\":%s,\"ts\":%lld}", jk, jv,
                           (long long)kvs[i].ts);
                free(jk);
                free(jv);
            }
            buf_printf(out, "],\"count\":%zu}", total);
        } else {
            for (size_t i = start; i < end; i++) {
                char *e = escape_line(kvs[i].val, kvs[i].vlen);
                buf_printf(out, "%s\t%s\n", kvs[i].key, e);
                free(e);
            }
        }
        kv_free(kvs, n);
        return;
    }

    if (!strcmp(path, "/batch")) {
        size_t pos = 0;
        const char *elem;
        size_t elen;
        int any = 0;
        while (json_array_each(r->body, r->body_len, &pos, &elem, &elen)) {
            any = 1;
            const char *p = elem;
            const char *end = elem + elen;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                               *p == '\r'))
                p++;
            if (p < end && *p == '[') {
                size_t nstr = 0;
                char **strs = json_arr_strings(elem, elen, &nstr);
                if (nstr >= 2) {
                    const char *op = strs[0];
                    const char *k = strs[1];
                    if (!strcmp(op, "set") && nstr >= 3) {
                        long ttl = nstr >= 4 ? strtol(strs[3], NULL, 10) : 0;
                        int rc = store_set(s, k, strlen(k), strs[2],
                                           strlen(strs[2]), ttl, 0);
                        buf_printf(out, "set %s %s\n", k,
                                   rc == 0 ? "ok" : "error");
                    } else if (!strcmp(op, "append") && nstr >= 3) {
                        int rc = store_set(s, k, strlen(k), strs[2],
                                           strlen(strs[2]), 0, 1);
                        buf_printf(out, "append %s %s\n", k,
                                   rc == 0 ? "ok" : "error");
                    } else if (!strcmp(op, "get")) {
                        size_t vlen = 0;
                        char *v = store_get(s, k, strlen(k), &vlen, NULL);
                        if (v) {
                            char *e = escape_line(v, vlen);
                            buf_printf(out, "get %s %s\n", k, e);
                            free(e);
                            free(v);
                        } else {
                            buf_printf(out, "get %s missing\n", k);
                        }
                    } else if (!strcmp(op, "del")) {
                        int existed = store_del(s, k, strlen(k));
                        buf_printf(out, "del %s %s\n", k,
                                   existed > 0 ? "ok" : "missing");
                    } else {
                        buf_puts(out, "error: bad batch op\n");
                    }
                } else {
                    buf_puts(out, "error: bad batch element\n");
                }
                for (size_t i = 0; i < nstr; i++)
                    free(strs[i]);
                free(strs);
            } else if (p < end && *p == '{') {
                char *op_set = json_field(elem, elen, "set");
                char *op_get = json_field(elem, elen, "get");
                char *op_del = json_field(elem, elen, "del");
                char *op_app = json_field(elem, elen, "append");
                char *val = json_field(elem, elen, "value");
                char *ttls = json_field(elem, elen, "ttl");
                long ttl = ttls ? strtol(ttls, NULL, 10) : 0;
                if (op_set) {
                    int rc = store_set(s, op_set, strlen(op_set), val ? val : "",
                                       val ? strlen(val) : 0, ttl, 0);
                    buf_printf(out, "set %s %s\n", op_set,
                               rc == 0 ? "ok" : "error");
                } else if (op_app) {
                    int rc = store_set(s, op_app, strlen(op_app), val ? val : "",
                                       val ? strlen(val) : 0, 0, 1);
                    buf_printf(out, "append %s %s\n", op_app,
                               rc == 0 ? "ok" : "error");
                } else if (op_get) {
                    size_t vlen = 0;
                    char *v = store_get(s, op_get, strlen(op_get), &vlen, NULL);
                    if (v) {
                        char *e = escape_line(v, vlen);
                        buf_printf(out, "get %s %s\n", op_get, e);
                        free(e);
                        free(v);
                    } else {
                        buf_printf(out, "get %s missing\n", op_get);
                    }
                } else if (op_del) {
                    int existed = store_del(s, op_del, strlen(op_del));
                    buf_printf(out, "del %s %s\n", op_del,
                               existed > 0 ? "ok" : "missing");
                } else {
                    buf_puts(out, "error: bad batch element\n");
                }
                free(op_set);
                free(op_get);
                free(op_del);
                free(op_app);
                free(val);
                free(ttls);
            } else {
                buf_puts(out, "error: bad batch element\n");
            }
        }
        if (!any) {
            *status = 400;
            buf_puts(out, "error: empty batch");
            return;
        }
        store_sync(s);
        return;
    }

    *status = 404;
    buf_puts(out, "error: unknown path");
}

void http_handle_conn(int fd, store_t *s)
{
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    req_t r;
    memset(&r, 0, sizeof r);
    int rc = read_request(fd, &r);
    if (rc == -2) {
        send_response(fd, 413, "text/plain; charset=utf-8",
                      "error: body too large\n", 21, 0);
        return;
    }
    if (rc != 0) {
        send_response(fd, 400, "text/plain; charset=utf-8",
                      "error: bad request\n", 19, 0);
        return;
    }
    if (!strcmp(r.method, "OPTIONS")) {
        send_response(fd, 204, "text/plain", "", 0, 0);
        free(r.body);
        return;
    }
    if (strcmp(r.method, "GET") && strcmp(r.method, "POST") &&
        strcmp(r.method, "DELETE") && strcmp(r.method, "HEAD")) {
        send_response(fd, 405, "text/plain; charset=utf-8",
                      "error: method not allowed\n", 25, 0);
        free(r.body);
        return;
    }
    if (!auth_ok(&r)) {
        send_response(fd, 401, "text/plain; charset=utf-8",
                      "error: unauthorized\n", 19, 0);
        free(r.body);
        return;
    }

    int status = 200;
    const char *ctype = "text/plain; charset=utf-8";
    buf_t out = {0};
    route(&r, s, &out, &status, &ctype);
    int head = !strcmp(r.method, "HEAD");
    send_response(fd, status, ctype, out.p ? out.p : "", out.len, head);
    free(out.p);
    free(r.body);
}
