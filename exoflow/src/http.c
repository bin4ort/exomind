/* exoflow HTTP layer: plain-text API for machines and LLMs, shaped like
 * exomind's and exosched's (one result per line, lowercase ok / error). */
#include "exoflow.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADERS (16 * 1024)
#define MAX_BODY (1024u * 1024u)

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

static int qp_long(const char *qs, const char *name, long def)
{
    char tmp[32];
    if (!qp_str(qs, name, tmp, sizeof tmp))
        return (int)def;
    return (int)strtol(tmp, NULL, 10);
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
        "# exoflow v" EXOFLOW_VERSION "\n"
        "\n"
        "The orchestrator for agent swarms: a dependency-graph task\n"
        "orchestrator. Flows are named lists of steps; steps carry deps,\n"
        "state, owner and an optional deadline. ALL durable state lives in\n"
        "exomind under keys `exoflow:flow:<id>`; step deadlines are also\n"
        "registered as reminders on exosched so agents see them in the\n"
        "exomind note feed. Every state change is appended to the note feed\n"
        "as `flow <id> step <sid> -> <state> by <owner>` (the swarm's paper\n"
        "trail).\n"
        "\n"
        "All endpoints answer in plain text (tab-separated listings); add\n"
        "`json=1` for JSON. Errors are `error: <reason>`, unknown flows\n"
        "answer 404 `missing`.\n"
        "\n"
        "## endpoints\n"
        "\n"
        "| method | path                          | purpose                      |\n"
        "|--------|-------------------------------|------------------------------|\n"
        "| GET    | /                             | this spec                    |\n"
        "| GET    | /ping                         | liveness: answers `pong`     |\n"
        "| POST   | /flow                         | create a flow (body below)   |\n"
        "| GET    | /flow?id=<f>                  | one flow, TSV or json=1      |\n"
        "| GET    | /flows                        | list flows (status=, limit, offset) |\n"
        "| GET    | /next?flow=<f>&worker=<w>     | claim the next ready step    |\n"
        "| POST   | /step?flow=<f>&id=<s>         | done / failed / unclaim      |\n"
        "| POST   | /flow?id=<f>&action=cancel    | cancel non-terminal steps    |\n"
        "| DELETE | /flow?id=<f>                  | remove a flow and its keys   |\n"
        "\n"
        "## creating a flow\n"
        "\n"
        "POST /flow: line 1 is the flow name; each following line is a step:\n"
        "\n"
        "    id<TAB>description<TAB>deps<TAB>deadline_epoch(optional)\n"
        "\n"
        "deps are comma-separated step ids (may be empty). Description and\n"
        "name may use \\n \\t \\\\ escapes. Replies `ok <flow-id> <nsteps>`.\n"
        "Step ids and worker names are restricted to [A-Za-z0-9._-] (they\n"
        "travel inside URLs, TSV columns and the deps column). Rejects:\n"
        "duplicate ids, unknown deps, cyclic deps.\n"
        "\n"
        "## reading\n"
        "\n"
        "GET /flow?id=<f> returns the flow header line `flow<TAB><id><TAB><name><TAB><status>`\n"
        "then one line per step: `step<TAB><id><TAB><state><TAB><owner><TAB><deadline><TAB><desc>`\n"
        "(deadline is an epoch or `-`). Flow status is derived: `done` when\n"
        "all steps are done, `cancelled` when any step was cancelled,\n"
        "`failed` when every step is terminal with at least one failure,\n"
        "else `active`.\n"
        "\n"
        "GET /flows lists flows with `status=` (active|done|cancelled|failed)\n"
        "and `limit=`/`offset=` (defaults 100/0).\n"
        "\n"
        "## claiming & transitions\n"
        "\n"
        "GET /next?flow=<f>&worker=<w> atomically claims the first pending\n"
        "step whose deps are all done and replies `ok <stepid>` (or `none`;\n"
        "json=1 gives `{\"flow\":..,\"step\":..}`, null when none). Claims are\n"
        "exclusive: a claimed step is never handed out again until\n"
        "`unclaim` or `failed`.\n"
        "\n"
        "POST /step?flow=<f>&id=<s> with body `done [note...]`,\n"
        "`failed [note...]` or `unclaim`. Rules: done only when ALL deps are\n"
        "done (`error: deps pending`); done twice (`error: already done`);\n"
        "failed is allowed from pending/claimed/overdue; unclaim releases a\n"
        "claimed step back to pending. The optional note text is appended to\n"
        "the audit note.\n"
        "\n"
        "## deadlines\n"
        "\n"
        "A step may carry a deadline_epoch (4th column). On creation exoflow\n"
        "registers an exosched reminder `at <epoch> \"exoflow <f> <s>\"` so\n"
        "agents see the fired deadline in exomind's note feed. exoflow does\n"
        "not listen for timer fires: instead every read of /flow and /next\n"
        "and every startup reload lazily sweeps deadlines - a pending or\n"
        "claimed step whose deadline has passed becomes `overdue` (audited:\n"
        "`-> overdue by <owner>`). This keeps the daemon stateless w.r.t.\n"
        "exosched while still using it for scheduling. Registration is best\n"
        "effort: a down exosched never breaks flow creation, and the lazy\n"
        "sweep is authoritative.\n"
        "\n"
        "## durability\n"
        "\n"
        "Flows live in exomind; exoflow reloads them on startup (list prefix\n"
        "+ batch get). If exomind is down at startup, reload is retried in\n"
        "the background every second while /ping keeps answering. Every\n"
        "state change is persisted before it is acknowledged and appended to\n"
        "the note feed.\n"
        "\n"
        "## auth\n"
        "\n"
        "Start with `--token secret` (or env EXOFLOW_TOKEN), then send\n"
        "`Authorization: Bearer secret` on every request.\n"
        "\n"
        "usage: exoflow [--host <addr>] [--port <n>] [--exomind URL]\n"
        "               [--exosched URL] [--token <t>] [--help] [--version]\n";
}

