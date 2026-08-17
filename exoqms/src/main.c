/* exoqms: the Quality Management System for the AI-native stack.
 * Durable state lives in exomind; the audit program runs exodoc and
 * exoqms-ui as child processes. */
#include "exoqms.h"
#include "../../common/exo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static pthread_mutex_t conn_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t conn_cv = PTHREAD_COND_INITIALIZER;
static int g_active = 0;

typedef struct {
    int fd;
    exo_t *e;
    cfg_t *cfg;
    qms_t *q;
} conn_args_t;

static void *conn_thread(void *arg)
{
    conn_args_t *a = arg;
    http_handle_conn(a->fd, a->e, a->cfg, a->q);
    close(a->fd);
    free(a);
    pthread_mutex_lock(&conn_mu);
    g_active--;
    if (g_active == 0)
        pthread_cond_broadcast(&conn_cv);
    pthread_mutex_unlock(&conn_mu);
    return NULL;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    printf("exoqms v" EXOQMS_VERSION
           " - the Quality Management System for the AI-native stack\n"
           "usage: %s [options]\n"
           "       %s /operation [options]  one-shot console op, no server\n"
           "  --serve          the only way to run the HTTP server\n"
           "                   (with --host/--port); without it the binary\n"
           "                   never binds a port\n"
           "  --body <text>    request body for the operation, instead\n"
           "                   of reading stdin\n"
           "  <operation>      run ONE API operation in-process and print\n"
           "                   the response body: /objectives (list, or\n"
           "                   POST a body title<TAB>metric<TAB>target to\n"
           "                   add), /nc (list, POST a body to raise, or\n"
           "                   /nc?id=<id>&action=<a> to transition),\n"
           "                   /audit (POST a body name<TAB>criteria, or\n"
           "                   /audit?criteria=<a,b,c> to run a program;\n"
           "                   /audit?id=<id> prints a report), /audits,\n"
           "                   /issues, /report, /trends, /ping\n"
           "  --host <addr>      bind address (default 127.0.0.1)\n"
           "  --port <n>         port (default 7657)\n"
           "  --exomind <url>    storage backend (default http://127.0.0.1:7654)\n"
           "  --exosched <url>   scheduler health probe (default http://127.0.0.1:7655)\n"
            "  --exodoc <path>    exodoc binary for doc-compliance (default exodoc)\n"
            "  --ui <path>        exoqms-ui binary for ui-audit (default none)\n"
            "  --code <path>      exoqms-code binary for code-safety (default none)\n"
            "  --svg <path>       exoqms-svg binary for asset-logic (default none)\n"
            "  --rules <dir>      rule files for debt/hygiene/secrets\n"
            "                     (default: rules/ next to --code, else\n"
            "                     <repo>/exoqms/code/rules)\n"
           "  --repo <dir>       stack repo root for docs/stack.tsv (default .)\n"
           "  --agents <a,b,c>   active agent ids for the dogfood check\n"
           "  --notes24h <n>     min notes in last 24h for the dogfood check\n"
           "  --token <t>        require 'Authorization: Bearer <t>'\n"
           "  --help             show this help\n"
           "  --version          show version\n"
           "env: EXOQMS_TOKEN same as --token\n",
           argv0, argv0);
}

/* keeps retrying the startup reload until exomind answers; runs detached so
 * the daemon serves requests even while its storage backend is down */
static void *reload_thread(void *arg)
{
    struct {
        qms_t *q;
        exo_t *e;
        char **agents;
        int *notes24h;
    } *a = arg;
    for (;;) {
        sleep(1);
        char err[256];
        if (qms_reload(a->q, a->e, a->agents, a->notes24h, err, sizeof err) == 0)
            break;
    }
    return NULL;
}

/* the one-shot console operations: the same paths GET / serves on the
 * daemon. Exit codes: 0 ok, 1 the operation failed (error response),
 * 2 usage (no such operation). */
