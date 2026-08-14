/* exosched HTTP layer: plain-text API for machines and LLMs, shaped like
 * exomind's (one result per line, lowercase ok / error: <reason>). */
#include "exosched.h"

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

/* looks up a header by name (case-insensitive), returns malloc'd value or NULL */
static char *hdr_value(const req_t *r, const char *name)
{
    char *copy = xstrdup(r->hdrs);
    char *line = copy;
    char *save = NULL;
    strtok_r(line, "\n", &save); /* skip request line */
    char *res = NULL;
    for (char *h = strtok_r(NULL, "\n", &save); h; h = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(h);
        while (l > 0 && (h[l - 1] == '\r' || h[l - 1] == ' '))
            h[--l] = 0;
        if (ci_prefix(h, name)) {
            const char *v = h + strlen(name);
            while (*v == ' ')
                v++;
            res = xstrdup(v);
            break;
        }
    }
    free(copy);
    return res;
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
        "# exosched v" EXOSCHED_VERSION "\n"
        "\n"
        "The alarm clock for AI agents: a scheduled-reminders + WebSocket\n"
        "push daemon. Every timer's durable state lives in exomind (the\n"
        "external long-term memory server) under keys `exosched:timer:<id>`\n"
        "with a TTL slightly past fire time, so timers survive restarts and\n"
        "fired timers expire on their own. Every fire is also written into\n"
        "the exomind note feed (`fired timer <id>: <message> at <epoch>`),\n"
        "growing the searchable knowledge feed.\n"
        "\n"
        "All endpoints answer in plain text; add `json=1` to listings for\n"
        "JSON. Errors are `error: <reason>`.\n"
        "\n"
        "## endpoints\n"
        "\n"
        "| method | path                 | purpose                             |\n"
        "|--------|----------------------|-------------------------------------|\n"
        "| GET    | /                    | this spec                           |\n"
        "| GET    | /ping                | liveness: answers `pong`            |\n"
        "| POST   | /remind              | schedule a reminder (body below)    |\n"
        "| GET    | /timers              | active timers (json=1 for JSON)     |\n"
        "| DELETE | /timer?id=<id>       | cancel a timer: `ok` or `missing`   |\n"
        "| GET    | /ws                  | WebSocket push channel (RFC 6455)   |\n"
        "\n"
        "## scheduling\n"
        "\n"
        "POST /remind with a plain-text body:\n"
        "\n"
        "    in 90s \"water the plants\"\n"
        "    in 5m \"stand up and stretch\"\n"
        "    in 2h \"push the branch\"\n"
        "    at 1786740704 \"fire at this unix epoch\"\n"
        "    every 10m \"check the pipeline\"\n"
        "    every 30s \"nudge\" until 1786740704\n"
        "\n"
        "Units: s, m, h, d. `every <n><unit> \"msg\"` schedules a RECURRING\n"
        "timer that fires every <n><unit>; the optional `until <epoch>` suffix\n"
        "(quoted messages only) stops it after the fire at or before that\n"
        "epoch. `at` in the past is rejected. The message may be quoted\n"
        "(\\\" escapes work) or unquoted to the end of the body. The answer\n"
        "is `ok <id> <when-epoch>` with an id of the form `<epoch>:<hex>`.\n"
        "\n"
        "## timers\n"
        "\n"
        "GET /timers lists active timers, one per line, tab-separated:\n"
        "`id <TAB> epoch <TAB> remaining_s <TAB> message`; recurring timers\n"
        "append two more columns `repeat_s <TAB> until` (0 until = forever).\n"
        "Add `json=1` for a JSON array; objects carry `repeat_s` and `until`\n"
        "(0 for one-shots).\n"
        "\n"
        "## websocket push\n"
        "\n"
        "GET /ws upgrades to RFC 6455 WebSocket. The server then pushes one\n"
        "text frame per fired timer (every fire of a recurring timer):\n"
        "\n"
        "    timer <id> <epoch> <message>\n"
        "\n"
        "to every connected client. No client input is required; close and\n"
        "ping frames are handled.\n"
        "\n"
        "## durability\n"
        "\n"
        "On startup exosched reloads all `exosched:timer:*` keys from\n"
        "exomind: future timers are rescheduled, overdue one-shots are logged\n"
        "as notes (`missed timer ...`) and dropped, overdue recurring timers\n"
        "catch up to their next fire (skipped fires are logged). If exomind\n"
        "is down at startup, reload is retried every second until it\n"
        "succeeds. If exomind is down at fire time, the timer is kept and\n"
        "its note/reschedule is retried every 5s until exomind answers, so\n"
        "a fire is never silently lost. Cancel deletes the key.\n"
        "\n"
        "## auth\n"
        "\n"
        "Start with `--token secret` (or env EXOSCHED_TOKEN), then send\n"
        "`Authorization: Bearer secret` on every request, including the\n"
        "WebSocket upgrade.\n"
        "\n"
        "usage: exosched [--host <addr>] [--port <n>] [--exomind URL]\n"
        "                [--token <t>] [--help] [--version]\n";
}

