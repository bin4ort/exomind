/*
 * exocontext — context continuity for AI agents.
 *
 * A tiny daemon that compresses an agent's durable state into a bounded,
 * recency-ranked digest: everything under `agent:<id>:*` plus the notes
 * mentioning the agent, capped at a character budget. An agent that
 * restarts (or spawns a fresh context window) can reconstruct its
 * working state from a single GET /context?agent=<id>.
 *
 * Backend: exomind (durable memory). Zero compile dependencies, C11.
 */
#include "exocontext.h"
#include "../../common/exo.h"

#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_AGENT 256
#define MAX_BODY (64u * 1024u)

typedef struct {
    char **p;
    size_t len, cap;
} strlist_t;

/* seen-set to avoid duplicate keys across the notes and keys sections */
static int seen_before(char **seen, size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(seen[i], key) == 0)
            return 1;
    return 0;
}

/* Compose the context digest. Emits into out (plain text):
 *   # context for <agent> (budget <n> chars)
 *   ## summary (compressed history)        <- only after auto-compression
 *   <raw summary lines, re-expanded>
 *   ## notes (newest first)
 *   <raw note line>
 *   ## state
 *   key\tvalue
 * Stops adding once the budget is consumed (a crossing line is kept).
 */
void ctx_build(exo_t *e, const char *agent, size_t budget, buf_t *out,
               char *err, size_t errsz)
{
    buf_printf(out, "# context for %s (budget %zu chars)\n", agent, budget);

    char **seen = NULL;
    size_t nseen = 0;

    /* 1. agent state keys (namespace `agent:<id>:*`), values via batch.
     * The compressed-history summary, when present, is re-expanded
     * first so a resuming agent sees the summarized decisions/state
     * before the live tail. */
    char pf[512];
    snprintf(pf, sizeof pf, "agent:%s:", agent);
    char **keys = NULL;
    size_t nk = 0;
    if (exo_list(e, pf, &keys, &nk, err, errsz) == 0 && nk > 0) {
        char **vals = NULL;
        if (exo_batch_get(e, keys, nk, &vals, err, errsz) != 0) {
            free(keys);
            return;
        }
        size_t plen = strlen(pf);
        size_t sidx = nk;
        for (size_t i = 0; i < nk; i++)
            if (strcmp(keys[i] + plen, CTX_SUMMARY_SUFFIX + 1) == 0) {
                sidx = i;
                break;
            }
        if (sidx < nk) {
            seen = xrealloc(seen, (nseen + 1) * sizeof(*seen));
            seen[nseen++] = xstrdup(keys[sidx]);
        }
        if (sidx < nk && vals[sidx] && vals[sidx][0]) {
            buf_puts(out, "\n## summary (compressed history)\n");
            char *save = NULL;
            for (char *l = strtok_r(vals[sidx], "\n", &save); l;
                 l = strtok_r(NULL, "\n", &save)) {
                if (out->len >= budget)
                    break;
                buf_printf(out, "%s\n", l);
            }
        }
        buf_puts(out, "\n## state (agent:<id>:* keys)\n");
        for (size_t i = 0; i < nk && out->len < budget; i++) {
            if (i == sidx || !vals[i])
                continue;
            if (seen_before(seen, nseen, keys[i]))
                continue;
            seen = xrealloc(seen, (nseen + 1) * sizeof(*seen));
            seen[nseen++] = xstrdup(keys[i]);
            char *v = vals[i];
            char *nl = strchr(v, '\n');
            size_t vl = nl ? (size_t)(nl - v) : strlen(v);
            if (vl > 400)
                vl = 400; /* per-key cap inside the digest */
            buf_printf(out, "%s\t%.*s\n", keys[i], (int)vl, v);
            free(v);
        }
        free(vals);
        free(keys);
    }

    /* 2. notes mentioning the agent, newest first (exomind orders desc) */
    char q[1024];
    snprintf(q, sizeof q, "/notes?limit=200&q=agent%%3A%s", agent);
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    if (exo_request(e, "GET", q, NULL, 0, 0, &resp, &rlen, &status, err,
                    errsz) == 0 && status == 200 && resp) {
        buf_puts(out, "\n## notes (newest first)\n");
        char *save = NULL;
        for (char *l = strtok_r(resp, "\n", &save); l;
             l = strtok_r(NULL, "\n", &save)) {
            if (out->len >= budget)
                break;
            if (seen_before(seen, nseen, l))
                continue;
            seen = xrealloc(seen, (nseen + 1) * sizeof(*seen));
            seen[nseen++] = xstrdup(l);
            buf_printf(out, "%s\n", l);
        }
    }
    for (size_t i = 0; i < nseen; i++)
        free(seen[i]);
    free(seen);
    free(resp);
}

