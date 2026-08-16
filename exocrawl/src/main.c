/*
 * exocrawl: AI-native web research daemon.
 *
 * Plain-text, token-efficient API: /search (private metasearch),
 * /fetch (HTML -> clean text), /scrape (concurrent batch), cache in
 * exomind. TLS via the curl binary. No cookies, no JS, no tracking.
 */
#include "exocrawl.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_HDRS (16 * 1024)
#define MAX_QUERY 4096

typedef struct {
    int port;
    const char *token;
    int concurrency;
    int pace_ms;
    int use_cache;
    const char *exomind;
    char *instances; /* optional searxng instance list file */
    char *uas_file;
} cfg_t;

static net_t g_net;
static cfg_t g_cfg;
static pool_t g_pool;
static size_t g_fetches, g_errors, g_cache_hits;
static uint64_t g_bytes;

/* ---------------- tiny HTTP server ---------------- */

static void send_all(int fd, const char *s, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
}

static void http_out(int fd, int status, const char *ct, const char *body)
{
    char hdr[512];
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n"
             "Access-Control-Allow-Origin: *\r\n\r\n",
             status, status == 200 ? "OK" : "Error", ct, strlen(body));
    send_all(fd, hdr, strlen(hdr));
    send_all(fd, body, strlen(body));
}

static int auth_ok(const char *auth)
{
    if (getenv("EXOCRAWL_DEBUG")) fprintf(stderr, "auth_ok: [%s] cfg=[%s]\n", auth, g_cfg.token ? g_cfg.token : "(null)");
    if (!g_cfg.token || !g_cfg.token[0])
        return 1;
    return auth && strncmp(auth, "Bearer ", 7) == 0 &&
           strcmp(auth + 7, g_cfg.token) == 0;
}

static char *query_param(const char *query, const char *name, char *out,
                         size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq)
            break;
        if ((size_t)(eq - p) == nlen && strncmp(p, name, nlen) == 0) {
            const char *v = eq + 1;
            const char *amp = strchr(v, '&');
            size_t vn = amp ? (size_t)(amp - v) : strlen(v);
            size_t w = 0;
            for (size_t i = 0; i < vn && w + 1 < cap; i++) {
                if (v[i] == '%' && i + 2 < vn) {
                    char hb[3] = {v[i + 1], v[i + 2], 0};
                    if (isxdigit((unsigned char)hb[0]) &&
                        isxdigit((unsigned char)hb[1])) {
                        out[w++] = (char)strtol(hb, NULL, 16);
                        i += 2;
                        continue;
                    }
                }
                if (v[i] == '+')
                    out[w++] = ' ';
                else
                    out[w++] = v[i];
            }
            out[w] = 0;
            return out;
        }
        const char *amp = strchr(p, '&');
        if (!amp)
            break;
        p = amp + 1;
    }
    return NULL;
}

/* ---------------- exomind cache ---------------- */

static int exomind_req(const char *method, const char *path, const char *body,
                       char **out, size_t *outlen)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(7654);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[8192];
    snprintf(req, sizeof req,
             "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
             "Content-Length: %zu\r\n\r\n%s",
             method, path, body ? strlen(body) : 0, body ? body : "");
    send_all(fd, req, strlen(req));
    char buf[65536];
    size_t n = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + n, sizeof buf - n - 1, 0);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n + 1 >= sizeof buf)
            break;
    }
    close(fd);
    buf[n] = 0;
    if (n == 0)
        return -1;
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end)
        return -1;
    *out = strdup(hdr_end + 4);
    *outlen = n - (size_t)(hdr_end + 4 - buf);
    return 0;
}

static uint64_t fnv64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static char *cache_get(const char *url)
{
    if (!g_cfg.use_cache)
        return NULL;
    char path[1024];
    snprintf(path, sizeof path, "/get?key=exocrawl:cache:%llx",
             (unsigned long long)fnv64(url));
    char *out = NULL;
    size_t n = 0;
    if (exomind_req("GET", path, NULL, &out, &n) != 0)
        return NULL;
    if (!out || strncmp(out, "missing", 7) == 0) {
        free(out);
        return NULL;
    }
    g_cache_hits++;
    return out;
}

static void cache_put(const char *url, const char *text)
{
    if (!g_cfg.use_cache)
        return;
    char path[4096];
    char *enc = url_encode(url);
    snprintf(path, sizeof path, "/set?key=exocrawl:cache:%llx&ttl=86400",
             (unsigned long long)fnv64(url));
    (void)enc;
    char *out = NULL;
    size_t n = 0;
    exomind_req("POST", path, text, &out, &n);
    free(out);
}

