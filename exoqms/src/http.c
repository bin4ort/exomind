/* exoqms HTTP layer: plain-text API for machines and LLMs, shaped like
 * the rest of the stack (one result per line, lowercase ok / error:).
 * Pattern: exosched/src/http.c. */
#include "exoqms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADERS (16 * 1024)
#define MAX_BODY (1024u * 1024u)
#define MAX_KEY 4096

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
    char *hdrs;
} req_t;

/* returns 0 ok, -1 malformed, -2 body too large */
static int read_request(int fd, req_t *r)
{
    char buf[MAX_HEADERS];
    size_t n = 0;
    size_t hdr_end = 0;
    while (n < sizeof buf - 1 && !hdr_end) {
        ssize_t got = read(fd, buf + n, sizeof buf - 1 - n);
        if (got <= 0)
            return -1;
        n += (size_t)got;
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
    r->hdrs = xstrndup(buf, hdr_end);
    buf[hdr_end - 2] = 0;

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
        size_t buffered = n - hdr_end;
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
    case 500: return "Internal Server Error";
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
            for (char *q = out; *q; q++)
                if (*q == '+')
                    *q = ' ';
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
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

static const char *spec_text(void)
{
    return
        "# exoqms v" EXOQMS_VERSION "\n"
        "\n"
        "The QUALITY MANAGEMENT SYSTEM for the AI-native stack: ISO\n"
        "9001-flavored quality objectives (6.2), monitoring and\n"
        "measurement (9.1), non-conformity + corrective action (8.7,\n"
        "10.2) and ISO 19011 audit programs. Durable state lives in\n"
        "exomind under `exoqms:*` keys. The audit program runs the ten\n"
        "checks defined in exoqms/standard.md, invoking exodoc, exoqms-ui,\n"
        "exoqms-code and exoqms-svg as child processes under a 5s hard\n"
        "timeout each. The universal checks (debt, hygiene, secrets) run\n"
        "exoqms-code --rules against the rule files and partition the\n"
        "findings by check-id prefix (`debt-*`, `hygiene-*`, `secrets-*`).\n"
        "\n"
        "All endpoints answer in plain text; add `json=1` to listings\n"
        "for JSON. Errors are `error: <reason>`. Wire format for bodies:\n"
        "tab-separated fields.\n"
        "\n"
        "## endpoints\n"
        "\n"
        "| method | path                    | purpose                         |\n"
        "|--------|-------------------------|---------------------------------|\n"
        "| GET    | /                       | this spec                       |\n"
        "| GET    | /ping                   | liveness: answers `pong`        |\n"
        "| POST   | /objectives             | add objective (body below)      |\n"
        "| GET    | /objectives             | list objectives                 |\n"
        "| POST   | /nc                     | raise non-conformity            |\n"
        "| GET    | /nc?id=<id>             | NC detail                       |\n"
        "| GET    | /nc?status=<st>         | list NCs, filtered              |\n"
        "| POST   | /nc?id=<id>&action=<a>  | NC lifecycle transition         |\n"
        "| POST   | /audit                  | run an audit program (body)     |\n"
        "| GET    | /audit?id=<id>          | audit report with findings      |\n"
        "| GET    | /audits                 | audit program list              |\n"
        "| GET    | /report                 | consolidated quality picture    |\n"
        "| GET    | /trends                 | metric trend + verdict          |\n"
        "\n"
        "## objectives (ISO 9001 6.2)\n"
        "\n"
        "POST /objectives body: `title<TAB>metric_key<TAB>target` with an\n"
        "optional fourth field `period` (default `iter`). Target is a\n"
        "number or a string; `met` means value >= target for numbers,\n"
        "equality for strings. Answer: `ok <id>`.\n"
        "\n"
        "GET /objectives: one record per line\n"
        "`id<TAB>title<TAB>metric_key<TAB>target<TAB>period<TAB>created`.\n"
        "\n"
        "## non-conformities (ISO 9001 8.7 / 10.2)\n"
        "\n"
        "POST /nc body: `title<TAB>severity<TAB>description`; severity is\n"
        "`major` or `minor`. Answer: `ok <id>`; id has the form\n"
        "`<epoch>:<hex>`, status starts `open`.\n"
        "\n"
        "Lifecycle: `open -> analysis -> corrective -> verify -> closed`.\n"
        "POST /nc?id=<id>&action=analyse|correct|verify|close advances the\n"
        "NC; every transition is written to the exomind note feed as an\n"
        "audit-trail entry. Invalid transitions are rejected with 400\n"
        "naming the expected status. `close` is the escape hatch: it is\n"
        "allowed from ANY status when the body carries\n"
        "`corrective_action<TAB>evidence` (a third field is appended as a\n"
        "note, a fourth sets `closed_by`, default `api`). From `verify`\n"
        "the body may be just a note.\n"
        "\n"
        "## audit programs (ISO 19011)\n"
        "\n"
        "POST /audit body: `name<TAB>criteria` where criteria is a\n"
        "comma-separated list of check ids (empty = all seven), plus an\n"
        "optional third field `agents` for the dogfood check. Query\n"
        "`?target=<path>` feeds the ui-audit check (and overrides the\n"
        "scan target of code-safety and asset-logic). Each check runs\n"
        "with a 5s timeout; child processes that overrun are SIGKILLed.\n"
        "\n"
        "Checks: `component-tests` (run each manifest test command), "
        "`doc-compliance` (exodoc audit), `dogfood` (swarm conventions),\n"
        "`ui-audit` (exoqms-ui), `metrics` (iter trend of\n"
        "metric:iterN:tests_passing), `code-safety` (exoqms-code on the\n"
        "stack's own C source; default target = the manifest source dirs,\n"
        "passes when 0 major findings — minor findings are non-fatal),\n"
        "`asset-logic` (exoqms-svg on the stack's own SVG assets; default\n"
        "target = the repo root, passes when 0 major findings). The\n"
        "universal checks: `debt` (passes when debt-* findings <= the\n"
        "thresholds.debt from .exoqms.json, default 10), `hygiene`\n"
        "(passes when 0 hygiene-* findings), `secrets` (passes when 0\n"
        "secrets-* findings; matched lines are masked to *** in the\n"
        "evidence). Without a stack manifest the component-tests check\n"
        "runs the `test` commands from .exoqms.json against the whole\n"
        "project, and doc-compliance verifies the `docs` file list.\n"
        "Answer:\n"
        "`ok <audit-id> <score>%`.\n"
        "GET /audit?id= prints the record line plus one findings line per\n"
        "check: `check<TAB>result<TAB>evidence`.\n"
        "\n"
        "## report and trends\n"
        "\n"
        "GET /report: objective statuses (met/not/no-data), open NC\n"
        "count, last audit score, trend verdict and stagnation flag.\n"
        "GET /trends: `metric:iterN:tests_passing <value>` lines oldest\n"
        "to newest plus `trend up|flat|down`.\n"
        "\n"
        "## durability\n"
        "\n"
        "On startup exoqms reloads all `exoqms:obj:*`, `exoqms:nc:*` and\n"
        "`exoqms:audit:*` keys plus its config keys from exomind. If\n"
        "exomind is down at startup the reload is retried every second in\n"
        "the background until it succeeds. State is never stored locally.\n"
        "\n"
        "## auth\n"
        "\n"
        "Start with `--token secret` (or env EXOQMS_TOKEN), then send\n"
        "`Authorization: Bearer secret` on every request.\n"
        "\n"
        "usage: exoqms [--host <addr>] [--port <n>]\n"
        "              [--exomind URL] [--exosched URL] [--exodoc <path>]\n"
        "              [--ui <path>] [--code <path>] [--svg <path>]\n"
        "              [--rules <dir>] [--repo <dir>] [--agents <a,b,c>]\n"
        "              [--notes24h <n>] [--token <t>] [--help] [--version]\n";
}

/* ---------- handlers ---------- */

static const char *known_check(const char *id)
{
    static const char *known[] = {"component-tests", "doc-compliance",
                                  "dogfood", "ui-audit", "metrics",
                                  "code-safety", "asset-logic", "debt",
                                  "hygiene", "secrets", "agent-health",
                                  "docs-coverage"};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(known[i], id))
            return known[i];
    return NULL;
}

static void route(req_t *r, exo_t *e, cfg_t *cfg, qms_t *q, buf_t *out,
                  int *status, const char **ctype)
{
    const char *path = r->path;
    char tmp[4096];
    char err[512];

    if (!strcmp(path, "/") || !strcmp(path, "/help") ||
        !strcmp(path, "/spec")) {
        *ctype = "text/markdown; charset=utf-8";
        buf_puts(out, spec_text());
        return;
    }
    if (!strcmp(path, "/ping")) {
        buf_puts(out, "pong");
        return;
    }

    /* ---- objectives ---- */
    if (!strcmp(path, "/objectives")) {
        if (!strcmp(r->method, "POST")) {
            if (r->body_len == 0) {
                *status = 400;
                buf_puts(out, "error: empty body");
                return;
            }
            char *bf[4];
            int nf = tab_split(r->body, bf, 4);
            if (nf < 3) {
                *status = 400;
                buf_puts(out, "error: body must be "
                              "title<TAB>metric_key<TAB>target");
                return;
            }
            char id[ID_MAX];
            const char *period = nf > 3 && bf[3][0] ? bf[3] : "iter";
            if (obj_create(q, e, bf[0], bf[1], bf[2], period, id, sizeof id,
                           err, sizeof err) != 0) {
                *status = 400;
                buf_printf(out, "error: %s", err);
                return;
            }
            buf_printf(out, "ok %s", id);
            buf_t note = {0};
            buf_printf(&note, "objective %s %s: %s target %s (period %s)",
                       id, bf[0], bf[1], bf[2], period);
            if (exo_note(e, note.p, err, sizeof err) != 0)
                fprintf(stderr, "exoqms: objective note failed: %s\n", err);
            buf_free(&note);
            return;
        }
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        pthread_mutex_lock(&q->mu);
        int j = qp_str(r->query, "json", tmp, sizeof tmp);
        if (j) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "[");
            for (size_t i = 0; i < q->n_objs; i++) {
                obj_t *o = &q->objs[i];
                if (i)
                    buf_puts(out, ",");
                char *je1 = json_escape(o->desc, strlen(o->desc));
                char *je2 = json_escape(o->metric, strlen(o->metric));
                char *je3 = json_escape(o->target, strlen(o->target));
                char *je4 = json_escape(o->period, strlen(o->period));
                buf_printf(out,
                           "{\"id\":\"%s\",\"title\":\"%s\","
                           "\"metric_key\":\"%s\",\"target\":\"%s\","
                           "\"period\":\"%s\",\"created\":%lld}",
                           o->id, je1, je2, je3, je4, (long long)o->created);
                free(je1);
                free(je2);
                free(je3);
                free(je4);
            }
            buf_puts(out, "]");
        } else {
            for (size_t i = 0; i < q->n_objs; i++) {
                obj_t *o = &q->objs[i];
                char *d = esc_line(o->desc, strlen(o->desc));
                char *m = esc_line(o->metric, strlen(o->metric));
                char *t = esc_line(o->target, strlen(o->target));
                char *p = esc_line(o->period, strlen(o->period));
                buf_printf(out, "%s\t%s\t%s\t%s\t%s\t%lld\n", o->id, d, m, t,
                           p, (long long)o->created);
                free(d);
                free(m);
                free(t);
                free(p);
            }
        }
        pthread_mutex_unlock(&q->mu);
        return;
    }

    /* ---- non-conformities ---- */
    if (!strcmp(path, "/nc")) {
        char id[ID_MAX];
        int has_id = qp_str(r->query, "id", id, sizeof id);
        if (!strcmp(r->method, "POST")) {
            if (has_id) {
                char act[32];
                if (!qp_str(r->query, "action", act, sizeof act)) {
                    *status = 400;
                    buf_puts(out, "error: missing action "
                                  "(analyse|correct|verify|close)");
                    return;
                }
                char nst[STATUS_MAX];
                if (nc_transition(q, e, id, act, r->body ? r->body : "",
                                  nst, sizeof nst, err, sizeof err) != 0) {
                    *status = 400;
                    buf_printf(out, "error: %s", err);
                    return;
                }
                buf_printf(out, "ok %s %s", id, nst);
                return;
            }
            if (r->body_len == 0) {
                *status = 400;
                buf_puts(out, "error: empty body");
                return;
            }
            char *bf[4];
            int nf = tab_split(r->body, bf, 4);
            if (nf < 3) {
                *status = 400;
                buf_puts(out, "error: body must be "
                              "title<TAB>severity<TAB>description");
                return;
            }
            char nid[ID_MAX];
            if (nc_create(q, e, bf[0], bf[1], bf[2], "api", nid, sizeof nid,
                          err, sizeof err) != 0) {
                *status = 400;
                buf_printf(out, "error: %s", err);
                return;
            }
            buf_printf(out, "ok %s", nid);
            buf_t note = {0};
            buf_printf(&note, "nc %s %s %s open (source api)", nid, bf[0],
                       bf[1]);
            if (exo_note(e, note.p, err, sizeof err) != 0)
                fprintf(stderr, "exoqms: nc note failed: %s\n", err);
            buf_free(&note);
            return;
        }
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        char stfilter[64];
        int have_st = qp_str(r->query, "status", stfilter, sizeof stfilter);
        int j = qp_str(r->query, "json", tmp, sizeof tmp);
        pthread_mutex_lock(&q->mu);
        if (has_id) {
            nc_t *c = nc_find(q, id);
            if (!c) {
                pthread_mutex_unlock(&q->mu);
                *status = 404;
                buf_puts(out, "error: no such nc");
                return;
            }
            if (j) {
                *ctype = "application/json; charset=utf-8";
                char *je1 = json_escape(c->title, strlen(c->title));
                char *je2 = json_escape(c->desc, strlen(c->desc));
                char *je3 = json_escape(c->source, strlen(c->source));
                char *je4 = json_escape(c->caction, strlen(c->caction));
                char *je5 = json_escape(c->evidence, strlen(c->evidence));
                char *je6 = json_escape(c->closed_by, strlen(c->closed_by));
                buf_printf(out,
                           "{\"id\":\"%s\",\"title\":\"%s\","
                           "\"description\":\"%s\",\"severity\":\"%s\","
                           "\"status\":\"%s\",\"source\":\"%s\","
                           "\"detected_at\":%lld,\"corrective_action\":\"%s\","
                           "\"evidence\":\"%s\",\"closed_at\":%lld,"
                           "\"closed_by\":\"%s\"}",
                           c->id, je1, je2, c->sev, c->status, je3,
                           (long long)c->detected_at, je4, je5,
                           (long long)c->closed_at, je6);
                free(je1);
                free(je2);
                free(je3);
                free(je4);
                free(je5);
                free(je6);
            } else {
                char *t = esc_line(c->title, strlen(c->title));
                char *d = esc_line(c->desc, strlen(c->desc));
                char *s = esc_line(c->source, strlen(c->source));
                char *ca = esc_line(c->caction, strlen(c->caction));
                char *ev = esc_line(c->evidence, strlen(c->evidence));
                char *cb = esc_line(c->closed_by, strlen(c->closed_by));
                buf_printf(out,
                           "%s\t%s\t%s\t%s\t%lld\t%s\t%s\t%s\t%lld\t%s\t%s\n",
                           c->id, t, c->sev, c->status,
                           (long long)c->detected_at, s, ca, ev,
                           (long long)c->closed_at, cb, d);
                free(t);
                free(d);
                free(s);
                free(ca);
                free(ev);
                free(cb);
            }
            pthread_mutex_unlock(&q->mu);
            return;
        }
        if (j) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "[");
            size_t k = 0;
            for (size_t i = 0; i < q->n_ncs; i++) {
                nc_t *c = &q->ncs[i];
                if (have_st && strcmp(c->status, stfilter) != 0)
                    continue;
                if (k++)
                    buf_puts(out, ",");
                char *je1 = json_escape(c->title, strlen(c->title));
                char *je2 = json_escape(c->source, strlen(c->source));
                buf_printf(out,
                           "{\"id\":\"%s\",\"title\":\"%s\","
                           "\"severity\":\"%s\",\"status\":\"%s\","
                           "\"detected_at\":%lld,\"source\":\"%s\"}",
                           c->id, je1, c->sev, c->status,
                           (long long)c->detected_at, je2);
                free(je1);
                free(je2);
            }
            buf_puts(out, "]");
        } else {
            for (size_t i = 0; i < q->n_ncs; i++) {
                nc_t *c = &q->ncs[i];
                if (have_st && strcmp(c->status, stfilter) != 0)
                    continue;
                char *t = esc_line(c->title, strlen(c->title));
                char *s = esc_line(c->source, strlen(c->source));
                buf_printf(out, "%s\t%s\t%s\t%s\t%lld\t%s\n", c->id, t,
                           c->sev, c->status, (long long)c->detected_at, s);
                free(t);
                free(s);
            }
        }
        pthread_mutex_unlock(&q->mu);
        return;
    }

    /* ---- audits ---- */
    if (!strcmp(path, "/audits")) {
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        int j = qp_str(r->query, "json", tmp, sizeof tmp);
        pthread_mutex_lock(&q->mu);
        if (j) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "[");
            for (size_t i = 0; i < q->n_audits; i++) {
                audit_t *a = &q->audits[i];
                if (i)
                    buf_puts(out, ",");
                char *je1 = json_escape(a->name, strlen(a->name));
                char *je2 = json_escape(a->criteria, strlen(a->criteria));
                buf_printf(out,
                           "{\"id\":\"%s\",\"name\":\"%s\",\"criteria\":\"%s\","
                           "\"status\":\"%s\",\"score\":%d,"
                           "\"scheduled_at\":%lld}",
                           a->id, je1, je2, a->status, a->score,
                           (long long)a->scheduled);
                free(je1);
                free(je2);
            }
            buf_puts(out, "]");
        } else {
            for (size_t i = 0; i < q->n_audits; i++) {
                audit_t *a = &q->audits[i];
                buf_printf(out, "%s\t%s\t%s\t%d\t%lld\n", a->id, a->name,
                           a->status, a->score, (long long)a->scheduled);
            }
        }
        pthread_mutex_unlock(&q->mu);
        return;
    }

    if (!strcmp(path, "/audit")) {
        if (strcmp(r->method, "GET") && strcmp(r->method, "POST") &&
            strcmp(r->method, "HEAD")) {
            *status = 405;
            buf_puts(out, "error: use GET or POST");
            return;
        }
        if (strcmp(r->method, "POST")) {
            char id[ID_MAX];
            if (!qp_str(r->query, "id", id, sizeof id)) {
                *status = 400;
                buf_puts(out, "error: missing id");
                return;
            }
            int j = qp_str(r->query, "json", tmp, sizeof tmp);
            pthread_mutex_lock(&q->mu);
            audit_t *a = audit_find(q, id);
            if (!a) {
                pthread_mutex_unlock(&q->mu);
                *status = 404;
                buf_puts(out, "error: no such audit");
                return;
            }
            if (j) {
                *ctype = "application/json; charset=utf-8";
                char *je1 = json_escape(a->name, strlen(a->name));
                char *je2 = json_escape(a->criteria, strlen(a->criteria));
                buf_printf(out,
                           "{\"id\":\"%s\",\"name\":\"%s\",\"criteria\":\"%s\","
                           "\"scheduled_at\":%lld,\"status\":\"%s\","
                           "\"score\":%d,\"findings\":[",
                           a->id, je1, je2, (long long)a->scheduled, a->status,
                           a->score);
                free(je1);
                free(je2);
                char *copy = xstrdup(a->findings);
                char *save = NULL;
                int k = 0;
                for (char *l = strtok_r(copy, "\n", &save); l;
                     l = strtok_r(NULL, "\n", &save)) {
                    char *bf[3];
                    int nf = tab_split(l, bf, 3);
                    if (k++)
                        buf_puts(out, ",");
                    char *je1 = json_escape(bf[0], strlen(bf[0]));
                    char *je2 = nf > 1 ? json_escape(bf[1], strlen(bf[1]))
                                       : xstrdup("");
                    char *je3 = nf > 2 ? json_escape(bf[2], strlen(bf[2]))
                                       : xstrdup("");
                    buf_printf(out,
                               "{\"check\":\"%s\",\"result\":\"%s\","
                               "\"evidence\":\"%s\"}",
                               je1, je2, je3);
                    free(je1);
                    free(je2);
                    free(je3);
                }
                free(copy);
                buf_puts(out, "]}");
            } else {
                buf_printf(out, "%s\t%s\t%s\t%lld\t%s\t%d\n", a->id, a->name,
                           a->criteria, (long long)a->scheduled, a->status,
                           a->score);
                char *copy = xstrdup(a->findings);
                char *save = NULL;
                for (char *l = strtok_r(copy, "\n", &save); l;
                     l = strtok_r(NULL, "\n", &save)) {
                    char *bf[3];
                    int nf = tab_split(l, bf, 3);
                    char *ev = nf > 2 ? bf[2] : "";
                    buf_printf(out, "%s\t%s\t%s\n", bf[0],
                               nf > 1 ? bf[1] : "", ev);
                }
                free(copy);
            }
            pthread_mutex_unlock(&q->mu);
            return;
        }
        /* ---- POST /audit: run the audit program ---- */
        if (r->body_len == 0) {
            *status = 400;
            buf_puts(out, "error: empty body");
            return;
        }
        char *bf[4];
        int nf = tab_split(r->body, bf, 4);
        if (nf < 1 || !bf[0][0]) {
            *status = 400;
            buf_puts(out, "error: body must be name<TAB>criteria");
            return;
        }
        char *name = bf[0];
        char *criteria = nf > 1 ? bf[1] : (char *)"";
        char *agents = nf > 2 && bf[2][0] ? bf[2] : NULL;
        char qagents[1024];
        if (qp_str(r->query, "agents", qagents, sizeof qagents) && qagents[0])
            agents = qagents;
        char target[4096];
        int have_target = qp_str(r->query, "target", target, sizeof target);

        /* validate criteria */
        char *ids[16];
        int nids = 0;
        if (!criteria[0]) {
            ids[nids++] = (char *)"component-tests";
            ids[nids++] = (char *)"doc-compliance";
            ids[nids++] = (char *)"dogfood";
            ids[nids++] = (char *)"ui-audit";
            ids[nids++] = (char *)"metrics";
            ids[nids++] = (char *)"code-safety";
            ids[nids++] = (char *)"asset-logic";
        } else {
            char *copy = xstrdup(criteria);
            char *save = NULL;
            for (char *c = strtok_r(copy, ",", &save); c;
                 c = strtok_r(NULL, ",", &save)) {
                while (*c == ' ')
                    c++;
                if (!known_check(c)) {
                    *status = 400;
                    buf_printf(out, "error: unknown check '%s'", c);
                    free(copy);
                    return;
                }
                ids[nids++] = xstrdup(c);
            }
            free(copy);
        }

        char id[ID_MAX];
        snprintf(id, sizeof id, "%lld:%08x", (long long)now_epoch(),
                 rand32());

        check_ctx_t ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.cfg = cfg;
        ctx.exo = e;
        ctx.target = have_target ? target : NULL;
        ctx.agents = agents;

        finding_t finds[16];
        int nfinds = 0;
        for (int i = 0; i < nids; i++) {
            check_run(ids[i], &ctx, &finds[nfinds]);
            nfinds++;
            if (ids[i] != (char *)"component-tests" &&
                ids[i] != (char *)"doc-compliance" &&
                ids[i] != (char *)"dogfood" &&
                ids[i] != (char *)"ui-audit" &&
                ids[i] != (char *)"metrics" &&
                ids[i] != (char *)"code-safety" &&
                ids[i] != (char *)"asset-logic" &&
                ids[i] != (char *)"debt" &&
                ids[i] != (char *)"hygiene" &&
                ids[i] != (char *)"secrets")
                free(ids[i]);
        }
        ctx_cleanup(&ctx);
        int pass = 0, fail = 0, skip = 0;
        buf_t blob = {0};
        for (int i = 0; i < nfinds; i++) {
            const char *res = finds[i].res == R_PASS ? "pass"
                              : finds[i].res == R_FAIL ? "fail" : "skip";
            if (finds[i].res == R_PASS)
                pass++;
            else if (finds[i].res == R_FAIL)
                fail++;
            else
                skip++;
            char *ev = esc_line(finds[i].evidence, strlen(finds[i].evidence));
            buf_printf(&blob, "%s\t%s\t%s\n", finds[i].id, res, ev);
            free(ev);
        }
        int score = (pass + fail) > 0
                        ? (100 * pass) / (pass + fail)
                        : 0;
        if (audit_save(q, e, id, name, criteria ? criteria : "", blob.p,
                       score, err, sizeof err) != 0) {
            buf_free(&blob);
            *status = 500;
            buf_printf(out, "error: exomind unavailable: %s", err);
            return;
        }
        buf_free(&blob);
        buf_printf(out, "ok %s %d%%", id, score);
        buf_t note = {0};
        buf_printf(&note, "exoqms audit %s %s: %d pass, %d fail, %d skip "
                          "(score %d%%)", id, name, pass, fail, skip, score);
        if (exo_note(e, note.p, err, sizeof err) != 0)
            fprintf(stderr, "exoqms: audit note failed: %s\n", err);
        buf_free(&note);
        return;
    }

    /* ---- consolidated quality picture ---- */
    if (!strcmp(path, "/report")) {
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        int j = qp_str(r->query, "json", tmp, sizeof tmp);
        int met = 0, nd = 0;
        pthread_mutex_lock(&q->mu);
        size_t total = q->n_objs;
        buf_t objs_json = {0};
        if (j) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(&objs_json, "\"objectives\":[");
        }
        for (size_t i = 0; i < q->n_objs; i++) {
            obj_t *o = &q->objs[i];
            char *v = NULL;
            int is_met = -1;
            char val[128] = "no-data";
            if (exo_get(e, o->metric, &v, err, sizeof err) == 0 && v) {
                trim_crlf(v);
                char *ve = v;
                while (*ve && isspace((unsigned char)*ve))
                    ve++;
                if (isdigit((unsigned char)*ve) || *ve == '-') {
                    long long mv = strtoll(ve, NULL, 10);
                    long long tg = strtoll(o->target, NULL, 10);
                    snprintf(val, sizeof val, "%lld", (long long)mv);
                    is_met = mv >= tg;
                } else {
                    snprintf(val, sizeof val, "%s", ve);
                    is_met = strcmp(ve, o->target) == 0;
                }
            }
            free(v);
            if (is_met < 0)
                nd++;
            else if (is_met)
                met++;
            if (j) {
                if (i)
                    buf_puts(&objs_json, ",");
                char *je1 = json_escape(o->desc, strlen(o->desc));
                char *je2 = json_escape(o->metric, strlen(o->metric));
                char *je3 = json_escape(o->target, strlen(o->target));
                buf_printf(&objs_json,
                           "{\"id\":\"%s\",\"title\":\"%s\","
                           "\"metric_key\":\"%s\",\"target\":\"%s\","
                           "\"value\":\"%s\",\"met\":%s}",
                           o->id, je1, je2, je3, val,
                           is_met < 0 ? "null" : is_met ? "true" : "false");
                free(je1);
                free(je2);
                free(je3);
            } else {
                char *d = esc_line(o->desc, strlen(o->desc));
                char *m = esc_line(o->metric, strlen(o->metric));
                char *t = esc_line(o->target, strlen(o->target));
                buf_printf(out, "objective\t%s\t%s\t%s\t%s\t%s\t%s\n", o->id,
                           d, m, t, val,
                           is_met < 0 ? "no-data" : is_met ? "met" : "not");
                free(d);
                free(m);
                free(t);
            }
        }
        int open_ncs = 0;
        for (size_t i = 0; i < q->n_ncs; i++)
            if (strcmp(q->ncs[i].status, "closed") != 0)
                open_ncs++;
        audit_t *last = NULL;
        for (size_t i = 0; i < q->n_audits; i++)
            if (!last || q->audits[i].scheduled > last->scheduled)
                last = &q->audits[i];
        pthread_mutex_unlock(&q->mu);
        if (j) {
            buf_printf(&objs_json, "]");
            buf_printf(out, "{%s,\"objectives_summary\":{\"met\":%d,"
                       "\"total\":%zu,\"no_data\":%d},\"open_ncs\":%d,",
                       objs_json.p, met, total, nd, open_ncs);
            if (last)
                buf_printf(out,
                           "\"last_audit\":{\"id\":\"%s\",\"score\":%d,"
                           "\"status\":\"%s\"}",
                           last->id, last->score, last->status);
            else
                buf_puts(out, "\"last_audit\":null");
            int64_t *vals = NULL;
            int n = 0;
            char *list = NULL;
            size_t llen = 0;
            int flag = 0;
            const char *tv = "unknown";
            if (trend_values(e, &vals, &n, &list, &llen) == 0) {
                tv = trend_verdict(vals, n, &flag);
                free(vals);
                free(list);
            }
            buf_printf(out, ",\"trend\":\"%s\",\"stagnation\":%s}", tv,
                       flag ? "true" : "false");
        } else {
            buf_printf(out, "objectives_summary\t%d/%zu met\t%d no-data\n",
                       met, total, nd);
            buf_printf(out, "open_ncs\t%d\n", open_ncs);
            if (last)
                buf_printf(out, "last_audit\t%s\t%d\t%s\n", last->id,
                           last->score, last->status);
            else
                buf_puts(out, "last_audit\tnone\n");
            int64_t *vals = NULL;
            int n = 0;
            char *list = NULL;
            size_t llen = 0;
            int flag = 0;
            const char *tv = "unknown";
            if (trend_values(e, &vals, &n, &list, &llen) == 0) {
                tv = trend_verdict(vals, n, &flag);
                free(vals);
                free(list);
            }
            buf_printf(out, "trend\t%s\n", tv);
            buf_printf(out, "stagnation\t%d\n", flag);
        }
        buf_free(&objs_json);
        return;
    }

    /* ---- trends ---- */
    if (!strcmp(path, "/trends")) {
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        int j = qp_str(r->query, "json", tmp, sizeof tmp);
        int64_t *vals = NULL;
        int n = 0;
        char *list = NULL;
        size_t llen = 0;
        if (trend_values(e, &vals, &n, &list, &llen) != 0) {
            *status = 500;
            buf_puts(out, "error: exomind unreachable");
            return;
        }
        int flag = 0;
        const char *tv = trend_verdict(vals, n, &flag);
        if (j) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "{\"values\":[");
            char *save = NULL;
            int k = 0;
            for (char *l = strtok_r(list, "\n", &save); l;
                 l = strtok_r(NULL, "\n", &save)) {
                char *tab = strchr(l, '\t');
                if (k++)
                    buf_puts(out, ",");
                if (tab) {
                    *tab = 0;
                    buf_printf(out, "{\"key\":\"%s\",\"value\":%s}", l,
                               tab + 1);
                } else {
                    buf_printf(out, "{\"key\":\"%s\",\"value\":null}", l);
                }
            }
            buf_printf(out, "],\"trend\":\"%s\",\"stagnation\":%s}", tv,
                       flag ? "true" : "false");
        } else {
            buf_puts(out, list);
            buf_printf(out, "trend %s\n", tv);
        }
        free(vals);
        free(list);
        return;
    }

    *status = 404;
    buf_puts(out, "error: unknown path");
}