/* ---------------- session auto-compression ---------------- */

/* storage byte budget for one session's log: EXO_CTX_BUDGET env,
 * default 16 KiB. Measured as the sum of `strlen(key)+strlen(value)+2`
 * over the live `agent:<id>:*` keys (the summary key excluded). */
static size_t ctx_storage_budget(void)
{
    const char *v = getenv("EXO_CTX_BUDGET");
    if (!v || !*v)
        return CTX_BUDGET_DEFAULT;
    long n = atol(v);
    return n > 0 ? (size_t)n : CTX_BUDGET_DEFAULT;
}

static void strlist_add(strlist_t *sl, const char *s)
{
    for (size_t i = 0; i < sl->len; i++)
        if (strcmp(sl->p[i], s) == 0)
            return;
    sl->p = xrealloc(sl->p, (sl->len + 1) * sizeof(*sl->p));
    sl->p[sl->len++] = xstrdup(s);
}

/* carry the terse decisions/state lines of a folded value forward:
 * keep only `decided:` / `state:` prefixed lines (case-insensitive) */
static void summary_state_lines(const char *val, strlist_t *lines)
{
    char *copy = xstrdup(val);
    char *save = NULL;
    for (char *l = strtok_r(copy, "\n", &save); l;
         l = strtok_r(NULL, "\n", &save)) {
        size_t n = strlen(l);
        while (n > 0 && (l[n - 1] == '\r' || l[n - 1] == '\n'))
            l[--n] = 0;
        if (ci_prefix(l, "decided:") || ci_prefix(l, "state:"))
            strlist_add(lines, l);
    }
    free(copy);
}

/* cumulative entry count from an existing summary header:
 * `# summary <n> entries (compressed at <epoch>)` */
static size_t summary_count_of(const char *val)
{
    size_t n = 0;
    if (val && sscanf(val, "# summary %zu entries", &n) == 1)
        return n;
    return 0;
}

/* the state lines already carried in an existing summary (skips the
 * `# summary` header and the `(+N entries compressed)` footer) */
static void summary_lines_of(const char *val, strlist_t *lines)
{
    if (!val)
        return;
    char *copy = xstrdup(val);
    char *save = NULL;
    for (char *l = strtok_r(copy, "\n", &save); l;
         l = strtok_r(NULL, "\n", &save)) {
        if (strncmp(l, "# summary", 9) == 0)
            continue;
        if (l[0] == '(' && l[1] == '+')
            continue;
        strlist_add(lines, l);
    }
    free(copy);
}

/* Auto-compress a long session: when the stored `agent:<id>:*` log
 * exceeds the byte budget, fold the oldest entries into a line-based
 * summary under `agent:<id>:summary` (decisions/state lines carried
 * forward, cumulative folded count + compression timestamp recorded)
 * and delete them; `ctx:summary:<id>` records the last compression.
 * Best-effort: backend failures are logged, the live keys are left in
 * place, and the caller op still succeeds. Returns the number of
 * entries folded (0 = session fits the budget, nothing to fold). */