/* ---------------- fetch pipeline ---------------- */

typedef struct {
    const char *url;
    size_t max;
    int links, images;
    char *result; /* malloc'd text */
    int status;   /* 0 ok, -1 error */
    char err[256];
} fetch_job_t;

static void fetch_worker(fetch_job_t *j)
{
    char host[256];
    url_host(j->url, host, sizeof host);
    pace_wait(host, g_cfg.pace_ms);

    char *cached = cache_get(j->url);
    if (cached) {
        j->result = cached;
        j->status = 0;
        return;
    }
    resp_t r;
    char err[256];
    if (net_fetch(&g_net, j->url, &r, err, sizeof err) != 0) {
        snprintf(j->err, sizeof j->err, "%s", err);
        j->status = -1;
        g_errors++;
        return;
    }
    if (r.status != 200 || !r.body) {
        snprintf(j->err, sizeof j->err, "http %d", r.status);
        j->status = -1;
        g_errors++;
        resp_free(&r);
        return;
    }
    __sync_fetch_and_add(&g_fetches, 1);
    __sync_fetch_and_add(&g_bytes, r.blen);
    page_t p;
    page_extract(r.body, r.blen, j->url, &p, j->max);
    buf_t out;
    buf_init(&out, j->max + 4096);
    if (p.title && p.title[0])
        buf_printf(&out, "# %s\n\n", p.title);
    if (p.text)
        buf_puts(&out, p.text);
    if (j->links && p.nlinks > 0) {
        buf_puts(&out, "\n## links\n");
        for (size_t i = 0; i < p.nlinks; i++)
            buf_printf(&out, "- %s\n", p.links[i]);
    }
    if (j->images && p.nimages > 0) {
        buf_puts(&out, "\n## images\n");
        for (size_t i = 0; i < p.nimages; i++)
            buf_printf(&out, "- %s\n", p.images[i]);
    }
    /* the max cap covers the whole output, title included */
    if (out.len > j->max)
        out.len = j->max;
    if (out.p)
        out.p[out.len] = 0;
    j->result = out.p;
    page_free(&p);
    resp_free(&r);
    if (g_cfg.use_cache)
        cache_put(j->url, j->result);
}

/* ---------------- request handling ---------------- */

static void handle_fetch(int fd, const char *query, const char *body)
{
    (void)body;
    char url[MAX_URL];
    char mx[32];
    if (!query_param(query, "url", url, sizeof url)) {
        http_out(fd, 400, "text/plain", "error: missing url\n");
        return;
    }
    if (!url_is_http(url)) {
        http_out(fd, 400, "text/plain", "error: url must be http(s)\n");
        return;
    }
    size_t max = 8000;
    if (query_param(query, "max", mx, sizeof mx))
        max = strtoul(mx, NULL, 10);
    if (max == 0 || max > MAX_EXTRACT)
        max = MAX_EXTRACT;
    char lb[8], ib[8];
    int links = 0, images = 0;
    if (query_param(query, "links", lb, sizeof lb) && lb[0] == '1')
        links = 1;
    if (query_param(query, "images", ib, sizeof ib) && ib[0] == '1')
        images = 1;

    fetch_job_t j;
    memset(&j, 0, sizeof j);
    j.url = url;
    j.max = max;
    j.links = links;
    j.images = images;
    fetch_worker(&j);
    if (j.status != 0) {
        char out[512];
        snprintf(out, sizeof out, "error: %s\n", j.err);
        http_out(fd, 502, "text/plain", out);
        return;
    }
    http_out(fd, 200, "text/plain; charset=utf-8", j.result ? j.result : "");
    free(j.result);
}