static int known_console_op(const char *path)
{
    static const char *ops[] = {"/",          "/help",     "/spec",
                                "/ping",      "/objectives", "/nc",
                                "/audit",     "/audits",     "/issues",
                                "/report",    "/trends"};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (!strcmp(ops[i], path))
            return 1;
    return 0;
}

static int qp_get(const char *qs, const char *name, char *out, size_t cap)
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

/* request body: --body wins; else stdin, but only when it is not a
 * terminal (a TTY would block waiting for EOF). Returns the byte count. */
static size_t console_body(const char *body_arg, char *body, size_t cap)
{
    size_t blen = 0;
    if (body_arg[0]) {
        snprintf(body, cap, "%s", body_arg);
        return strlen(body);
    }
    if (isatty(0))
        return 0;
    ssize_t n;
    while (blen < cap - 1 &&
           (n = read(0, body + blen, cap - 1 - blen)) > 0)
        blen += (size_t)n;
    body[blen] = 0;
    /* a pipe line usually carries a trailing newline; strip the line
     * terminator so the last TSV field is clean (curl -d / --body are
     * passed through verbatim) */
    while (blen > 0 && (body[blen - 1] == '\n' || body[blen - 1] == '\r'))
        blen--;
    body[blen] = 0;
    return blen;
}

/* console-mode operation: run one exact API operation in-process
 * (`exoqms /objectives`, `exoqms /audit?criteria=metrics`), print the
 * response body, exit 0/1/2. No HTTP socket is ever opened in this mode. */