static void flow_tsv(const flow_t *f, buf_t *out)
{
    char *en = esc_line(f->name, strlen(f->name));
    buf_printf(out, "flow\t%s\t%s\t%s\n", f->id, en, flow_status(f));
    free(en);
    for (size_t i = 0; i < f->nsteps; i++) {
        const step_t *s = &f->steps[i];
        char *ed = esc_line(s->desc, strlen(s->desc));
        buf_printf(out, "step\t%s\t%s\t%s\t%lld\t%s\n", s->id, s->state,
                   s->owner, (long long)(s->deadline > 0 ? s->deadline : 0),
                   ed);
        free(ed);
    }
}

static void flow_json(const flow_t *f, buf_t *out)
{
    char *en = json_escape(f->name, strlen(f->name));
    buf_printf(out, "{\"id\":\"%s\",\"name\":\"%s\",\"status\":\"%s\","
                    "\"steps\":[",
               f->id, en, flow_status(f));
    free(en);
    for (size_t i = 0; i < f->nsteps; i++) {
        const step_t *s = &f->steps[i];
        if (i)
            buf_puts(out, ",");
        char *ed = json_escape(s->desc, strlen(s->desc));
        buf_printf(out,
                   "{\"id\":\"%s\",\"desc\":\"%s\",\"deps\":[", s->id, ed);
        free(ed);
        for (size_t j = 0; j < s->ndeps; j++) {
            if (j)
                buf_puts(out, ",");
            buf_printf(out, "\"%s\"", s->deps[j]);
        }
        buf_printf(out, "],\"state\":\"%s\",\"owner\":\"%s\",\"deadline\":%lld}",
                   s->state, s->owner, (long long)s->deadline);
    }
    buf_printf(out, "]}");
}