static void handle_search(int fd, const char *query)
{
    char q[MAX_QUERY];
    if (!query_param(query, "q", q, sizeof q) || !q[0]) {
        http_out(fd, 400, "text/plain", "error: missing q\n");
        return;
    }
    char nbuf[16], ebuf[64], jbuf[8];
    int n = 10;
    if (query_param(query, "n", nbuf, sizeof nbuf))
        n = atoi(nbuf);
    if (n < 1 || n > 50)
        n = 10;
    const char *engines = query_param(query, "engines", ebuf, sizeof ebuf)
                              ? ebuf
                              : NULL;
    char err[256];
    result_t *res = NULL;
    size_t nres = 0;
    if (search_run(&g_net, q, engines, n, &res, &nres, err, sizeof err) !=
        0) {
        char out[512];
        snprintf(out, sizeof out, "error: %s\n", err);
        http_out(fd, 502, "text/plain", out);
        return;
    }
    int use_json = query_param(query, "json", jbuf, sizeof jbuf) && jbuf[0] == '1';
    buf_t out;
    buf_init(&out, 8192);
    if (use_json) {
        buf_puts(&out, "[");
        for (size_t i = 0; i < nres; i++) {
            buf_printf(&out, "%s{\"title\":\"%s\",\"url\":\"%s\",\"snippet\":\"%s\"}",
                       i ? "," : "", res[i].title ? res[i].title : "",
                       res[i].url ? res[i].url : "",
                       res[i].snippet ? res[i].snippet : "");
        }
        buf_puts(&out, "]\n");
    } else {
        for (size_t i = 0; i < nres; i++)
            buf_printf(&out, "%zu\t%s\t%s\t%s\n", i + 1,
                       res[i].title ? res[i].title : "",
                       res[i].url ? res[i].url : "",
                       res[i].snippet ? res[i].snippet : "");
    }
    results_free(res, nres);
    http_out(fd, 200, use_json ? "application/json" : "text/plain; charset=utf-8",
             out.p ? out.p : "");
    buf_free(&out);
}

typedef struct {
    fetch_job_t **jobs;
    size_t n;
} scrape_ctx_t;

static void scrape_one(job_t *jb)
{
    fetch_worker(jb->arg);
}

static void handle_scrape(int fd, const char *query, const char *body)
{
    (void)query;
    if (!body || !body[0]) {
        http_out(fd, 400, "text/plain", "error: empty body\n");
        return;
    }
    size_t njobs = 0;
    fetch_job_t *jobs = NULL;
    const char *p = body;
    while (*p && njobs < 64) {
        while (*p == '\n')
            p++;
        if (!*p)
            break;
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[8192];
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        /* url [TAB max] */
        char *tab = strchr(line, '\t');
        size_t max = 8000;
        if (tab) {
            *tab = 0;
            max = strtoul(tab + 1, NULL, 10);
        }
        if (url_is_http(line)) {
            jobs = realloc(jobs, (njobs + 1) * sizeof(fetch_job_t));
            if (!jobs)
                break;
            memset(&jobs[njobs], 0, sizeof jobs[njobs]);
            jobs[njobs].url = strdup(line);
            jobs[njobs].max = max;
            njobs++;
        }
        p = eol ? eol + 1 : p + len;
    }
    if (njobs == 0) {
        http_out(fd, 400, "text/plain", "error: no valid urls\n");
        free(jobs);
        return;
    }
    for (size_t i = 0; i < njobs; i++)
        pool_submit(&g_pool, scrape_one, &jobs[i]);
    pool_wait(&g_pool);
    buf_t out;
    buf_init(&out, 8192);
    buf_printf(&out, "ok %zu\n", njobs);
    for (size_t i = 0; i < njobs; i++) {
        fetch_job_t *j = &jobs[i];
        if (j->status == 0)
            buf_printf(&out, "fetch %s %zu ok\n", j->url, strlen(j->result));
        else
            buf_printf(&out, "fetch %s error: %s\n", j->url, j->err);
        free((void *)j->url);
        free(j->result);
    }
    free(jobs);
    http_out(fd, 200, "text/plain", out.p ? out.p : "");
    buf_free(&out);
}

static void handle_stats(int fd)
{
    char out[512];
    snprintf(out, sizeof out,
             "fetches: %zu\nerrors: %zu\ncache_hits: %zu\nbytes: %llu\n",
             g_fetches, g_errors, g_cache_hits,
             (unsigned long long)g_bytes);
    http_out(fd, 200, "text/plain", out);
}

static void handle_spec(int fd)
{
    http_out(fd, 200, "text/plain; charset=utf-8",
             "exocrawl v0.1.0 - AI-native web research daemon\n"
             "plain text, lowercase ok/error replies, token-efficient\n"
             "GET / - this specification\n\n"
             "GET /search?q=<query>[&n=10][&engines=ddg,mojeek,marginalia,bing,wikipedia|all][&json=1]\n"
             "  independent private metasearch (own adapters, no third-party\n"
             "  aggregator; ads filtered, rotated, no accounts/cookies)\n"
             "GET /fetch?url=<http(s) url>[&max=8000][&links=1][&images=1]\n"
             "  HTML -> clean plain text (boilerplate removed, headings as\n"
             "  ##, links/images appended as sections when requested)\n"
             "POST /scrape  body: one url per line [TAB max] - concurrent\n"
             "  fetch-all with per-host pacing and identity rotation\n"
             "GET /stats - counters\n"
             "GET /ping - pong\n\n"
             "privacy: stateless, no cookies, no JS, no tracking params;\n"
             "optional exomind cache (keys exocrawl:cache:*).\n");
}

