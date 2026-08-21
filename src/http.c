/*
 * exomind HTTP layer: a deliberately tiny HTTP/1.1 server whose responses
 * are plain text shaped for LLM token efficiency. No templates, no JSON
 * ceremony unless asked (json=1), one-line results wherever possible.
 */
#include "http.h"
#include "store.h"
#include "util.h"
#include "version.h"
#include "../common/exo.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int qp_str(const char *q, const char *name, char *out, size_t cap);
static int mkdir_p_path(const char *path);


int g_rate_limit_active = 0;

/* project memory: a second store file living in the agent's project root
 * (auto-detected, or --project-root). Keys with the `p:` prefix and any
 * request with proj=1 operate on it. */
static store_t *g_proj = NULL;
static char g_proj_root[2048] = "";
static char g_backup_dir[2048] = "";
static char g_mandate[8192] = "";

void http_set_project(store_t *proj, const char *root)
{
    g_proj = proj;
    if (root)
        snprintf(g_proj_root, sizeof g_proj_root, "%s", root);
}

void http_set_backup_dir(const char *dir)
{
    if (dir)
        snprintf(g_backup_dir, sizeof g_backup_dir, "%s", dir);
}

void http_set_mandate(const char *text)
{
    if (text)
        snprintf(g_mandate, sizeof g_mandate, "%s", text);
}

const char *http_project_root(void)
{
    return g_proj_root;
}

/* pick the store for a request: proj=1 or a `p:` key routes to the
 * project store (falling back to the main store when none is attached) */
static store_t *pick_store(store_t *s, const char *key, const char *query)
{
    char proj[8];
    if (query && qp_str(query, "proj", proj, sizeof proj) && proj[0] == '1')
        return g_proj ? g_proj : s;
    if (key && key[0] == 'p' && key[1] == ':')
        return g_proj ? g_proj : s;
    return s;
}

/* copy the live data file into the backup dir, timestamped; keep the 24
 * newest copies. Returns 0 on success. */
int exo_backup_now(store_t *s, char *err, size_t errsz)
{
    if (!g_backup_dir[0])
        return 0;
    struct stat st;
    if (stat(g_backup_dir, &st) != 0) {
        if (mkdir_p_path(g_backup_dir) != 0) {
            snprintf(err, errsz, "cannot create backup dir %s", g_backup_dir);
            return -1;
        }
    }
    char stamp[64];
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono); /* unique even within one second */
    snprintf(stamp, sizeof stamp, "%lld-%06ld", (long long)now_epoch(),
             (long)mono.tv_nsec / 1000);
    char dst[4096];
    snprintf(dst, sizeof dst, "%s/exomind-%s.dat", g_backup_dir, stamp);
    FILE *in = fopen(store_path(s), "rb");
    if (!in) {
        snprintf(err, errsz, "cannot open %s", store_path(s));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        snprintf(err, errsz, "cannot write %s", dst);
        return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    /* prune: keep the 24 newest */
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "ls -1t %s/exomind-*.dat 2>/dev/null | tail -n +25 | "
             "xargs -r rm -f", g_backup_dir);
    (void)system(cmd);
    (void)err;
    (void)errsz;
    return 0;
}

/* mkdir -p for a single path (used by backups) */
int mkdir_p_path(const char *path)
{
    char tmp[4096];
    if (strlen(path) >= sizeof tmp)
        return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
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
#define MAX_TOKENS 64

typedef struct {
    char token[256];
    size_t tlen;
    int readonly;
    char *prefix;
    size_t plen;
} tok_t;

static tok_t g_toks[MAX_TOKENS];
static size_t g_ntoks = 0;

static int find_tok(const char *tok, size_t len)
{
    for (size_t i = 0; i < g_ntoks; i++)
        if (g_toks[i].tlen == len && memcmp(g_toks[i].token, tok, len) == 0)
            return (int)i;
    return -1;
}

void http_set_token(const char *token)
{
    int idx = find_tok(token, strlen(token));
    if (idx < 0) {
        if (g_ntoks >= MAX_TOKENS)
            return;
        idx = (int)g_ntoks++;
        snprintf(g_toks[idx].token, sizeof g_toks[idx].token, "%s", token);
        g_toks[idx].tlen = strlen(g_toks[idx].token);
    }
    g_toks[idx].readonly = 0;
    free(g_toks[idx].prefix);
    g_toks[idx].prefix = NULL;
    g_toks[idx].plen = 0;
}

/*
 * Token file syntax: one token per line, `#` comments and blank lines
 * ignored. A line is `token` optionally followed by colon-separated
 * modifiers: `ro` makes the token read-only, `scope=<prefix>*` (or
 * `prefix=<prefix>`) restricts it to keys under that prefix.
 */
int http_load_tokens(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "exomind: cannot open tokens file %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    char line[1024];
    int added = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (l == sizeof line - 1) {
            fprintf(stderr, "exomind: tokens: line too long, skipping\n");
            continue;
        }
        if (!l || line[0] == '#')
            continue;
        char *mod = strchr(line, ':');
        if (mod) {
            *mod = 0;
            mod++;
        }
        if (!line[0] || strlen(line) >= sizeof g_toks[0].token) {
            fprintf(stderr, "exomind: tokens: bad token, skipping\n");
            continue;
        }
        int readonly = 0;
        char *prefix = NULL;
        int bad = 0;
        while (mod && *mod) {
            if (strncmp(mod, "scope=", 6) == 0) {
                prefix = xstrdup(mod + 6);
                size_t pl = strlen(prefix);
                if (pl && prefix[pl - 1] == '*')
                    prefix[pl - 1] = 0;
                mod = NULL; /* scope value may itself contain ':' */
            } else if (strncmp(mod, "prefix=", 7) == 0) {
                prefix = xstrdup(mod + 7);
                mod = NULL;
            } else {
                char *next = strchr(mod, ':');
                if (next)
                    *next = 0;
                if (!strcmp(mod, "ro")) {
                    readonly = 1;
                } else {
                    fprintf(stderr, "exomind: tokens: bad modifier %s, skipping\n",
                            mod);
                    bad = 1;
                }
                mod = next ? next + 1 : NULL;
            }
        }
        if (bad) {
            free(prefix);
            continue;
        }
        int idx = find_tok(line, strlen(line));
        if (idx < 0) {
            if (g_ntoks >= MAX_TOKENS) {
                fprintf(stderr, "exomind: tokens: too many tokens, skipping\n");
                free(prefix);
                continue;
            }
            idx = (int)g_ntoks++;
            snprintf(g_toks[idx].token, sizeof g_toks[idx].token, "%s", line);
            g_toks[idx].tlen = strlen(g_toks[idx].token);
        }
        g_toks[idx].readonly = readonly;
        free(g_toks[idx].prefix);
        g_toks[idx].prefix = prefix;
        g_toks[idx].plen = prefix ? strlen(prefix) : 0;
        added++;
    }
    fclose(f);
    return added;
}