/* case-insensitive substring search */
static int ci_strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    size_t hl = strlen(hay);
    if (nl > hl)
        return 0;
    for (size_t i = 0; i + nl <= hl; i++)
        if (ci_prefix(hay + i, needle))
            return 1;
    return 0;
}

/* handshake for /ws, returns 0 ok / -1 not a websocket upgrade */
static int ws_handshake(const req_t *r, int fd)
{
    char *upg = hdr_value(r, "upgrade:");
    char *conn = hdr_value(r, "connection:");
    char *key = hdr_value(r, "sec-websocket-key:");
    int ok = upg && ci_strstr(upg, "websocket") &&
             conn && ci_strstr(conn, "upgrade") && key && key[0];
    free(upg);
    free(conn);
    if (!ok) {
        free(key);
        return -1;
    }
    char accept[64];
    if (ws_make_accept(key, accept, sizeof accept) != 0) {
        free(key);
        return -1;
    }
    free(key);
    char hdr[640];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n",
                     accept);
    if (n > 0)
        (void)!write(fd, hdr, (size_t)n);
    return 0;
}

static int make_timer_id(char *id, size_t cap)
{
    for (int try = 0; try < 4; try++) {
        snprintf(id, cap, "%lld:%08x", (long long)now_epoch(),
                 (unsigned)rand32());
        if (!timer_find(id))
            return 0;
    }
    return -1;
}