int ctx_compress(exo_t *e, const char *agent, char *err, size_t errsz)
{
    size_t budget = ctx_storage_budget();
    char pf[512];
    snprintf(pf, sizeof pf, "agent:%s:", agent);
    char **keys = NULL;
    size_t nk = 0;
    if (exo_list(e, pf, &keys, &nk, err, errsz) != 0) {
        free(keys);
        return 0;
    }
    if (nk == 0) {
        free(keys);
        return 0;
    }
    char **vals = NULL;
    if (exo_batch_get(e, keys, nk, &vals, err, errsz) != 0) {
        for (size_t i = 0; i < nk; i++)
            free(keys[i]);
        free(keys);
        return 0;
    }
    size_t plen = strlen(pf);
    size_t sidx = nk;
    for (size_t i = 0; i < nk; i++)
        if (strcmp(keys[i] + plen, CTX_SUMMARY_SUFFIX + 1) == 0) {
            sidx = i;
            break;
        }
    size_t live = nk - (sidx != nk);
    size_t total = 0;
    for (size_t i = 0; i < nk; i++) {
        if (i == sidx)
            continue;
        total += strlen(keys[i]) + strlen(vals[i]) + 2;
    }
    if (live <= 1 || total <= budget) {
        for (size_t i = 0; i < nk; i++) {
            free(keys[i]);
            free(vals[i]);
        }
        free(keys);
        free(vals);
        return 0;
    }

    /* fold from the oldest entries (the log lists keys asc) until the
     * live tail fits the budget; the newest entry is never folded */
    strlist_t lines = {0};
    size_t folded = 0, folded_bytes = 0;
    for (size_t i = 0; i < nk && folded < live - 1 &&
                        total - folded_bytes > budget; i++) {
        if (i == sidx)
            continue;
        summary_state_lines(vals[i], &lines);
        folded++;
        folded_bytes += strlen(keys[i]) + strlen(vals[i]) + 2;
    }
    if (folded == 0) {
        for (size_t i = 0; i < nk; i++) {
            free(keys[i]);
            free(vals[i]);
        }
        free(keys);
        free(vals);
        return 0;
    }

    /* merge into any existing summary: header count accumulates,
     * state lines dedupe, footer records this round's increment */
    strlist_t old = {0};
    size_t oldcount = 0;
    if (sidx != nk) {
        oldcount = summary_count_of(vals[sidx]);
        summary_lines_of(vals[sidx], &old);
    }
    buf_t sb = {0};
    buf_printf(&sb, "# summary %zu entries (compressed at %lld)\n",
               oldcount + folded, (long long)now_epoch());
    size_t kept = 0;
    for (size_t i = 0; i < lines.len && kept < CTX_SUMMARY_MAX_LINES; i++) {
        buf_printf(&sb, "%s\n", lines.p[i]);
        kept++;
    }
    for (size_t i = 0; i < old.len && kept < CTX_SUMMARY_MAX_LINES; i++) {
        buf_printf(&sb, "%s\n", old.p[i]);
        kept++;
    }
    buf_printf(&sb, "(+%zu entries compressed)\n", folded);

    char sk[512];
    snprintf(sk, sizeof sk, "agent:%s" CTX_SUMMARY_SUFFIX, agent);
    if (exo_persist(e, sk, sb.p, 0, err, errsz) != 0) {
        exo_log(EXO_LOG_WARN, "exocontext: summary write failed for %s",
                agent);
        goto out;
    }
    char mk[512];
    snprintf(mk, sizeof mk, "ctx:summary:%s", agent);
    char ts[32];
    snprintf(ts, sizeof ts, "%lld", (long long)now_epoch());
    if (exo_persist(e, mk, ts, 0, err, errsz) != 0)
        exo_log(EXO_LOG_WARN,
                "exocontext: summary marker write failed for %s", agent);
    size_t del_left = folded;
    for (size_t i = 0; i < nk && del_left > 0; i++) {
        if (i == sidx)
            continue;
        if (exo_del(e, keys[i], NULL, err, errsz) != 0)
            exo_log(EXO_LOG_WARN, "exocontext: del %s failed", keys[i]);
        del_left--;
    }
out:
    buf_free(&sb);
    for (size_t i = 0; i < lines.len; i++)
        free(lines.p[i]);
    free(lines.p);
    for (size_t i = 0; i < old.len; i++)
        free(old.p[i]);
    free(old.p);
    for (size_t i = 0; i < nk; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
    return (int)folded;
}

/* ---------------- minimal HTTP layer ---------------- */

static const char *g_token = NULL;

/* global exomind handle, wired in main() */
exo_t *g_exo_ctx = NULL;
int g_rate_limit_active = 0;

/* the self-describing spec: GET / in server mode, and the no-arg guide
 * in console mode. */
static const char *spec_text(void)
{
    return "exocontext v" EXOCONTEXT_VERSION " - context continuity for AI agents\n"
           "plain text, lowercase ok/error replies, token-efficient\n\n"
           "GET / - this specification\n"
           "GET /ping - liveness: pong\n"
           "GET /context?agent=<id>[&budget=<chars>] - bounded digest of an\n"
           "   agent's state: notes mentioning `agent:<id>` (newest first)\n"
           "   plus every `agent:<id>:*` key with its value, capped at the\n"
           "   budget (default 4000 chars). An agent can reconstruct its\n"
           "   working context after a restart from this single call.\n"
           "POST /context - same, body `agent=<id>[&budget=<chars>]`\n"
           "Auto-compress: when a session's stored log (`agent:<id>:*` keys)\n"
           "   exceeds EXO_CTX_BUDGET bytes (default 16384), the oldest\n"
           "   entries are folded into a summary under `agent:<id>:summary`\n"
           "   (decided:/state: lines carried forward, folded count +\n"
           "   timestamp recorded) and the digest re-expands it before the\n"
           "   live tail; `ctx:summary:<id>` records the last compression.\n"
           "Add `json=1` for a JSON array.\n"
           "console: exocontext /context?agent=<id>[&budget=<chars>]\n"
           "  one-shot, in-process, no port bound (body on --body or stdin);\n"
           "  no args prints this guide\n"
           "server: only with --serve (or --port <n>)\n"
           "Backend: exomind (durable memory). Zero compile deps, C11.\n";
}

void http_set_token(const char *tok)
{
    g_token = tok;
}

static void http_out(int fd, int status, const char *ctype, const char *body)
{
    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     status,
                     status == 200 ? "OK" :
                     status == 401 ? "Unauthorized" :
                     status == 400 ? "Bad Request" :
                     status == 405 ? "Method Not Allowed" :
                     status == 503 ? "Service Unavailable" : "Error",
                     ctype, strlen(body));
    (void)!n;
    write(fd, head, (size_t)n);
    write(fd, body, strlen(body));
}