int http_handle_conn(int fd, exo_t *e, cfg_t *cfg, qms_t *q)
{
    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    req_t r;
    memset(&r, 0, sizeof r);
    int rc = read_request(fd, &r);
    if (rc == -2) {
        send_response(fd, 413, "text/plain; charset=utf-8",
                      "error: body too large\n", 21, 0);
        return 0;
    }
    if (rc != 0) {
        send_response(fd, 400, "text/plain; charset=utf-8",
                      "error: bad request\n", 19, 0);
        return 0;
    }
    if (!strcmp(r.method, "OPTIONS")) {
        send_response(fd, 204, "text/plain", "", 0, 0);
        free(r.body);
        free(r.hdrs);
        return 0;
    }
    if (strcmp(r.method, "GET") && strcmp(r.method, "POST") &&
        strcmp(r.method, "DELETE") && strcmp(r.method, "HEAD")) {
        send_response(fd, 405, "text/plain; charset=utf-8",
                      "error: method not allowed\n", 25, 0);
        free(r.body);
        free(r.hdrs);
        return 0;
    }
    if (!auth_ok(&r)) {
        send_response(fd, 401, "text/plain; charset=utf-8",
                      "error: unauthorized\n", 19, 0);
        free(r.body);
        free(r.hdrs);
        return 0;
    }

    int status = 200;
    const char *ctype = "text/plain; charset=utf-8";
    buf_t out = {0};
    route(&r, e, cfg, q, &out, &status, &ctype);
    int head = !strcmp(r.method, "HEAD");
    send_response(fd, status, ctype, out.p ? out.p : "", out.len, head);
    buf_free(&out);
    free(r.body);
    free(r.hdrs);
    return 0;
}