static void route(req_t *r, exo_t *e, buf_t *out, int *status,
                  const char **ctype)
{
    const char *path = r->path;
    char tmp[4096];

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

    if (!strcmp(path, "/remind")) {
        if (strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use POST");
            return;
        }
        if (r->body_len == 0) {
            *status = 400;
            buf_puts(out, "error: empty body");
            return;
        }
        int64_t fire = 0, repeat = 0, until = 0;
        char *msg = NULL;
        char err[256];
        if (parse_schedule(r->body, r->body_len, &fire, &repeat, &until, &msg,
                           err, sizeof err) != 0) {
            *status = 400;
            buf_printf(out, "error: %s", err);
            return;
        }
        char id[TIMER_ID_MAX];
        if (make_timer_id(id, sizeof id) != 0) {
            *status = 500;
            buf_puts(out, "error: cannot allocate timer id");
            free(msg);
            return;
        }
        char key[512];
        snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", id);
        char perr[256];
        char *value = timer_value(fire, repeat, until, msg);
        size_t vlen = strlen(value);
        if (vlen + 64 > 4096) {
            *status = 413;
            buf_puts(out, "error: message too large");
            free(value);
            free(msg);
            return;
        }
        if (exo_persist(e, key, value, timer_ttl(fire), perr, sizeof perr) != 0) {
            *status = 500;
            buf_printf(out, "error: exomind unavailable: %s", perr);
            free(value);
            free(msg);
            return;
        }
        free(value);
        if (timer_add(id, fire, repeat, until, msg) != 0) {
            *status = 500;
            buf_puts(out, "error: timer already exists");
            int existed = 0;
            (void)exo_del(e, key, &existed, perr, sizeof perr);
            free(msg);
            return;
        }
        buf_printf(out, "ok %s %lld", id, (long long)fire);
        free(msg);
        return;
    }

    if (!strcmp(path, "/timers")) {
        if (strcmp(r->method, "GET")) {
            *status = 405;
            buf_puts(out, "error: use GET");
            return;
        }
        size_t n = 0;
        timer_rec_t *snap = timers_snapshot(&n);
        int64_t now = now_epoch();
        if (qp_str(r->query, "json", tmp, sizeof tmp)) {
            *ctype = "application/json; charset=utf-8";
            buf_puts(out, "[");
            for (size_t i = 0; i < n; i++) {
                if (i)
                    buf_puts(out, ",");
                char *je = json_escape(snap[i].msg, strlen(snap[i].msg));
                buf_printf(out,
                           "{\"id\":\"%s\",\"epoch\":%lld,\"remaining_s\":%lld,"
                           "\"repeat_s\":%lld,\"until\":%lld,\"message\":\"%s\"}",
                           snap[i].id, (long long)snap[i].wall_fire,
                           (long long)(snap[i].wall_fire - now),
                           (long long)snap[i].repeat, (long long)snap[i].until,
                           je);
                free(je);
            }
            buf_puts(out, "]");
        } else {
            for (size_t i = 0; i < n; i++) {
                char *e = esc_line(snap[i].msg, strlen(snap[i].msg));
                buf_printf(out, "%s\t%lld\t%lld\t%s", snap[i].id,
                           (long long)snap[i].wall_fire,
                           (long long)(snap[i].wall_fire - now), e);
                free(e);
                if (snap[i].repeat > 0)
                    buf_printf(out, "\t%lld\t%lld", (long long)snap[i].repeat,
                               (long long)snap[i].until);
                buf_puts(out, "\n");
            }
        }
        timers_snapshot_free(snap, n);
        return;
    }

    if (!strcmp(path, "/timer")) {
        if (strcmp(r->method, "DELETE") && strcmp(r->method, "POST")) {
            *status = 405;
            buf_puts(out, "error: use DELETE");
            return;
        }
        char id[TIMER_ID_MAX];
        if (!qp_str(r->query, "id", id, sizeof id)) {
            *status = 400;
            buf_puts(out, "error: missing id");
            return;
        }
        if (!timer_cancel(id)) {
            *status = 404;
            buf_puts(out, "missing");
            return;
        }
        char key[512];
        snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", id);
        char err[256];
        int existed = 0;
        if (exo_del(e, key, &existed, err, sizeof err) != 0)
            fprintf(stderr, "exosched: cancel del failed for %s: %s\n", id,
                    err);
        buf_puts(out, "ok");
        return;
    }

    *status = 404;
    buf_puts(out, "error: unknown path");
}

int http_handle_conn(int fd, exo_t *e)
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

    if (!strcmp(r.path, "/ws")) {
        if (strcmp(r.method, "GET")) {
            send_response(fd, 405, "text/plain; charset=utf-8",
                          "error: use GET\n", 14, 0);
        } else if (ws_handshake(&r, fd) != 0) {
            send_response(fd, 400, "text/plain; charset=utf-8",
                          "error: not a websocket upgrade\n", 30, 0);
        } else {
            /* upgrade succeeded: this thread becomes the ws client */
            free(r.body);
            free(r.hdrs);
            struct timeval notv = {.tv_sec = 0, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &notv, sizeof notv);
            ws_handle_conn(fd);
            return 1; /* ws layer owns and closes the fd */
        }
        free(r.body);
        free(r.hdrs);
        return 0;
    }

    int status = 200;
    const char *ctype = "text/plain; charset=utf-8";
    buf_t out = {0};
    route(&r, e, &out, &status, &ctype);
    int head = !strcmp(r.method, "HEAD");
    send_response(fd, status, ctype, out.p ? out.p : "", out.len, head);
    buf_free(&out);
    free(r.body);
    free(r.hdrs);
    return 0;
}