static int query_param(const char *q, const char *name, char *out, size_t cap)
{
    const char *p = q;
    size_t nl = strlen(name);
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg >= nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
            size_t v = seg - nl - 1;
            if (v >= cap)
                v = cap - 1;
            memcpy(out, p + nl + 1, v);
            out[v] = 0;
            for (size_t i = 0; i < v; i++)
                if (out[i] == '+')
                    out[i] = ' ';
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

/* in-process dispatch: the same routing server mode and console mode
 * share. method/path/query/body come from the request line, or from
 * args/stdin in console mode. No auth here - that is server-layer. */
static void dispatch(const char *method, char *path, const char *qs,
                     const char *body, buf_t *out, int *status,
                     const char **ctype)
{
    *status = 200;
    *ctype = "text/plain";
    if (!strncmp(path, "/exoexocontext", 14) &&
        (path[14] == 0 || path[14] == '/')) {
        memmove(path, path + 14, strlen(path + 14) + 1);
        if (!path[0])
            strcpy(path, "/");
    }
    if (!strcmp(path, "/ping")) {
        if (strcmp(method, "GET") && strcmp(method, "HEAD"))
            *status = 405, buf_puts(out, "error: use GET\n");
        else
            buf_puts(out, "pong\n");
        return;
    }
    if (!strcmp(path, "/")) {
        buf_puts(out, spec_text());
        return;
    }
    if (!strcmp(path, "/context")) {
        char agent[MAX_AGENT] = "";
        char bbuf[16];
        size_t budget = 4000;
        if (qs) {
            (void)query_param(qs, "agent", agent, sizeof agent);
            if (query_param(qs, "budget", bbuf, sizeof bbuf))
                budget = (size_t)atol(bbuf);
        }
        if (!agent[0] && body && body[0]) {
            (void)query_param(body, "agent", agent, sizeof agent);
            if (query_param(body, "budget", bbuf, sizeof bbuf))
                budget = (size_t)atol(bbuf);
        }
        if (!agent[0]) {
            *status = 400;
            buf_puts(out, "error: missing agent\n");
            return;
        }
        if (budget < 256)
            budget = 256;
        if (budget > MAX_CONTEXT_BUDGET)
            budget = MAX_CONTEXT_BUDGET;
        char err[256];
        (void)ctx_compress(g_exo_ctx, agent, err, sizeof err);
        ctx_build(g_exo_ctx, agent, budget, out, err, sizeof err);
        return;
    }
    *status = 400;
    buf_puts(out, "error: unknown path\n");
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[65536];
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, buf + got, sizeof buf - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        buf[got] = 0;
        /* headers end? if so, read up to Content-Length body bytes */
        char *he = strstr(buf, "\r\n\r\n");
        if (he) {
            long cl = 0;
            const char *clp = strstr(buf, "Content-Length:");
            if (clp)
                cl = atol(clp + 15);
            size_t have = got - ((size_t)(he + 4 - buf));
            if (have >= (size_t)cl)
                break;
        }
        if (got >= sizeof buf - 1)
            break;
    }
    if (got == 0) {
        close(fd);
        return NULL;
    }
    /* parse the request line from a copy: strtok would clobber the buffer
     * that the auth/body scanners read afterwards */
    char line[1200];
    const char *nl = strchr(buf, '\n');
    if (!nl) {
        close(fd);
        return NULL;
    }
    size_t ll = (size_t)(nl - buf);
    if (ll >= sizeof line)
        ll = sizeof line - 1;
    memcpy(line, buf, ll);
    line[ll] = 0;
    if (ll > 0 && line[ll - 1] == '\r')
        line[ll - 1] = 0;
    char method[16], target[1024], proto[16];
    if (sscanf(line, "%15s %1023s %15s", method, target, proto) != 3) {
        close(fd);
        return NULL;
    }
    /* auth */
    int ok_auth = g_token == NULL;
    if (!ok_auth) {
        const char *hdr = strstr(buf, "Authorization:");
        if (hdr) {
            char tok[256];
            if (sscanf(hdr, "Authorization: Bearer %255s", tok) == 1 &&
                strcmp(tok, g_token) == 0)
                ok_auth = 1;
        }
    }
    if (!ok_auth) {
        http_out(fd, 401, "text/plain", "error: unauthorized\n");
        close(fd);
        return NULL;
    }
    /* find the query string */
    char path[1024];
    const char *qs = NULL;
    strncpy(path, target, sizeof path - 1);
    path[sizeof path - 1] = 0;
    char *qm = strchr(path, '?');
    if (qm) {
        *qm = 0;
        qs = qm + 1;
    }

    if (g_rate_limit_active && !exo_rate_take()) {
        http_out(fd, 429, "text/plain", "error: rate limit exceeded\n");
        close(fd);
        return NULL;
    }
    const char *body = "";
    char *he2 = strstr(buf, "\r\n\r\n");
    if (he2)
        body = he2 + 4;
    buf_t out = {0};
    int status = 200;
    const char *ctype = "text/plain";
    dispatch(method, path, qs ? qs : "", body, &out, &status, &ctype);
    http_out(fd, status, ctype, out.p ? out.p : "");
    buf_free(&out);
    close(fd);
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--serve] [--port 7659] [--token secret] "
            "--exomind http://127.0.0.1:7654\n"
            "       %s /context?agent=<id>[&budget=<n>]\n",
            prog, prog);
}