typedef struct {
    char method[16];
    char path[2048];
    char query[8192];
    char *body;
    size_t body_len;
    char auth[512];
    int has_ct_json;
    int tok_idx; /* matched token in g_toks, -1 if auth is off */
} req_t;

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
        for (size_t i = from; i + 2 <= n; i++) {
            if (i + 4 <= n && memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                hdr_end = i + 4;
                break;
            }
            if (memcmp(buf + i, "\n\n", 2) == 0) {
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
            heol = strchr(hdrline, '\n');
            if (!heol) {
                heol = hdrline + strlen(hdrline);
                last = 1;
            }
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
            char *at = r->auth + strlen(r->auth);
            while (at > r->auth && (at[-1] == ' ' || at[-1] == '\t'))
                *--at = 0;
        } else if (ci_prefix(hdrline, "content-type:")) {
            if (strstr(hdrline + 13, "json"))
                r->has_ct_json = 1;
        }
        if (last)
            break;
        hdrline = heol + (heol[1] == '\n' ? 2 : 1);
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
    case 403: return "Forbidden";
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

/* qp_str for the "key" parameter; returns 2 when the value does not fit cap */
static int qp_key(const char *qs, char *out, size_t cap)
{
    size_t nl = strlen("key");
    const char *p = qs;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen > nl && strncmp(p, "key", nl) == 0 && p[nl] == '=') {
            size_t vlen = seglen - nl - 1;
            if (vlen >= cap)
                return 2;
            memcpy(out, p + nl + 1, vlen);
            out[vlen] = 0;
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* true when the body looks like a urlencoded form: some pair uses a known
 * field name. Raw-text bodies may contain '=' (e.g. "a=b") and must not be
 * misread as forms. */
static int is_form_body(const char *b, size_t len)
{
    char *s = xstrndup(b, len);
    char *save = NULL;
    int is = 0;
    for (char *pair = strtok_r(s, "&", &save); pair && !is;
         pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (!eq)
            continue;
        size_t nl = (size_t)(eq - pair);
        if ((nl == 3 && memcmp(pair, "key", 3) == 0) ||
            (nl == 5 && memcmp(pair, "value", 5) == 0) ||
            (nl == 3 && memcmp(pair, "ttl", 3) == 0) ||
            (nl == 6 && memcmp(pair, "append", 6) == 0))
            is = 1;
    }
    free(s);
    return is;
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

static int auth_ok(req_t *r)
{
    r->tok_idx = -1;
    if (g_ntoks == 0)
        return 1;
    const char *got = r->auth;
    if (!ci_prefix(got, "Bearer "))
        return 0;
    got += 7;
    size_t l1 = strlen(got);
    int idx = find_tok(got, l1);
    if (idx < 0)
        return 0;
    r->tok_idx = idx;
    return 1;
}

static int tok_allows_key(const tok_t *t, const char *key, size_t klen)
{
    return !t || !t->prefix ||
           (klen >= t->plen && memcmp(key, t->prefix, t->plen) == 0);
}

static int tok_can_write(const tok_t *t, const char *key, size_t klen)
{
    return tok_allows_key(t, key, klen) && (!t || !t->readonly);
}

/* drop every record whose key falls outside the token's prefix scope */
static void filter_scope(kv_t *kvs, size_t *n, const tok_t *t)
{
    if (!t || !t->prefix)
        return;
    size_t w = 0;
    for (size_t i = 0; i < *n; i++) {
        if (tok_allows_key(t, kvs[i].key, kvs[i].klen)) {
            if (w != i)
                kvs[w] = kvs[i];
            w++;
        } else {
            free(kvs[i].key);
            free(kvs[i].val);
        }
    }
    *n = w;
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

/* @nonnull */
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

/*
 * Delete key and, when the token may write it, also drop the associated
 * vector (vec:<key>). Returns whether the main key existed.
 */
static int del_key_cascade(store_t *s, const tok_t *tok, const char *key,
                           size_t klen, const char *query)
{
    store_t *ts5 = pick_store(s, key, query);
    int existed = store_del(ts5, key, klen);
    if (klen + EXO_VEC_KEY_PREFIX_LEN <= MAX_KEY) {
        char vkey[MAX_KEY + 1];
        memcpy(vkey, EXO_VEC_KEY_PREFIX, EXO_VEC_KEY_PREFIX_LEN);
        memcpy(vkey + EXO_VEC_KEY_PREFIX_LEN, key, klen);
        size_t vklen = klen + EXO_VEC_KEY_PREFIX_LEN;
        if (tok_can_write(tok, vkey, vklen))
            store_del(s, vkey, vklen);
    }
    return existed;
}

/*
 * Compute the embedding of body and store it durably under vec:<k>.
 * Returns 1 stored, 0 denied, -1 key too long or store failure.
 * The caller formats the reply line.
 */
static int http_embed(store_t *s, const tok_t *tok, const char *k, size_t klen,
                      const char *body, size_t blen, long ttl)
{
    if (klen == 0 || klen + EXO_VEC_KEY_PREFIX_LEN > MAX_KEY)
        return -1;
    char vkey[MAX_KEY + 1];
    memcpy(vkey, EXO_VEC_KEY_PREFIX, EXO_VEC_KEY_PREFIX_LEN);
    memcpy(vkey + EXO_VEC_KEY_PREFIX_LEN, k, klen);
    size_t vklen = klen + EXO_VEC_KEY_PREFIX_LEN;
    if (!tok_can_write(tok, vkey, vklen))
        return 0;
    uint8_t idx[EXO_VEC_DIM], val[EXO_VEC_DIM], nnz;
    vec_embed(body, blen, idx, val, &nnz);
    char *enc = NULL;
    size_t elen = 0;
    vec_encode(nnz, idx, val, &enc, &elen);
    int rc = store_set(s, vkey, vklen, enc, elen, ttl, 0);
    free(enc);
    return rc == 0 ? 1 : -1;
}

const char *http_help_text(void)
{
    return
        "# exomind v" EXOMIND_VERSION "\n"
        "\n"
        "External long-term memory for AI agents. Every endpoint answers in\n"
        "plain text optimized for LLM consumption; add `json=1` to any listing\n"
        "endpoint for machine-readable JSON. Errors are `error: <reason>`.\n"
        "\n"
        "exomind is one module of the exomind stack. Run `exomind --help modules`\n"
        "for the whole stack's spec (every module prints it), or run any sibling\n"
        "binary bare for its own: `exosched` (reminders + delivery receipts),\n"
        "`exoflow` (swarm orchestrator), `exoqms` (quality management),\n"
        "`exocrawl` (web research), `exocontext` (context continuity), `exodoc`\n"
        "(documentation audit, batch), `exokit` (behavioral dev kit, batch).\n"
        "Every sibling is reachable by name from any one of them.\n"
        "\n"
        "## running this binary\n"
        "\n"
        "No arguments prints this guide and exits (nothing is bound). A first\n"
        "argument starting with `/` runs ONE operation in-process and prints the\n"
        "response body, e.g. `exomind /set?key=greeting --body 'hello'` or\n"
        "`exomind /search?q=parser`. Server mode binds a port only with `--serve`\n"
        "(or an explicit `--port`); `exomind-server` is the same binary in MCP\n"
        "(stdio JSON-RPC) mode. `--update` fetches, rebuilds and reinstalls\n"
        "itself from the source tree.\n"
        "\n"
        "## endpoints\n"
        "\n"
        "| method | path          | purpose                                  |\n"
        "|--------|---------------|------------------------------------------|\n"
        "| GET    | /             | this help (the API describes itself)     |\n"
        "| GET    | /ping         | liveness: answers `pong`                 |\n"
        "| GET    | /repl?from=N  | raw log records from byte offset N       |\n"
        "| POST   | /set          | store `key` -> `value`                   |\n"
        "| GET    | /get?key=k    | read raw value (404 body: `missing`)     |\n"
        "| POST   | /append?key=k | append body to value, newline-separated  |\n"
        "| DELETE | /del?key=k    | delete key                               |\n"
        "| GET    | /list         | keys (prefix=, limit=, offset=, sort=)   |\n"
        "| GET    | /search?q=t   | ranked substring search over keys+values |\n"
        "| GET    | /embed?key=k  | read stored vector (`dim 256 i:v ...`)   |\n"
        "| POST   | /embed?key=k  | embed raw body, store as `vec:<k>`       |\n"
        "| DELETE | /embed?key=k  | delete vector                            |\n"
        "| POST   | /sim?k=10     | nearest vectors to body, one per line    |\n"
        "| POST   | /note        | store body as a timestamped note         |\n"
        "| GET    | /notes       | notes, newest first (q=, limit=, offset=)|\n"
        "| POST   | /batch       | JSON array of ops; one result line each  |\n"
        "| GET    | /stats       | counters and health                      |\n"
        "| POST   | /backup      | write a timestamped backup copy          |\n"
        "| GET    | /project     | project store location                   |\n"
        "| POST   | /outdate     | mark key outdated (kept in history)      |\n"
        "| GET    | /outdated    | outdated marker + history of a key       |\n"
        "| POST   | /revive      | clear an outdated marker                 |\n"
        "| POST   | /link        | associate two memories (from=, to=, rel=)|\n"
        "| GET    | /assoc       | associations of a key (both directions)  |\n"
        "| GET    | /recall      | search + outdated + history + assoc      |\n"
        "| GET    | /mandate     | mandatory briefing (ack: agent:<id>:ready)|\n"
        "note: query values (reasons, values with spaces) must be URL-\n"
        "encoded (use %20 for spaces) or the request is rejected\n"
        "\n"
        "## memory model\n"
        "\n"
        "Two stores: the main memory file holds general facts; a project\n"
        "memory file lives in the agent's project root (auto-detected\n"
        "upward from the CWD via a .git/.exo marker, or --project-root), at\n"
        "<root>/.exo/project.dat. Keys with the `p:` prefix and any request\n"
        "with `proj=1` operate on the project store; /search and /recall\n"
        "cover both. Outdated memories are never silently deleted:\n"
        "/outdate keeps the value, records history:<key> and marks\n"
        "outdated:<key>, so past errors and fixes stay cross-referenceable\n"
        "via /link and /assoc.\n"
        "| GET    | /snapshot    | lossless dump of all live records        |\n"
        "| POST   | /restore     | replace entire store from a snapshot     |\n"
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
        "`[\"append\",\"k\",\"v\"]`, `[\"embed\",\"k\",\"text\"]` or objects\n"
        "`{\"set\":\"k\",\"value\":\"v\",\"ttl\":60}`. Result lines look like\n"
        "`set k ok`, `get k <value>`, `del k ok|missing`, `embed k ok <dim>`.\n"
        "`note` is not a batch op; store notes with POST /note.\n"
        "\n"
        "    curl -X POST localhost:7654/batch \\\n"
        "      -d '[{\"set\":\"a\",\"value\":\"1\"},{\"get\":\"a\"},{\"del\":\"a\"}]'\n"
        "\n"
        "## notes\n"
        "\n"
        "`POST /note` accepts raw text and answers `ok <key>`. `GET /notes`\n"
        "lists newest-first; `GET /notes?q=term` filters by content.\n"
        "\n"
        "## snapshot & restore\n"
        "\n"
        "`GET /snapshot` dumps every live record (no tombstones, no expired\n"
        "keys) as a plain-text, length-prefixed dump that `/restore` reads\n"
        "back verbatim, so values may contain tabs, newlines or binary:\n"
        "\n"
        "    exomind-snapshot-v1\n"
        "    <klen>\\t<vlen>\\t<key><value>\\n    (one record per line)\n"
        "\n"
        "`POST /restore` with such a body atomically replaces the entire\n"
        "store (temp file + fsync + rename) and answers `ok <n_records>`.\n"
        "A malformed body answers `error: bad snapshot` and leaves the store\n"
        "untouched. TTLs and write timestamps are not preserved.\n"
        "\n"
        "## vectors (exovec)\n"
        "\n"
        "`POST /embed?key=k` hashes the raw body into a fixed 256-dimension\n"
        "count vector and stores it durably under the ordinary key `vec:<k>`\n"
        "(vectors are ordinary keys: TTLs, snapshots, restore, scoping and\n"
        "crash recovery all apply to them as-is). Answer: `ok <k> 256`.\n"
        "`ttl=` works: `POST /embed?key=k&ttl=60`.\n"
        "\n"
        "Embedding is local and deterministic, no model or network: the text\n"
        "is lowercased and split into words on any non-alphanumeric byte;\n"
        "each character 3-gram of a word is FNV-1a-hashed mod 256 into the\n"
        "count vector, and words shorter than 3 characters are hashed whole.\n"
        "Counts are clamped to 255; cosine similarity divides by both norms,\n"
        "so raw counts need no normalization step. The same text always\n"
        "produces the same vector.\n"
        "\n"
        "`GET /embed?key=k` answers `dim 256` followed by each nonzero\n"
        "dimension as `index:count`, ascending by index:\n"
        "`dim 256 12:3 45:1`. A key whose value is not a well-formed vector\n"
        "answers `error: bad vector`; vectors stored under other keys are\n"
        "never fed into the sim index (and /append on a vec: key corrupts\n"
        "it, so don't).\n"
        "\n"
        "`POST /sim` embeds the body and answers the top-k nearest stored\n"
        "vectors (default 10, `k=` up to 1000), one per line, best first:\n"
        "`key<TAB>similarity` where similarity is cosine in [0,1] with 6\n"
        "decimals and the `vec:` prefix is stripped. Vectors with no shared\n"
        "dimension and an empty query match nothing. `json=1` switches to\n"
        "`{\"results\":[{\"key\":\"k\",\"sim\":0.934512},...]}`. An\n"
        "in-memory index (rebuilt at load, kept in sync with writes) makes\n"
        "the scan cover only vectors.\n"
        "\n"
        "`DELETE /embed?key=k` removes the vector. `DELETE /del?key=k` also\n"
        "drops the vector for `k` when the token is allowed to write it.\n"
        "\n"
        "## auth\n"
        "\n"
        "Start with `--token secret` (or env EXOMIND_TOKEN) to require\n"
        "`Authorization: Bearer secret` on every request. Additional tokens\n"
        "can be loaded from a file with `--tokens <file>`; one token per\n"
        "line (`#` comments and blank lines ignored):\n"
        "\n"
        "    agent2                   full access\n"
        "    reader:ro                read-only (no writes, no restore)\n"
        "    logs:scope=logs/*        only keys under the `logs/` prefix\n"
        "    logro:ro:scope=logs/*    read-only and prefix-scoped\n"
        "\n"
        "Prefix-scoped tokens can only read and write keys matching their\n"
        "prefix; enforced on /get /set /append /del /list /search /notes\n"
        "/batch /snapshot /embed /sim. Vectors live under `vec:` keys, so a\n"
        "scope like `logs:scope=logs/*` covers vectors only with\n"
        "`scope=vec:logs/*`; scope `vec:*` for all vectors. Read-only tokens\n"
        "are denied on /set /append /del /note /restore, POST and DELETE\n"
        "/embed, and any write element of /batch, but may GET /embed and\n"
        "POST /sim. Violations answer `error: denied` (403). /restore\n"
        "additionally requires a full-access token; a scoped /snapshot dumps\n"
        "only in-scope records. Without --token/--tokens auth stays off and\n"
        "everything is allowed.\n"
        "\n"
        "## durability\n"
        "\n"
        "Append-only log with CRC32 integrity checks, crash-safe truncation\n"
        "recovery, TTL expiry and automatic compaction. Interactive writes are\n"
        "fsynced before acknowledgement; batch ops share one fsync.\n";
}

typedef struct {
    buf_t *out;
    const tok_t *tok;
} snap_ctx_t;

static int snap_emit(void *ctx, const char *key, size_t klen,
                     const char *val, size_t vlen)
{
    snap_ctx_t *c = ctx;
    if (!tok_allows_key(c->tok, key, klen))
        return 0;
    buf_printf(c->out, "%zu\t%zu\t", klen, vlen);
    buf_put(c->out, key, klen);
    buf_put(c->out, val, vlen);
    buf_puts(c->out, "\n");
    return 0;
}

/*
 * Parse a snapshot body: "exomind-snapshot-v1\n" then one record per line of
 * the form <klen>\t<vlen>\t<key><value>\n with raw, length-prefixed bytes.
 * Returns 0 and a malloc'd kv array, or -1 on any malformed input.
 */
static int parse_snapshot(const char *body, size_t len, kv_t **out,
                          size_t *n_out)
{
    static const char hdr[] = "exomind-snapshot-v1\n";
    if (len < sizeof hdr - 1 || memcmp(body, hdr, sizeof hdr - 1) != 0)
        return -1;
    const char *p = body + sizeof hdr - 1;
    const char *end = body + len;
    kv_t *arr = NULL;
    size_t cnt = 0, cap = 0;
    while (p < end) {
        char *ep;
        long klen = strtol(p, &ep, 10);
        if (ep == p || *ep != '\t')
            goto fail;
        p = ep + 1;
        long vlen = strtol(p, &ep, 10);
        if (ep == p || *ep != '\t')
            goto fail;
        if (klen <= 0 || klen > MAX_KEY || vlen < 0 || vlen > MAX_VAL)
            goto fail;
        p = ep + 1;
        size_t reclen = (size_t)klen + (size_t)vlen;
        if ((size_t)(end - p) < reclen)
            goto fail;
        const char *key = p;
        const char *val = key + (size_t)klen;
        p += reclen;
        if (p >= end || *p != '\n')
            goto fail;
        p++;
        if (cnt == cap) {
            cap = cap ? cap * 2 : 64;
            arr = xrealloc(arr, cap * sizeof(kv_t));
        }
        arr[cnt].key = xstrndup(key, (size_t)klen);
        arr[cnt].klen = (size_t)klen;
        arr[cnt].val = xmalloc((size_t)vlen + 1);
        if (vlen)
            memcpy(arr[cnt].val, val, (size_t)vlen);
        arr[cnt].val[vlen] = 0;
        arr[cnt].vlen = (size_t)vlen;
        arr[cnt].has_val = 1;
        arr[cnt].ts = 0;
        arr[cnt].score = 0;
        cnt++;
    }
    *out = arr;
    *n_out = cnt;
    return 0;
fail:
    for (size_t i = 0; i < cnt; i++) {
        free(arr[i].key);
        free(arr[i].val);
    }
    free(arr);
    return -1;
}

static void route(req_t *r, store_t *s, buf_t *out, int *status,
                  const char **ctype)
{

    const char *path = r->path;
    char key[MAX_KEY + 1];
    char tmp[4096];
    const tok_t *tok = r->tok_idx >= 0 ? &g_toks[r->tok_idx] : NULL;

    if (!strcmp(path, "/") || !strcmp(path, "/help") ||
        !strcmp(path, "/spec")) {
        *ctype = "text/markdown; charset=utf-8";
        buf_puts(out, http_help_text());
        return;
    }
    if (!strcmp(path, "/ping")) {
        buf_puts(out, "pong");
        return;
    }

    /*
     * Replication log reader: replay raw records starting at a byte offset.
     * The first line is the batch header, followed by one base64-encoded
     * raw log record per line. up to 512 records or 1 MiB, whichever comes
     * first; a torn tail stops the batch, `next` is the offset after the
     * last complete record. When `from` itself is not a record boundary the
     * server answers `repl error torn <off>` and the follower re-syncs.
     */
    if (!strcmp(path, "/repl")) {
        if (strcmp(r->method, "GET")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        char froms[64];
        if (!qp_str(r->query, "from", froms, sizeof froms) || froms[0] == 0 ||
            froms[0] == '-')
            goto unknown_path; /* bad params: console exit 2 */
        char *endp = NULL;
        unsigned long long from = strtoull(froms, &endp, 10);
        if (endp == froms || *endp != 0)
            goto unknown_path;
        enum { REPL_MAX_RECS = 512, REPL_MAX_BYTES = 1u << 20 };
        unsigned char *rbuf = xmalloc(REPL_MAX_BYTES);
        buf_t lines = {0};
        uint64_t off = from, next = from;
        size_t bytes = 0;
        int count = 0;
        store_lock(s);
        while (count < REPL_MAX_RECS && bytes < REPL_MAX_BYTES) {
            size_t consumed = 0;
            int rc = store_raw_at(s, off, rbuf, REPL_MAX_BYTES, &consumed);
            if (rc == 0)
                break; /* end of log */
            if (rc < 0) {
                if (count == 0) {
                    store_unlock(s);
                    http_buf_free(&lines);
                    free(rbuf);
                    *status = 500;
                    buf_printf(out, "repl error torn %llu\n",
                               (unsigned long long)off);
                    return;
                }
                break; /* stop at a torn tail */
            }
            char *b64 = b64_encode(rbuf, consumed);
            buf_puts(&lines, b64);
            buf_puts(&lines, "\n");
            free(b64);
            off += consumed;
            next = off;
            bytes += consumed;
            count++;
        }
        store_unlock(s);
        free(rbuf);
        buf_printf(out, "repl from %llu next %llu count %d\n",
                   (unsigned long long)from, (unsigned long long)next,
                   count);
        buf_put(out, lines.p, lines.len);
        http_buf_free(&lines);
        return;
    }

    if (!strcmp(path, "/stats")) {
        uint64_t entries, log_bytes, dead_bytes, reads, writes, deletes, misses;
        int64_t opened;
        store_stats(s, &entries, &log_bytes, &dead_bytes, &reads, &writes,
                    &deletes, &misses, &opened);
        int64_t uptime = (now_ms() - opened) / 1000;
        uint64_t pentries = 0;
        if (g_proj)
            store_stats(g_proj, &pentries, &log_bytes, &dead_bytes, &reads,
                        &writes, &deletes, &misses, &opened);
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_printf(out,
                       "{\"version\":\"%s\",\"uptime_s\":%lld,\"entries\":%llu,"
                       "\"log_bytes\":%llu,\"dead_bytes\":%llu,\"reads\":%llu,"
                       "\"writes\":%llu,\"deletes\":%llu,\"misses\":%llu,"
                       "\"repl\":{\"role\":\"%s\"",
                       EXOMIND_VERSION, (long long)uptime,
                       (unsigned long long)entries,
                       (unsigned long long)log_bytes,
                       (unsigned long long)dead_bytes,
                       (unsigned long long)reads,
                       (unsigned long long)writes,
                       (unsigned long long)deletes,
                       (unsigned long long)misses,
                       g_repl.enabled ? "follower" : "primary");
            if (g_repl.enabled)
                buf_printf(out,
                           ",\"primary\":\"%s\",\"lag\":%llu,\"next\":%llu,"
                           "\"last_sync\":%lld,\"errors\":%llu,"
                           "\"resyncs\":%llu",
                           g_repl.primary,
                           (unsigned long long)g_repl.lag,
                           (unsigned long long)g_repl.next,
                           (long long)g_repl.last_sync,
                           (unsigned long long)g_repl.errors,
                           (unsigned long long)g_repl.resyncs);
            buf_puts(out, "}}");
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
            if (g_repl.enabled)
                buf_printf(out,
                           "repl: role=follower primary=%s lag=%llu "
                           "next=%llu last_sync=%lld errors=%llu "
                           "resyncs=%llu\n",
                           g_repl.primary, (unsigned long long)g_repl.lag,
                           (unsigned long long)g_repl.next,
                           (long long)g_repl.last_sync,
                           (unsigned long long)g_repl.errors,
                           (unsigned long long)g_repl.resyncs);
            else
                buf_puts(out, "repl: role=primary\n");
        }
        return;
    }

    if (!strcmp(path, "/get")) {
        int kr = qp_key(r->query, key, sizeof key);
        if (kr == 2) {
            *status = 400;
            buf_puts(out, "error: key too long");
            return;
        }
        if (!kr) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        if (!tok_allows_key(tok, key, strlen(key))) {
            *status = 403;
            buf_puts(out, "error: denied");
            return;
        }
        store_t *ts2 = pick_store(s, key, r->query);
        size_t vlen = 0;
        int64_t ts = 0;
        char *v = store_get(ts2, key, strlen(key), &vlen, &ts);
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

        int body_is_json = r->body_len > 0 && r->has_ct_json;
        if (!body_is_json && r->body_len > 0 && r->body[0] == '{' &&
            json_field(r->body, r->body_len, "key") != NULL)
            body_is_json = 1;
        if (body_is_json) {
            char *jk = json_field(r->body, r->body_len, "key");
            char *jv = json_field(r->body, r->body_len, "value");
            char *jt = json_field(r->body, r->body_len, "ttl");
            char *ja = json_field(r->body, r->body_len, "append");
            if (jk && strlen(jk) >= sizeof key) {
                *status = 400;
                buf_puts(out, "error: key too long");
                free(jk);
                free(jv);
                free(jt);
                free(ja);
                return;
            }
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
        } else if (r->body_len > 0 && is_form_body(r->body, r->body_len)) {
            char *b = xstrdup(r->body);
            char *save = NULL;
            for (char *pair = strtok_r(b, "&", &save); pair;
                 pair = strtok_r(NULL, "&", &save)) {
                char *eq = strchr(pair, '=');
                if (!eq)
                    continue;
                *eq = 0;
                url_decode(pair);
                size_t dlen = url_decode(eq + 1);
                if (!strcmp(pair, "key")) {
                    if (dlen >= sizeof key) {
                        *status = 400;
                        buf_puts(out, "error: key too long");
                        free(b);
                        free(v);
                        return;
                    }
                    memcpy(key, eq + 1, dlen);
                    key[dlen] = 0;
                    have_key = 1;
                } else if (!strcmp(pair, "value")) {
                    free(v);
                    v = xmalloc(dlen + 1);
                    memcpy(v, eq + 1, dlen);
                    v[dlen] = 0;
                    vlen = dlen;
                } else if (!strcmp(pair, "ttl")) {
                    ttl = strtol(eq + 1, NULL, 10);
                } else if (!strcmp(pair, "append")) {
                    append = (strtol(eq + 1, NULL, 10) != 0) ||
                             !strcmp(eq + 1, "true");
                }
            }
            free(b);
        } else {
            int kr = qp_key(r->query, key, sizeof key);
            if (kr == 2) {
                *status = 400;
                buf_puts(out, "error: key too long");
                free(v);
                return;
            }
            if (kr)
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
        if (!key[0]) {
            *status = 400;
            buf_puts(out, "error: empty key");
            free(v);
            return;
        }
        if (strlen(key) > MAX_KEY) {
            *status = 400;
            buf_puts(out, "error: key too long");
            free(v);
            return;
        }
        if (!tok_can_write(tok, key, strlen(key))) {
            *status = 403;
            buf_puts(out, "error: denied");
            free(v);
            return;
        }
        if (vlen > MAX_VAL) {
            *status = 413;
            buf_puts(out, "error: value too large");
            free(v);
            return;
        }
        store_t *ts3 = pick_store(s, key, r->query);
        if (store_set(ts3, key, strlen(key), v, vlen, ttl, append) != 0) {
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
        int kr = qp_key(r->query, key, sizeof key);
        if (kr == 2) {
            *status = 400;
            buf_puts(out, "error: key too long");
            return;
        }
        if (!kr) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        if (!key[0]) {
            *status = 400;
            buf_puts(out, "error: empty key");
            return;
        }
        if (!tok_can_write(tok, key, strlen(key))) {
            *status = 403;
            buf_puts(out, "error: denied");
            return;
        }
        store_t *ts4 = pick_store(s, key, r->query);
        if (store_set(ts4, key, strlen(key), r->body, r->body_len, 0, 1) != 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            return;
        }
        store_sync(ts4);
        buf_puts(out, "ok");
        return;
    }

    if (!strcmp(path, "/del")) {
        if (strcmp(r->method, "DELETE") && strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use DELETE");
            return;
        }
        int kr = qp_key(r->query, key, sizeof key);
        if (kr == 2) {
            *status = 400;
            buf_puts(out, "error: key too long");
            return;
        }
        if (!kr) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        if (!key[0]) {
            *status = 400;
            buf_puts(out, "error: empty key");
            return;
        }
        if (!tok_can_write(tok, key, strlen(key))) {
            *status = 403;
            buf_puts(out, "error: denied");
            return;
        }
        int existed = del_key_cascade(s, tok, key, strlen(key), r->query);
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

    if (!strcmp(path, "/embed")) {
        int kr = qp_key(r->query, key, sizeof key);
        if (kr == 2) {
            *status = 400;
            buf_puts(out, "error: key too long");
            return;
        }
        if (!kr) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        if (!key[0]) {
            *status = 400;
            buf_puts(out, "error: empty key");
            return;
        }
        if (strlen(key) + EXO_VEC_KEY_PREFIX_LEN > MAX_KEY) {
            *status = 400;
            buf_puts(out, "error: key too long");
            return;
        }
        char vkey[MAX_KEY + 1];
        size_t vklen = strlen(key) + EXO_VEC_KEY_PREFIX_LEN;
        memcpy(vkey, EXO_VEC_KEY_PREFIX, EXO_VEC_KEY_PREFIX_LEN);
        memcpy(vkey + EXO_VEC_KEY_PREFIX_LEN, key, strlen(key));

        if (!strcmp(r->method, "POST")) {
            int rc = http_embed(s, tok, key, strlen(key), r->body,
                                r->body_len, qp_int(r->query, "ttl", 0));
            if (rc == 0) {
                *status = 403;
                buf_puts(out, "error: denied");
            } else if (rc < 0) {
                *status = 500;
                buf_puts(out, "error: store failure");
            } else {
                store_sync(s);
                buf_printf(out, "ok %s %d", key, EXO_VEC_DIM);
            }
            return;
        }
        if (!strcmp(r->method, "DELETE")) {
            if (!tok_can_write(tok, vkey, vklen)) {
                *status = 403;
                buf_puts(out, "error: denied");
                return;
            }
            int existed = store_del(s, vkey, vklen);
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
        if (!tok_allows_key(tok, vkey, vklen)) {
            *status = 403;
            buf_puts(out, "error: denied");
            return;
        }
        size_t vlen = 0;
        char *v = store_get(s, vkey, vklen, &vlen, NULL);
        if (!v) {
            *status = 404;
            buf_puts(out, "missing");
            return;
        }
        uint8_t idx[EXO_VEC_DIM], val[EXO_VEC_DIM], nnz;
        if (vec_parse(v, vlen, idx, val, &nnz) != 0) {
            free(v);
            *status = 500;
            buf_puts(out, "error: bad vector");
            return;
        }
        free(v);
        buf_printf(out, "dim %d", EXO_VEC_DIM);
        for (uint8_t i = 0; i < nnz; i++)
            buf_printf(out, " %u:%u", (unsigned)idx[i], (unsigned)val[i]);
        return;
    }

    if (!strcmp(path, "/sim")) {
        if (strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use POST");
            return;
        }
        long k = qp_int(r->query, "k", 10);
        if (k <= 0 || k > 1000)
            k = 10;
        uint8_t idx[EXO_VEC_DIM], val[EXO_VEC_DIM], nnz;
        vec_embed(r->body, r->body_len, idx, val, &nnz);
        kv_t *kvs = NULL;
        size_t n = 0;
        store_vec_sim(s, idx, val, nnz, (int)k, &kvs, &n);
        filter_scope(kvs, &n, tok);
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "{\"results\":[");
            for (size_t i = 0; i < n; i++) {
                if (i)
                    buf_puts(out, ",");
                char *jk = json_escape(kvs[i].key + EXO_VEC_KEY_PREFIX_LEN,
                                       kvs[i].klen - EXO_VEC_KEY_PREFIX_LEN);
                buf_printf(out, "{\"key\":%s,\"sim\":%lld.%06lld}", jk,
                           (long long)(kvs[i].score / 1000000),
                           (long long)(kvs[i].score % 1000000));
                free(jk);
            }
            buf_puts(out, "]}");
        } else {
            for (size_t i = 0; i < n; i++)
                buf_printf(out, "%s\t%lld.%06lld\n",
                           kvs[i].key + EXO_VEC_KEY_PREFIX_LEN,
                           (long long)(kvs[i].score / 1000000),
                           (long long)(kvs[i].score % 1000000));
        }
        kv_free(kvs, n);
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
        store_t *ts6 = pick_store(s, has_prefix ? prefix : NULL, r->query);
        kv_t *kvs = NULL;
        size_t n = 0;
        store_query(ts6, Q_LIST, has_prefix ? prefix : NULL, NULL, desc,
                    &kvs, &n);
        filter_scope(kvs, &n, tok);
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
        kv_t *kvs = NULL, *kvs2 = NULL;
        size_t n = 0, n2 = 0;
        store_query(s, Q_SEARCH, NULL, q, 0, &kvs, &n);
        if (g_proj)
            store_query(g_proj, Q_SEARCH, NULL, q, 0, &kvs2, &n2);
        if (n2 > 0) {
            kvs = xrealloc(kvs, (n + n2) * sizeof(*kvs));
            memcpy(kvs + n, kvs2, n2 * sizeof(*kvs2));
            free(kvs2);
            n += n2;
        }
        filter_scope(kvs, &n, tok);
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
        if (!tok_can_write(tok, nkey, strlen(nkey))) {
            *status = 403;
            buf_puts(out, "error: denied");
            return;
        }
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
        filter_scope(kvs, &n, tok);
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
                        if (!tok_can_write(tok, k, strlen(k))) {
                            buf_printf(out, "set %s denied\n", k);
                        } else {
                            long ttl = nstr >= 4 ? strtol(strs[3], NULL, 10)
                                                 : 0;
                            int rc = store_set(s, k, strlen(k), strs[2],
                                               strlen(strs[2]), ttl, 0);
                            buf_printf(out, "set %s %s\n", k,
                                       rc == 0 ? "ok" : "error");
                        }
                    } else if (!strcmp(op, "append") && nstr >= 3) {
                        if (!tok_can_write(tok, k, strlen(k))) {
                            buf_printf(out, "append %s denied\n", k);
                        } else {
                            int rc = store_set(s, k, strlen(k), strs[2],
                                               strlen(strs[2]), 0, 1);
                            buf_printf(out, "append %s %s\n", k,
                                       rc == 0 ? "ok" : "error");
                        }
                    } else if (!strcmp(op, "get")) {
                        if (!tok_allows_key(tok, k, strlen(k))) {
                            buf_printf(out, "get %s denied\n", k);
                        } else {
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
                        }
                    } else if (!strcmp(op, "del")) {
                        if (!tok_can_write(tok, k, strlen(k))) {
                            buf_printf(out, "del %s denied\n", k);
                        } else {
                            int existed = del_key_cascade(s, tok, k,
                                                          strlen(k),
                                                          r->query);
                            buf_printf(out, "del %s %s\n", k,
                                       existed > 0 ? "ok" : "missing");
                        }
                    } else if (!strcmp(op, "embed") && nstr >= 3) {
                        int rc = http_embed(s, tok, k, strlen(k), strs[2],
                                            strlen(strs[2]),
                                            nstr >= 4 ? strtol(strs[3], NULL, 10) : 0);
                        if (rc == 1)
                            buf_printf(out, "embed %s ok %d\n", k, EXO_VEC_DIM);
                        else if (rc == 0)
                            buf_printf(out, "embed %s denied\n", k);
                        else
                            buf_printf(out, "embed %s error\n", k);
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
                char *op_emb = json_field(elem, elen, "embed");
                char *val = json_field(elem, elen, "value");
                char *ttls = json_field(elem, elen, "ttl");
                long ttl = ttls ? strtol(ttls, NULL, 10) : 0;
                if (op_set) {
                    if (!tok_can_write(tok, op_set, strlen(op_set))) {
                        buf_printf(out, "set %s denied\n", op_set);
                    } else {
                        int rc = store_set(s, op_set, strlen(op_set),
                                           val ? val : "",
                                           val ? strlen(val) : 0, ttl, 0);
                        buf_printf(out, "set %s %s\n", op_set,
                                   rc == 0 ? "ok" : "error");
                    }
                } else if (op_app) {
                    if (!tok_can_write(tok, op_app, strlen(op_app))) {
                        buf_printf(out, "append %s denied\n", op_app);
                    } else {
                        int rc = store_set(s, op_app, strlen(op_app),
                                           val ? val : "",
                                           val ? strlen(val) : 0, 0, 1);
                        buf_printf(out, "append %s %s\n", op_app,
                                   rc == 0 ? "ok" : "error");
                    }
                } else if (op_emb) {
                    int rc = http_embed(s, tok, op_emb, strlen(op_emb),
                                        val ? val : "",
                                        val ? strlen(val) : 0, ttl);
                    if (rc == 1)
                        buf_printf(out, "embed %s ok %d\n", op_emb, EXO_VEC_DIM);
                    else if (rc == 0)
                        buf_printf(out, "embed %s denied\n", op_emb);
                    else
                        buf_printf(out, "embed %s error\n", op_emb);
                } else if (op_get) {
                    if (!tok_allows_key(tok, op_get, strlen(op_get))) {
                        buf_printf(out, "get %s denied\n", op_get);
                    } else {
                        size_t vlen = 0;
                        char *v = store_get(s, op_get, strlen(op_get), &vlen,
                                            NULL);
                        if (v) {
                            char *e = escape_line(v, vlen);
                            buf_printf(out, "get %s %s\n", op_get, e);
                            free(e);
                            free(v);
                        } else {
                            buf_printf(out, "get %s missing\n", op_get);
                        }
                    }
                } else if (op_del) {
                    if (!tok_can_write(tok, op_del, strlen(op_del))) {
                        buf_printf(out, "del %s denied\n", op_del);
                    } else {
                        int existed = del_key_cascade(s, tok, op_del,
                                                      strlen(op_del),
                                                      r->query);
                        buf_printf(out, "del %s %s\n", op_del,
                                   existed > 0 ? "ok" : "missing");
                    }
                } else {
                    buf_puts(out, "error: bad batch element\n");
                }
                free(op_set);
                free(op_get);
                free(op_del);
                free(op_app);
                free(op_emb);
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

    if (!strcmp(path, "/snapshot")) {
        buf_puts(out, "exomind-snapshot-v1\n");
        snap_ctx_t ctx = {.out = out, .tok = tok};
        if (store_snapshot(s, snap_emit, &ctx) != 0) {
            *status = 500;
            out->len = 0;
            buf_puts(out, "error: store failure");
        }
        return;
    }

    if (!strcmp(path, "/restore")) {
        if (strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use POST");
            return;
        }
        if (tok && (tok->readonly || tok->prefix)) {
            *status = 403;
            buf_puts(out, "error: denied");
            return;
        }
        kv_t *kvs = NULL;
        size_t n = 0;
        if (parse_snapshot(r->body, r->body_len, &kvs, &n) != 0) {
            *status = 400;
            buf_puts(out, "error: bad snapshot");
            return;
        }
        int rc = store_restore(s, kvs, n);
        kv_free(kvs, n);
        if (rc < 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            return;
        }
        buf_printf(out, "ok %d", rc);
        return;
    }

    /* ---- project memory ---- */
    if (!strcmp(path, "/project")) {
        if (g_proj)
            buf_printf(out, "root\t%s\nfile\t%s\n", g_proj_root,
                       store_path(g_proj));
        else
            buf_puts(out, "no project store attached (no project root "
                          "detected; use --project-root)\n");
        return;
    }

    /* ---- backups ---- */
    if (!strcmp(path, "/backup")) {
        if (!g_backup_dir[0]) {
            *status = 400;
            buf_puts(out, "error: no backup dir (start with --backup <dir>)");
            return;
        }
        char berr[256];
        if (exo_backup_now(s, berr, sizeof berr) != 0) {
            *status = 500;
            buf_printf(out, "error: %s", berr);
            return;
        }
        buf_puts(out, "ok");
        return;
    }

    /* ---- memory associations: outdate / revive / link / assoc ---- */
    if (!strcmp(path, "/outdate")) {
        int kr = qp_key(r->query, key, sizeof key);
        if (kr != 1) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        char reason[512] = "";
        qp_str(r->query, "reason", reason, sizeof reason);
        char hk[MAX_KEY + 16];
        snprintf(hk, sizeof hk, "history:%s", key);
        char entry[1024];
        snprintf(entry, sizeof entry, "outdated %lld %s",
                 (long long)now_epoch(), reason[0] ? reason : "(no reason)");
        store_t *ts = pick_store(s, key, r->query);
        if (store_set(ts, hk, strlen(hk), entry, strlen(entry), 0, 1) != 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            return;
        }
        char mk[MAX_KEY + 16];
        snprintf(mk, sizeof mk, "outdated:%s", key);
        char mv[1024];
        snprintf(mv, sizeof mv, "%lld %s", (long long)now_epoch(),
                 reason[0] ? reason : "(no reason)");
        store_set(ts, mk, strlen(mk), mv, strlen(mv), 0, 0);
        store_sync(ts);
        buf_puts(out, "ok");
        return;
    }
    if (!strcmp(path, "/revive")) {
        int kr = qp_key(r->query, key, sizeof key);
        if (kr != 1) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        char mk[MAX_KEY + 16];
        snprintf(mk, sizeof mk, "outdated:%s", key);
        store_t *ts = pick_store(s, key, r->query);
        int existed = store_del(ts, mk, strlen(mk));
        store_sync(ts);
        buf_printf(out, "%s", existed > 0 ? "ok" : "not outdated");
        return;
    }
    if (!strcmp(path, "/outdated")) {
        int kr = qp_key(r->query, key, sizeof key);
        if (kr != 1) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        char mk[MAX_KEY + 16];
        snprintf(mk, sizeof mk, "outdated:%s", key);
        store_t *ts = pick_store(s, key, r->query);
        size_t vlen = 0;
        char *m = store_get(ts, mk, strlen(mk), &vlen, NULL);
        if (!m) {
            *status = 404;
            buf_puts(out, "not outdated");
            return;
        }
        buf_printf(out, "key\t%s\nmarker\t%s\n", key, m);
        /* the history of this key */
        char hk[MAX_KEY + 16];
        snprintf(hk, sizeof hk, "history:%s", key);
        char *h = store_get(ts, hk, strlen(hk), &vlen, NULL);
        if (h) {
            buf_puts(out, "history:\n");
            buf_put(out, h, vlen);
            free(h);
        }
        free(m);
        return;
    }
    if (!strcmp(path, "/link")) {
        char from[MAX_KEY], to[MAX_KEY];
        if (!qp_str(r->query, "from", from, sizeof from) ||
            !qp_str(r->query, "to", to, sizeof to) ||
            !from[0] || !to[0]) {
            *status = 400;
            buf_puts(out, "error: need from=<key> and to=<key>");
            return;
        }
        char rel[256] = "related";
        qp_str(r->query, "rel", rel, sizeof rel);
        char ak[MAX_KEY + 16];
        snprintf(ak, sizeof ak, "assoc:%s", from);
        char line[MAX_KEY + 256];
        snprintf(line, sizeof line, "%s\t%s", to, rel);
        store_t *ts = pick_store(s, from, r->query);
        if (store_set(ts, ak, strlen(ak), line, strlen(line), 0, 1) != 0) {
            *status = 500;
            buf_puts(out, "error: store failure");
            return;
        }
        store_sync(ts);
        buf_puts(out, "ok");
        return;
    }
    if (!strcmp(path, "/assoc")) {
        int kr = qp_key(r->query, key, sizeof key);
        if (kr != 1) {
            *status = 400;
            buf_puts(out, "error: missing key");
            return;
        }
        char ak[MAX_KEY + 16];
        snprintf(ak, sizeof ak, "assoc:%s", key);
        store_t *ts = pick_store(s, key, r->query);
        size_t vlen = 0;
        char *a = store_get(ts, ak, strlen(ak), &vlen, NULL);
        if (a) {
            buf_printf(out, "out\t%s\n", key);
            buf_put(out, a, vlen);
            free(a);
        }
        /* incoming links: scan assoc:* in both stores (bounded) */
        kv_t *kvs = NULL;
        size_t n = 0;
        store_query(ts, Q_LIST, "assoc:", NULL, 0, &kvs, &n);
        for (size_t i = 0; i < n; i++) {
            char *val = store_get(ts, kvs[i].key, kvs[i].klen, &vlen, NULL);
            if (!val)
                continue;
            char *hit = strstr(val, key);
            if (hit && (hit == val || hit[-1] == '\t' ||
                        hit[-1] == '\n')) {
                buf_printf(out, "in\t%s\t%.200s\n", kvs[i].key + 6, val);
            }
            free(val);
        }
        kv_free(kvs, n);
        return;
    }

    /* ---- recall: search + markers + history + associations ---- */
    if (!strcmp(path, "/recall")) {
        char q[4096];
        if (!qp_str(r->query, "q", q, sizeof q) || !q[0]) {
            *status = 400;
            buf_puts(out, "error: missing q");
            return;
        }
        long lim = qp_int(r->query, "limit", 10);
        if (lim < 0 || lim > 200)
            lim = 10;
        kv_t *kvs = NULL, *kvs2 = NULL;
        size_t n = 0, n2 = 0;
        store_query(s, Q_SEARCH, NULL, q, 0, &kvs, &n);
        if (g_proj)
            store_query(g_proj, Q_SEARCH, NULL, q, 0, &kvs2, &n2);
        if (n2 > 0) {
            kvs = xrealloc(kvs, (n + n2) * sizeof(*kvs));
            memcpy(kvs + n, kvs2, n2 * sizeof(*kvs2));
            free(kvs2);
            n += n2;
        }
        size_t m = (size_t)lim < n ? (size_t)lim : n;
        for (size_t i = 0; i < m; i++) {
            store_t *ts = pick_store(s, kvs[i].key, NULL);
            char *sn = snippet(kvs[i].val, kvs[i].vlen);
            buf_printf(out, "%s\t%s\n", kvs[i].key, sn);
            free(sn);
            char mk[MAX_KEY + 16];
            snprintf(mk, sizeof mk, "outdated:%s", kvs[i].key);
            size_t vlen = 0;
            char *mark = store_get(ts, mk, strlen(mk), &vlen, NULL);
            if (mark) {
                buf_printf(out, "  outdated: %s\n", mark);
                free(mark);
            }
            char hk[MAX_KEY + 16];
            snprintf(hk, sizeof hk, "history:%s", kvs[i].key);
            char *h = store_get(ts, hk, strlen(hk), &vlen, NULL);
            if (h) {
                char *h2 = xstrndup(h, vlen > 400 ? 400 : vlen);
                buf_printf(out, "  history: %s\n", h2);
                free(h2);
                free(h);
            }
            char ak[MAX_KEY + 16];
            snprintf(ak, sizeof ak, "assoc:%s", kvs[i].key);
            char *a = store_get(ts, ak, strlen(ak), &vlen, NULL);
            if (a) {
                char *a2 = xstrndup(a, vlen > 300 ? 300 : vlen);
                buf_printf(out, "  assoc: %s\n", a2);
                free(a2);
                free(a);
            }
        }
        kv_free(kvs, n);
        return;
    }

    /* ---- the mandate: what every agent must know ---- */
    if (!strcmp(path, "/mandate")) {
        if (!strcmp(r->method, "POST")) {
            if (!tok_can_write(tok, "mandate", 7)) {
                *status = 403;
                buf_puts(out, "error: denied");
                return;
            }
            http_set_mandate(r->body);
            store_set(s, "mandate", 7, r->body, r->body_len, 0, 0);
            store_sync(s);
            buf_puts(out, "ok");
            return;
        }
        char *m = NULL;
        size_t vlen = 0;
        if (g_mandate[0]) {
            buf_puts(out, g_mandate);
        } else {
            m = store_get(s, "mandate", 7, &vlen, NULL);
            if (m) {
                buf_put(out, m, vlen);
                free(m);
            }
        }
        buf_puts(out, "\n---\nacknowledge by writing: "
                      "set key agent:<your-id>:ready value 1\n");
        return;
    }

    goto unknown_path;
unknown_path:
    *status = 404;
    buf_puts(out, "error: unknown path");
}

void http_buf_free(buf_t *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* internal dispatch for the MCP bridge (no HTTP auth: the stdio channel
 * is local); path may carry the /exoexomind prefix which is stripped */
int http_dispatch(const char *method, const char *path, const char *query,
                  const char *body, size_t body_len, buf_t *out,
                  int *status, const char **ctype, store_t *s)
{
    req_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.method, sizeof r.method, "%s", method);
    snprintf(r.path, sizeof r.path, "%s", path);
    if (query)
        snprintf(r.query, sizeof r.query, "%s", query);
    r.tok_idx = -1;
    if (body && body_len > 0) {
        r.body = xmalloc(body_len + 1);
        memcpy(r.body, body, body_len);
        r.body[body_len] = 0;
        r.body_len = body_len;
    }
    route(&r, s, out, status, ctype);
    free(r.body);
    return 0;
}

void http_handle_conn(int fd, store_t *s)
{
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    req_t r;
    memset(&r, 0, sizeof r);
    r.tok_idx = -1;
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
    if (g_rate_limit_active && !exo_rate_take()) {
        send_response(fd, 429, "text/plain; charset=utf-8",
                      "error: rate limit exceeded\n", 26, 0);
        free(r.body);
        return;
    }
    /* modules are reachable at /exo<name> as well as at / */
    if (!strncmp(r.path, "/exoexomind", 11) &&
        (r.path[11] == 0 || r.path[11] == '/')) {
        memmove(r.path, r.path + 11, strlen(r.path + 11) + 1);
        if (!r.path[0])
            snprintf(r.path, sizeof r.path, "/");
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