static int console_run(const char *path, const char *body_arg,
                       exo_t *e, cfg_t *cfg, qms_t *q)
{
    char pathbuf[2048];
    char query[8192] = "";
    snprintf(pathbuf, sizeof pathbuf, "%s", path);
    char *qmark = strchr(pathbuf, '?');
    if (qmark) {
        *qmark = 0;
        snprintf(query, sizeof query, "%s", qmark + 1);
    }
    /* modules are reachable at /exoexoqms as well as at / */
    if (!strncmp(pathbuf, "/exoexoqms", 10) &&
        (pathbuf[10] == 0 || pathbuf[10] == '/')) {
        memmove(pathbuf, pathbuf + 10, strlen(pathbuf + 10) + 1);
        if (!pathbuf[0])
            snprintf(pathbuf, sizeof pathbuf, "/");
    }
    if (!known_console_op(pathbuf)) {
        fprintf(stderr, "exoqms: no such operation '%s' (run exoqms with "
                        "no arguments for the guide)\n", pathbuf);
        return 2;
    }
    /* method map from the route() checks: /objectives, /nc and /audit
     * take POST (create / transition / run an audit program); every op
     * falls back to GET (list / detail) when no body is supplied */
    const char *method = "GET";
    char body[65536] = "";
    size_t blen = 0;
    int postable = !strcmp(pathbuf, "/objectives") || !strcmp(pathbuf, "/nc") ||
                   !strcmp(pathbuf, "/audit");
    if (postable) {
        blen = console_body(body_arg, body, sizeof body);
        if (blen)
            method = "POST";
    }
    /* console-friendly audit run: the criteria in the query string
     * instead of a body — exoqms /audit?criteria=metrics */
    if (postable && !blen && !strcmp(pathbuf, "/audit")) {
        char crit[512];
        if (qp_get(query, "criteria", crit, sizeof crit) && crit[0]) {
            char name[128];
            if (!(qp_get(query, "name", name, sizeof name) && name[0]))
                snprintf(name, sizeof name, "console");
            blen = (size_t)snprintf(body, sizeof body, "%s\t%s", name, crit);
            method = "POST";
        }
    }
    buf_t out = {0};
    int status = 200;
    const char *ctype = "text/plain";
    http_dispatch(method, pathbuf, query, body, blen, &out, &status,
                  &ctype, e, cfg, q);
    if (status >= 400) {
        fprintf(stderr, "exoqms: %s failed (%d)\n%s", pathbuf, status,
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
    const char *host = "127.0.0.1";
    int port = 7657;
    const char *exomind_url = "http://127.0.0.1:7654";
    const char *token = getenv("EXOQMS_TOKEN");
    cfg_t cfg;
    cfg_defaults(&cfg);
    char *agents = NULL;
    int want_server = 0; /* --serve or an explicit --port requested */
    const char *body_arg = "";

    /* a leading /operation is a one-shot console op: run it directly */
    const char *console_path = (argc >= 2 && argv[1][0] == '/') ? argv[1]
                                                                : NULL;

    for (int i = console_path ? 2 : 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--host") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(a, "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
            want_server = 1;
        } else if (!strcmp(a, "--serve"))
            want_server = 1;
        else if (!strcmp(a, "--exomind") && i + 1 < argc)
            exomind_url = argv[++i];
        else if (!strcmp(a, "--body") && i + 1 < argc)
            body_arg = argv[++i];
        else if (!strcmp(a, "--exosched") && i + 1 < argc)
            snprintf(cfg.exosched_url, sizeof cfg.exosched_url, "%s",
                     argv[++i]);
        else if (!strcmp(a, "--exodoc") && i + 1 < argc)
            snprintf(cfg.exodoc_path, sizeof cfg.exodoc_path, "%s",
                     argv[++i]);
        else if (!strcmp(a, "--ui") && i + 1 < argc)
            snprintf(cfg.ui_path, sizeof cfg.ui_path, "%s", argv[++i]);
        else if (!strcmp(a, "--code") && i + 1 < argc)
            snprintf(cfg.code_path, sizeof cfg.code_path, "%s", argv[++i]);
        else if (!strcmp(a, "--kit") && i + 1 < argc)
            snprintf(cfg.kit_path, sizeof cfg.kit_path, "%s", argv[++i]);
        else if (!strcmp(a, "--svg") && i + 1 < argc)
            snprintf(cfg.svg_path, sizeof cfg.svg_path, "%s", argv[++i]);
        else if (!strcmp(a, "--rules") && i + 1 < argc)
            snprintf(cfg.rules_path, sizeof cfg.rules_path, "%s", argv[++i]);
        else if (!strcmp(a, "--rate-limit") && i + 1 < argc) {
            exo_rate_init(atol(argv[++i]));
            g_rate_limit_active = 1;
        } else if (!strcmp(a, "--log-level") && i + 1 < argc) {
            int lv = exo_parse_log_level(argv[++i]);
            if (lv < 0) {
                fprintf(stderr,
                        "exoqms: bad log level (error|warn|info|debug)\n");
                return 1;
            }
            exo_set_log_level(lv);
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            static exo_help_t self[1];
            self[0].name = "exoqms";
            self[0].spec = http_spec_text();
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
        } else if (!strcmp(a, "--repo") && i + 1 < argc)
            snprintf(cfg.repo, sizeof cfg.repo, "%s", argv[++i]);
        else if (!strcmp(a, "--agents") && i + 1 < argc) {
            snprintf(cfg.agents, sizeof cfg.agents, "%s", argv[++i]);
            agents = xstrdup(cfg.agents);
        } else if (!strcmp(a, "--notes24h") && i + 1 < argc)
            cfg.notes24h = atoi(argv[++i]);
        else if (!strcmp(a, "--token") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("exoqms v%s\n", EXOQMS_VERSION);
            return 0;
        } else {
            fprintf(stderr, "exoqms: unknown argument %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    exo_t exo;
    char err[256];
    if (exo_init(&exo, exomind_url, err, sizeof err) != 0) {
        fprintf(stderr, "exoqms: %s\n", err);
        return 1;
    }
    if (token)
        http_set_token(token);

    /* universal project config: <repo>/.exoqms.json */
    pcfg_defaults(&cfg.pcfg);
    int pcrc = pcfg_load(&cfg.pcfg, cfg.repo);
    if (pcrc == 0)
        fprintf(stderr, "exoqms: loaded project config %s/.exoqms.json "
                        "(debt threshold %d)\n", cfg.repo,
                        cfg.pcfg.debt_threshold);
    else if (pcrc < 0)
        fprintf(stderr, "exoqms: warning: malformed project config; using "
                        "defaults\n");
    else
        fprintf(stderr, "exoqms: no %s/.exoqms.json; universal checks use "
                        "defaults\n", cfg.repo);

    if (!want_server) {
        /* no HTTP listener except in server mode: run the operation
         * in-process, or show the guide (the same text GET / serves) */
        if (console_path) {
            qms_t q;
            qms_init(&q);
            if (!agents)
                agents = xstrdup(cfg.agents);
            int loaded = 0;
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000L};
            for (int attempt = 0; attempt < 5; attempt++) {
                if (qms_reload(&q, &exo, &agents, &cfg.notes24h, err,
                               sizeof err) == 0) {
                    loaded = 1;
                    break;
                }
                nanosleep(&pause, NULL);
            }
            if (!loaded)
                fprintf(stderr, "exoqms: exomind down at startup; running "
                                "the operation with an empty registry\n");
            int rc = console_run(console_path, body_arg, &exo, &cfg, &q);
            qms_free(&q);
            free(agents);
            pcfg_free(&cfg.pcfg);
            return rc;
        }
        printf("%s", http_spec_text());
        return 0;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        fprintf(stderr, "exoqms: socket failed: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!inet_pton(AF_INET, host, &addr.sin_addr)) {
        fprintf(stderr, "exoqms: bad host %s\n", host);
        close(lfd);
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "exoqms: cannot bind %s:%d: %s\n", host, port,
                strerror(errno));
        close(lfd);
        return 1;
    }
    if (listen(lfd, 64) < 0) {
        fprintf(stderr, "exoqms: listen failed: %s\n", strerror(errno));
        close(lfd);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    qms_t q;
    qms_init(&q);
    if (!agents)
        agents = xstrdup(cfg.agents);
    int loaded = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (qms_reload(&q, &exo, &agents, &cfg.notes24h, err, sizeof err) == 0) {
            loaded = 1;
            break;
        }
        sleep(1);
    }
    if (!loaded) {
        fprintf(stderr,
                "exoqms: exomind down at startup; serving with an empty "
                "registry and retrying reload in the background\n");
        pthread_t rt;
        struct reload_args {
            qms_t *q;
            exo_t *e;
            char **agents;
            int *notes24h;
        };
        struct reload_args *ra = xmalloc(sizeof *ra);
        ra->q = &q;
        ra->e = &exo;
        ra->agents = &agents;
        ra->notes24h = &cfg.notes24h;
        if (pthread_create(&rt, NULL, reload_thread, ra) != 0)
            fprintf(stderr, "exoqms: cannot start reload thread\n");
        else
            pthread_detach(rt);
    }

    fprintf(stderr,
            "exoqms v%s listening on http://%s:%d (exomind: %s%s)\n",
            EXOQMS_VERSION, host, port, exomind_url,
            token ? ", auth: bearer token" : "");

    while (!g_stop) {
        struct pollfd pfd = {.fd = lfd, .events = POLLIN};
        if (poll(&pfd, 1, 300) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!(pfd.revents & POLLIN))
            continue;
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            if (g_stop)
                break;
            continue;
        }
        conn_args_t *a = xmalloc(sizeof *a);
        a->fd = cfd;
        a->e = &exo;
        a->cfg = &cfg;
        a->q = &q;
        pthread_mutex_lock(&conn_mu);
        g_active++;
        pthread_mutex_unlock(&conn_mu);
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, a) != 0) {
            close(cfd);
            free(a);
            pthread_mutex_lock(&conn_mu);
            g_active--;
            pthread_mutex_unlock(&conn_mu);
        } else {
            pthread_detach(t);
        }
    }

    close(lfd);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&conn_mu);
    while (g_active > 0) {
        if (pthread_cond_timedwait(&conn_cv, &conn_mu, &deadline) != 0)
            break;
    }
    pthread_mutex_unlock(&conn_mu);
    qms_free(&q);
    pcfg_free(&cfg.pcfg);
    free(agents);

    fprintf(stderr, "exoqms: shutdown complete\n");
    return 0;
}