static void route(int fd, const char *method, const char *path,
                  const char *query, const char *body, const char *auth)
{
    if (!auth_ok(auth)) {
        http_out(fd, 401, "text/plain", "error: unauthorized\n");
        return;
    }
    if (strcmp(path, "/") == 0)
        handle_spec(fd);
    else if (strcmp(path, "/ping") == 0)
        http_out(fd, 200, "text/plain", "pong\n");
    else if (strcmp(path, "/search") == 0)
        handle_search(fd, query);
    else if (strcmp(path, "/fetch") == 0)
        handle_fetch(fd, query, body);
    else if (strcmp(path, "/scrape") == 0 && strcmp(method, "POST") == 0)
        handle_scrape(fd, query, body);
    else if (strcmp(path, "/stats") == 0)
        handle_stats(fd);
    else
        http_out(fd, 404, "text/plain", "error: unknown path\n");
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[65536];
    size_t n = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + n, sizeof buf - n - 1, 0);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n + 1 >= sizeof buf || strstr(buf, "\r\n\r\n") ||
            (n > 4 && strstr(buf, "\n\n")))
            break;
    }
    buf[n] = 0;
    char method[16] = {0}, path[2048] = {0}, query[8192] = {0}, auth[512] = {0};
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) {
        hdr_end = strstr(buf, "\n\n");
        if (hdr_end)
            hdr_end += 1;
    }
    if (hdr_end) {
        *hdr_end = 0;
    }
    char *first = buf;
    sscanf(first, "%15s %2047s", method, path);
    char *q = strchr(path, '?');
    if (q) {
        snprintf(query, sizeof query, "%s", q + 1);
        *q = 0;
    }
    const char *p = buf;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        if (!eol) {
            /* last header line: bounded by the truncating NUL */
            if (strncasecmp(p, "authorization:", 14) == 0) {
                const char *v = p + 14;
                while (*v == ' ')
                    v++;
                snprintf(auth, sizeof auth, "%s", v);
                size_t al = strlen(auth);
                while (al && (auth[al - 1] == '\r' || auth[al - 1] == '\n' ||
                              auth[al - 1] == ' ' || auth[al - 1] == '\t'))
                    auth[--al] = 0;
            }
            break;
        }
        *eol = 0;
        if (strncasecmp(p, "authorization:", 14) == 0) {
            const char *v = p + 14;
            while (*v == ' ')
                v++;
            snprintf(auth, sizeof auth, "%s", v);
            size_t al = strlen(auth);
            while (al && (auth[al - 1] == '\r' || auth[al - 1] == '\n' ||
                          auth[al - 1] == ' ' || auth[al - 1] == '\t'))
                auth[--al] = 0;
        }
        p = eol + 1;
    }
    const char *body = hdr_end ? hdr_end + 4 : "";
    if (body > buf + n)
        body = "";
    route(fd, method, path, query, body, auth);
    close(fd);
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--port 7658] [--token secret] [--concurrency 16]\n"
            "       [--pace-ms 200] [--cache exomind] [--proxy http://...]\n",
            prog);
}

int main(int argc, char **argv)
{
    g_cfg.port = 7658;
    g_cfg.concurrency = 16;
    g_cfg.pace_ms = 200;
    net_init(&g_net);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            g_cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--token") && i + 1 < argc)
            g_cfg.token = argv[++i];
        else if (!strcmp(argv[i], "--concurrency") && i + 1 < argc)
            g_cfg.concurrency = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pace-ms") && i + 1 < argc)
            g_cfg.pace_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
            g_cfg.use_cache = 1, g_cfg.exomind = argv[++i];
        else if (!strcmp(argv[i], "--proxy") && i + 1 < argc)
            g_net.proxy = argv[++i];
        else if (!strcmp(argv[i], "--engine-base") && i + 1 < argc)
            g_net.engine_base = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "exocrawl: unknown argument %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (pool_init(&g_pool, g_cfg.concurrency) != 0) {
        fprintf(stderr, "exocrawl: pool init failed\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "exocrawl: socket: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_cfg.port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "exocrawl: bind %d: %s\n", g_cfg.port,
                strerror(errno));
        return 1;
    }
    listen(srv, 64);
    fprintf(stderr, "exocrawl v%s on %d (concurrency %d, pace %d ms%s)\n",
            EXO_VERSION, g_cfg.port, g_cfg.concurrency, g_cfg.pace_ms,
            g_cfg.use_cache ? ", cache exomind" : "");
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        pthread_t t;
        pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
    return 0;
}