/* console mode: run ONE operation in-process (`exocontext
 * /context?agent=...`), agent/budget on the query, body on --body or
 * stdin when the query lacks an agent, print the response body, exit
 * 0/1/2. No socket is opened. */
static int console_run(exo_t *e, const char *path_arg, const char *body_arg)
{
    char pathbuf[512];
    char query[1536] = "";
    snprintf(pathbuf, sizeof pathbuf, "%s", path_arg);
    char *q = strchr(pathbuf, '?');
    if (q) {
        *q = 0;
        snprintf(query, sizeof query, "%s", q + 1);
    }
    char body[65536] = "";
    const char *b = "";
    if (body_arg[0]) {
        snprintf(body, sizeof body, "%s", body_arg);
        b = body;
    } else if (!query_param(query, "agent", body, sizeof body) &&
               !isatty(0)) {
        /* POST-style call: agent comes from the body on stdin */
        size_t blen = 0;
        ssize_t n;
        while (blen < sizeof body - 1 &&
               (n = read(0, body + blen, sizeof body - 1 - blen)) > 0)
            blen += (size_t)n;
        body[blen] = 0;
        b = body;
    }
    g_exo_ctx = e;
    if (strcmp(pathbuf, "/context") && strcmp(pathbuf, "/ping") &&
        strcmp(pathbuf, "/")) {
        fprintf(stderr, "exocontext: unknown operation %s\n", path_arg);
        return 2;
    }
    buf_t out = {0};
    int status = 200;
    const char *ctype = "text/plain";
    dispatch("GET", pathbuf, query, b, &out, &status, &ctype);
    if (status >= 400) {
        fprintf(stderr, "exocontext: %s failed (%d)\n%s", pathbuf, status,
                out.p ? out.p : "");
        buf_free(&out);
        return 1;
    }
    fputs(out.p ? out.p : "", stdout);
    buf_free(&out);
    return 0;
}