static void route(req_t *r, cli_t *xm, cli_t *xs, buf_t *out, int *status,
                  const char **ctype)
{
    const char *path = r->path;
    char tmp[4096];
    char err[256];

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

    if (!strcmp(path, "/flow")) {
        if (strcmp(r->method, "POST") == 0) {
            char action[32];
            if (qp_str(r->query, "action", action, sizeof action)) {
                if (strcmp(action, "cancel") != 0) {
                    *status = 400;
                    buf_printf(out, "error: bad action %s", action);
                    return;
                }
                char fid[FLOW_ID_MAX];
                if (!qp_str(r->query, "id", fid, sizeof fid)) {
                    *status = 400;
                    buf_puts(out, "error: missing id");
                    return;
                }
                flows_lock();
                if (!flow_find(fid)) {
                    flows_unlock();
                    *status = 404;
                    buf_puts(out, "missing");
                    return;
                }
                int rc = flow_cancel(xm, fid, err, sizeof err);
                flows_unlock();
                if (rc != 0) {
                    *status = 500;
                    buf_printf(out, "error: %s", err);
                    return;
                }
                buf_puts(out, "ok");
                return;
            }
            if (r->body_len == 0) {
                *status = 400;
                buf_puts(out, "error: empty body");
                return;
            }
            char fid[FLOW_ID_MAX];
            size_t nsteps = 0;
            if (flow_create(xm, xs, r->body, r->body_len, fid, sizeof fid,
                            &nsteps, err, sizeof err) != 0) {
                *status = 400;
                buf_printf(out, "error: %s", err);
                return;
            }
            buf_printf(out, "ok %s %zu", fid, nsteps);
            return;
        }
        if (strcmp(r->method, "GET") == 0 || strcmp(r->method, "DELETE") == 0) {
            char fid[FLOW_ID_MAX];
            if (!qp_str(r->query, "id", fid, sizeof fid)) {
                *status = 400;
                buf_puts(out, "error: missing id");
                return;
            }
            if (strcmp(r->method, "DELETE") == 0) {
                flows_lock();
                flow_t *f = flow_find(fid);
                flows_unlock();
                if (!f) {
                    *status = 404;
                    buf_puts(out, "missing");
                    return;
                }
                int existed = 0;
                if (flow_delete(xm, fid, &existed, err, sizeof err) != 0) {
                    *status = 500;
                    buf_printf(out, "error: %s", err);
                    return;
                }
                buf_puts(out, "ok");
                return;
            }
            flows_lock();
            flow_t *f = flow_find(fid);
            if (!f) {
                flows_unlock();
                *status = 404;
                buf_puts(out, "missing");
                return;
            }
            if (flow_sweep(xm, f, err, sizeof err) != 0) {
                flows_unlock();
                *status = 500;
                buf_printf(out, "error: exomind unavailable: %s", err);
                return;
            }
            if (qp_str(r->query, "json", tmp, sizeof tmp)) {
                *ctype = "application/json; charset=utf-8";
                flow_json(f, out);
            } else {
                flow_tsv(f, out);
            }
            flows_unlock();
            return;
        }
        *status = 405;
        buf_puts(out, "error: use GET/POST/DELETE");
        return;
    }

    if (!strcmp(path, "/flows")) {
        if (strcmp(r->method, "GET")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        char filt[16] = "";
        qp_str(r->query, "status", filt, sizeof filt);
        long limit = qp_long(r->query, "limit", 100);
        long offset = qp_long(r->query, "offset", 0);
        if (limit < 0)
            limit = 100;
        if (offset < 0)
            offset = 0;
        int json = qp_str(r->query, "json", tmp, sizeof tmp);
        if (json)
            *ctype = "application/json; charset=utf-8";
        flows_lock();
        long seen = 0, emitted = 0;
        if (json)
            buf_puts(out, "[");
        for (flow_t *f = flows_first(); f && emitted < limit; f = f->next) {
            const char *st = flow_status(f);
            if (filt[0] && strcmp(st, filt) != 0)
                continue;
            if (seen++ < offset)
                continue;
            if (json) {
                if (emitted)
                    buf_puts(out, ",");
                char *en = json_escape(f->name, strlen(f->name));
                buf_printf(out, "{\"id\":\"%s\",\"name\":\"%s\",\"status\":\"%s\"}",
                           f->id, en, st);
                free(en);
            } else {
                char *en = esc_line(f->name, strlen(f->name));
                buf_printf(out, "flow\t%s\t%s\t%s\n", f->id, en, st);
                free(en);
            }
            emitted++;
        }
        if (json)
            buf_puts(out, "]");
        flows_unlock();
        return;
    }

    if (!strcmp(path, "/next")) {
        if (strcmp(r->method, "GET")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        char fid[FLOW_ID_MAX], worker[OWNER_MAX];
        if (!qp_str(r->query, "flow", fid, sizeof fid)) {
            *status = 400;
            buf_puts(out, "error: missing flow");
            return;
        }
        if (!qp_str(r->query, "worker", worker, sizeof worker) ||
            !worker[0]) {
            *status = 400;
            buf_puts(out, "error: missing worker");
            return;
        }
        flows_lock();
        flow_t *f = flow_find(fid);
        if (!f) {
            flows_unlock();
            *status = 404;
            buf_puts(out, "missing");
            return;
        }
        char sid[STEP_ID_MAX];
        if (flow_next(xm, f, worker, sid, sizeof sid, err, sizeof err) != 0) {
            flows_unlock();
            if (strcmp(err, "bad worker") == 0) {
                *status = 400;
                buf_printf(out, "error: %s", err);
            } else {
                *status = 500;
                buf_printf(out, "error: exomind unavailable: %s", err);
            }
            return;
        }
        flows_unlock();
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            if (strcmp(sid, "none") == 0)
                buf_printf(out, "{\"flow\":\"%s\",\"step\":null}", fid);
            else
                buf_printf(out, "{\"flow\":\"%s\",\"step\":\"%s\"}", fid, sid);
        } else {
            if (strcmp(sid, "none") == 0)
                buf_puts(out, "none");
            else
                buf_printf(out, "ok %s", sid);
        }
        return;
    }

    if (!strcmp(path, "/step")) {
        if (strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use POST");
            return;
        }
        char fid[FLOW_ID_MAX], sid[STEP_ID_MAX];
        if (!qp_str(r->query, "flow", fid, sizeof fid) ||
            !qp_str(r->query, "id", sid, sizeof sid)) {
            *status = 400;
            buf_puts(out, "error: missing flow or id");
            return;
        }
        if (r->body_len == 0) {
            *status = 400;
            buf_puts(out, "error: empty body");
            return;
        }
        char *action = r->body;
        char *rest = NULL;
        for (char *p = action; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                *p = 0;
                rest = p + 1;
                break;
            }
        }
        while (rest && (*rest == ' ' || *rest == '\t'))
            rest++;
        char *note = rest ? xstrdup(rest) : NULL;
        flows_lock();
        flow_t *f = flow_find(fid);
        if (!f) {
            flows_unlock();
            free(note);
            *status = 404;
            buf_puts(out, "missing");
            return;
        }
        int rc = step_do(xm, f, sid, action, note, err, sizeof err);
        flows_unlock();
        free(note);
        if (rc != 0) {
            if (strcmp(err, "no such step") == 0) {
                *status = 404;
                buf_printf(out, "error: %s", err);
            } else if (strncmp(err, "exomind unavailable", 19) == 0) {
                *status = 500;
                buf_printf(out, "error: %s", err);
            } else {
                *status = 400;
                buf_printf(out, "error: %s", err);
            }
            return;
        }
        buf_puts(out, "ok");
        return;
    }

    *status = 404;
    buf_puts(out, "error: unknown path");
}

int http_handle_conn(int fd, cli_t *xm, cli_t *xs)
{
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
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
    route(&r, xm, xs, &out, &status, &ctype);
    int head = !strcmp(r.method, "HEAD");
    send_response(fd, status, ctype, out.p ? out.p : "", out.len, head);
    buf_free(&out);
    free(r.body);
    free(r.hdrs);
    return 0;
}