int main(int argc, char **argv)
{
    int port = 7659;
    const char *exomind_url = NULL;
    const char *token = NULL;
    const char *console_path = (argc >= 2 && argv[1][0] == '/') ? argv[1]
                                                                : NULL;
    int want_server = 0; /* only --serve or --port start the server */
    const char *body_arg = "";
    for (int i = console_path ? 2 : 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
            want_server = 1;
        } else if (!strcmp(a, "--serve"))
            want_server = 1;
        else if (!strcmp(a, "--body") && i + 1 < argc)
            body_arg = argv[++i];
        else if (!strcmp(a, "--token") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(a, "--keys") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(a, "--rate-limit") && i + 1 < argc) {
            exo_rate_init(atol(argv[++i]));
            g_rate_limit_active = 1;
        } else if (!strcmp(a, "--log-level") && i + 1 < argc) {
            int lv = exo_parse_log_level(argv[++i]);
            if (lv < 0) {
                fprintf(stderr,
                        "exocontext: bad log level (error|warn|info|debug)\n");
                return 1;
            }
            exo_set_log_level(lv);
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            static exo_help_t self[1];
            self[0].name = "exocontext";
            self[0].spec = "exocontext v" EXOCONTEXT_VERSION " - context continuity\n"
                "usage: exocontext [--serve] --exomind <url> [options]\n"
                "console: exocontext /context?agent=<id>[&budget=n]\n"
                "GET /context?agent=<id>[&budget=n] = bounded digest\n";
            exo_help_add(self, 1);
            exo_help_add_siblings();
            if (i + 1 < argc && !strcmp(argv[i + 1], "modules")) {
                exo_help_print_all();
                return 0;
            }
            if (i + 1 < argc) {
                exo_help_print_one(argv[i + 1]);
                return 0;
            }
            usage(argv[0]);
            return 0;
        }
        else if (!strcmp(a, "--exomind") && i + 1 < argc)
            exomind_url = argv[++i];
        else if (!strcmp(a, "--version")) {
            printf("exocontext v%s\n", EXOCONTEXT_VERSION);
            return 0;
        } else {
            fprintf(stderr, "exocontext: unknown argument %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    if (!want_server) {
        /* no HTTP listener except in server mode: run the op in-process
         * (backend init before dispatch), or print the guide (the same
         * text GET / serves) */
        if (console_path) {
            char pc[512];
            snprintf(pc, sizeof pc, "%s", console_path);
            char *qm = strchr(pc, '?');
            if (qm)
                *qm = 0;
            if (!strncmp(pc, "/exoexocontext", 14) &&
                (pc[14] == 0 || pc[14] == '/'))
                memmove(pc, pc + 14, strlen(pc + 14) + 1);
            int op_needs_exo = !strcmp(pc, "/context");
            if (!exomind_url && op_needs_exo) {
                fprintf(stderr, "exocontext: --exomind URL is required\n");
                return 1;
            }
            static exo_t e;
            exo_t *ep = NULL;
            if (op_needs_exo) {
                char err[256];
                if (exo_init(&e, exomind_url, err, sizeof err) != 0) {
                    fprintf(stderr, "exocontext: %s\n", err);
                    return 1;
                }
                ep = &e;
            }
            return console_run(ep, console_path, body_arg);
        }
        printf("%s", spec_text());
        return 0;
    }

    if (!exomind_url) {
        fprintf(stderr, "exocontext: --exomind URL is required\n");
        return 1;
    }
    static exo_t e;
    char err[256];
    if (exo_init(&e, exomind_url, err, sizeof err) != 0) {
        fprintf(stderr, "exocontext: %s\n", err);
        return 1;
    }
    g_exo_ctx = &e;
    http_set_token(token);
    g_rate_limit_active = 0;

    signal(SIGPIPE, SIG_IGN);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "exocontext: socket: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "exocontext: bind %d: %s\n", port, strerror(errno));
        return 1;
    }
    if (listen(srv, 32) != 0) {
        fprintf(stderr, "exocontext: listen: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr,
            "exocontext v" EXOCONTEXT_VERSION " listening on http://127.0.0.1:%d "
            "(exomind: %s)\n", port, exomind_url);
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_t th;
        pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)fd);
        pthread_detach(th);
    }
}
