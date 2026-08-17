#include "http.h"
#include "store.h"
#include "util.h"
#include "version.h"

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
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static pthread_mutex_t conn_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t conn_cv = PTHREAD_COND_INITIALIZER;
static int g_active = 0;

typedef struct {
    int fd;
    store_t *s;
} conn_args_t;

static void *conn_thread(void *arg)
{
    conn_args_t *a = arg;
    http_handle_conn(a->fd, a->s);
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

static char *read_whole_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *p = NULL;
    size_t n = 0, cap = 0;
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (n + got + 1 > cap) {
            cap = (n + got + 1) * 2;
            p = xrealloc(p, cap);
        }
        memcpy(p + n, chunk, got);
        n += got;
    }
    fclose(f);
    if (p)
        p[n] = 0;
    if (len)
        *len = n;
    return p;
}

static int mkdir_p(const char *path)
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

#include "../common/exo.h"

static void usage(const char *argv0)
{
    printf("exomind v" EXOMIND_VERSION
           " - external memory for AI agents\n"
           "usage: %s [options]\n"
           "  --host <addr>    bind address (default 127.0.0.1)\n"
           "  --port <n>       port (default 7654)\n"
           "  --data <file>    data file (default exomind.dat)\n"
           "  --token <t>      require 'Authorization: Bearer <t>'\n"
           "  --tokens <file>  load extra tokens (token[:ro][:scope=<p>*])\n"
           "  --keys <file>    key file (same format as --tokens); managed\n"
           "                   with the `keys` subcommand\n"
           "  --rate-limit <n> max requests/second (429 beyond)\n"
           "  --log-level <error|warn|info|debug>\n"
           "  --mcp            serve as a stdio Model Context Protocol\n"
           "                   server (installing as `exomind-server` does\n"
           "                   the same)\n"
           "  keys add <name[:mods]> | keys list | keys remove <name>\n"
           "  --help [modules] show this guide, or the whole stack's\n"
           "  --version        show version\n"
           "env: EXOMIND_TOKEN same as --token\n"
           "the daemon serves its API at / and /exoexomind on the bound\n"
           "address; GET / is the base usage information\n",
           argv0);
}

/* whole-stack usage registry: siblings from common, own spec here */
static exo_help_t EXO_SELF[] = {
    {"exomind", NULL}, /* filled at runtime: http_help_text() */
};

static store_t *g_store = NULL;

/* MCP bridge: translate a tool call into an internal HTTP dispatch */
static int mcp_call(const char *tool, const char *args, char *out,
                    size_t cap)
{
    char method[16] = "GET";
    char path[4096];
    char query[4096] = "";
    char body[4096] = "";
    size_t blen = 0;
    char v[4096];
    if (!strcmp(tool, "ping")) {
        snprintf(path, sizeof path, "/ping");
    } else if (!strcmp(tool, "set") || !strcmp(tool, "append")) {
        snprintf(method, sizeof method, "POST");
        if (!exo_json_str(args, "key", v, sizeof v))
            return snprintf(out, cap, "error: missing key"), 1;
        char ttl[32] = "";
        exo_json_str(args, "ttl", ttl, sizeof ttl);
        snprintf(path, sizeof path, "/%s", tool);
        snprintf(query, sizeof query, "key=%s%s%s", v,
                 ttl[0] ? "&ttl=" : "", ttl);
        exo_json_str(args, "value", body, sizeof body);
        blen = strlen(body);
        if (!blen) {
            exo_json_str(args, "body", body, sizeof body);
            blen = strlen(body);
        }
    } else if (!strcmp(tool, "get")) {
        if (!exo_json_str(args, "key", v, sizeof v))
            return snprintf(out, cap, "error: missing key"), 1;
        snprintf(path, sizeof path, "/get");
        snprintf(query, sizeof query, "key=%s", v);
    } else if (!strcmp(tool, "del")) {
        if (!exo_json_str(args, "key", v, sizeof v))
            return snprintf(out, cap, "error: missing key"), 1;
        snprintf(method, sizeof method, "DELETE");
        snprintf(path, sizeof path, "/del");
        snprintf(query, sizeof query, "key=%s", v);
    } else if (!strcmp(tool, "list")) {
        snprintf(path, sizeof path, "/list");
        exo_json_str(args, "prefix", v, sizeof v);
        if (v[0])
            snprintf(query, sizeof query, "prefix=%s", v);
    } else if (!strcmp(tool, "search")) {
        if (!exo_json_str(args, "q", v, sizeof v))
            return snprintf(out, cap, "error: missing q"), 1;
        snprintf(query, sizeof query, "q=%s", v);
        snprintf(path, sizeof path, "/search");
    } else if (!strcmp(tool, "note")) {
        snprintf(method, sizeof method, "POST");
        snprintf(path, sizeof path, "/note");
        exo_json_str(args, "text", body, sizeof body);
        blen = strlen(body);
    } else if (!strcmp(tool, "notes")) {
        snprintf(path, sizeof path, "/notes");
        exo_json_str(args, "q", v, sizeof v);
        if (v[0])
            snprintf(query, sizeof query, "q=%s", v);
    } else if (!strcmp(tool, "batch")) {
        snprintf(method, sizeof method, "POST");
        snprintf(path, sizeof path, "/batch");
        exo_json_str(args, "ops", body, sizeof body);
        blen = strlen(body);
    } else if (!strcmp(tool, "stats")) {
        snprintf(path, sizeof path, "/stats");
    } else if (!strcmp(tool, "snapshot")) {
        snprintf(path, sizeof path, "/snapshot");
    } else if (!strcmp(tool, "sim")) {
        snprintf(method, sizeof method, "POST");
        snprintf(path, sizeof path, "/sim");
        exo_json_str(args, "text", body, sizeof body);
        blen = strlen(body);
    } else {
        return snprintf(out, cap, "error: no such tool %s", tool), 1;
    }
    buf_t o = {0};
    int status = 200;
    const char *ctype = "text/plain";
    http_dispatch(method, path, query, body, blen, &o, &status, &ctype,
                  g_store);
    if (status == 404)
        snprintf(out, cap, "missing");
    else
        snprintf(out, cap, "%s", o.p ? o.p : "");
    http_buf_free(&o);
    return 0;
}

static const exo_mcp_tool_t MCP_TOOLS[] = {
    {"ping", "liveness check", "{\"type\":\"object\",\"properties\":{}}"},
    {"set", "store a value under a key",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"},\"ttl\":{\"type\":\"string\"}},\"required\":[\"key\",\"value\"]}"},
    {"get", "read a stored value",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}"},
    {"append", "append to a value, newline-separated",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"key\"]}"},
    {"del", "delete a key",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}"},
    {"list", "list keys (prefix filter)",
     "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"}}}"},
    {"search", "ranked substring search over keys and values",
     "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}},\"required\":[\"q\"]}"},
    {"note", "store a timestamped note",
     "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}"},
    {"notes", "list notes newest-first",
     "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}"},
    {"batch", "run a batch of ops (JSON array)",
     "{\"type\":\"object\",\"properties\":{\"ops\":{\"type\":\"string\"}},\"required\":[\"ops\"]}"},
    {"stats", "counters and health", "{\"type\":\"object\",\"properties\":{}}"},
    {"snapshot", "full store dump", "{\"type\":\"object\",\"properties\":{}}"},
    {"sim", "nearest vectors to the given text",
     "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}"},
};

/* returns nonzero when the binary is invoked as <module>-server */
static int invoked_as_server(const char *argv0)
{
    const char *b = strrchr(argv0, '/');
    b = b ? b + 1 : argv0;
    size_t n = strlen(b);
    return n > 7 && strncmp(b + n - 7, "-server", 7) == 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 7654;
    const char *data = "exomind.dat";
    const char *token = getenv("EXOMIND_TOKEN");
    const char *tokens_file = NULL;
    int mcp_mode = 0;
    long rate_limit = 0;
    const char *backup_dir = NULL;
    const char *project_root = NULL;
    const char *mandate_text = NULL;

    EXO_SELF[0].spec = http_help_text();
    exo_help_add_siblings();
    exo_help_add(EXO_SELF, sizeof EXO_SELF / sizeof EXO_SELF[0]);

    /* console subcommands run before the daemon starts */
    if (argc >= 2 && !strcmp(argv[1], "keys")) {
        const char *keys_file = NULL;
        for (int i = 2; i < argc; i++)
            if (!strcmp(argv[i], "--keys") && i + 1 < argc)
                keys_file = argv[++i];
        char err[256];
        if (argc >= 3 && !strcmp(argv[2], "add") && argc >= 4) {
            if (exo_keys_add(keys_file, argv[3], err, sizeof err) != 0) {
                fprintf(stderr, "exomind keys add: %s\n", err);
                return 1;
            }
            printf("ok key added\n");
            return 0;
        }
        if (argc >= 3 && !strcmp(argv[2], "remove") && argc >= 4) {
            int r = exo_keys_remove(keys_file, argv[3], err, sizeof err);
            if (r < 0) {
                fprintf(stderr, "exomind keys remove: %s\n", err);
                return 1;
            }
            printf(r ? "ok key removed\n" : "missing key\n");
            return 0;
        }
        if (argc >= 3 && !strcmp(argv[2], "list")) {
            if (exo_keys_list(keys_file, err, sizeof err) != 0) {
                fprintf(stderr, "exomind keys list: %s\n", err);
                return 1;
            }
            return 0;
        }
        fprintf(stderr,
                "usage: exomind keys add <name[:mods]> --keys <file>\n"
                "       exomind keys remove <name> --keys <file>\n"
                "       exomind keys list --keys <file>\n"
                "mods: ro, scope=<prefix>* (same as --tokens lines)\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--host") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(a, "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(a, "--data") && i + 1 < argc)
            data = argv[++i];
        else if (!strcmp(a, "--token") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(a, "--tokens") && i + 1 < argc)
            tokens_file = argv[++i];
        else if (!strcmp(a, "--keys") && i + 1 < argc)
            tokens_file = argv[++i];
        else if (!strcmp(a, "--backup") && i + 1 < argc)
            backup_dir = argv[++i];
        else if (!strcmp(a, "--project-root") && i + 1 < argc)
            project_root = argv[++i];
        else if (!strcmp(a, "--mandate") && i + 1 < argc)
            mandate_text = argv[++i];
        else if (!strcmp(a, "--mandate-file") && i + 1 < argc) {
            size_t mn = 0;
            char *mt = read_whole_file(argv[++i], &mn);
            if (!mt) {
                fprintf(stderr, "exomind: cannot read mandate file %s\n",
                        argv[i]);
                return 1;
            }
            static char mbuf[8192];
            snprintf(mbuf, sizeof mbuf, "%s", mt);
            free(mt);
            mandate_text = mbuf;
        } else if (!strcmp(a, "--rate-limit") && i + 1 < argc)
            rate_limit = atol(argv[++i]);
        else if (!strcmp(a, "--log-level") && i + 1 < argc) {
            int lv = exo_parse_log_level(argv[++i]);
            if (lv < 0) {
                fprintf(stderr,
                        "exomind: bad log level (error|warn|info|debug)\n");
                return 1;
            }
            exo_set_log_level(lv);
        } else if (!strcmp(a, "--mcp")) {
            mcp_mode = 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
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
        } else if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("exomind v%s\n", EXOMIND_VERSION);
            return 0;
        } else {
            fprintf(stderr, "exomind: unknown argument %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }
    if (invoked_as_server(argv[0]))
        mcp_mode = 1;
    if (mcp_mode) {
        g_store = store_open(data);
        exo_mcp_register(MCP_TOOLS, sizeof MCP_TOOLS / sizeof MCP_TOOLS[0],
                         "exomind", EXOMIND_VERSION, mcp_call);
        return exo_mcp_stdio();
    }

    char *dpath = xstrdup(data);
    char *slash = strrchr(dpath, '/');
    if (slash) {
        *slash = 0;
        if (mkdir_p(dpath) != 0) {
            fprintf(stderr, "exomind: cannot create directory %s: %s\n", dpath,
                    strerror(errno));
            free(dpath);
            return 1;
        }
    }
    free(dpath);

    g_store = store_open(data);
    if (rate_limit > 0) {
        exo_rate_init(rate_limit);
        g_rate_limit_active = 1;
    }
    store_t *s = g_store;

    /* project memory: auto-detect the project root upward from the CWD
     * (marker: a .git dir, a .exo dir, or a docs/stack.tsv manifest),
     * overridable with --project-root. The project store lives in
     * <root>/.exo/project.dat — even when exomind runs from elsewhere. */
    if (project_root) {
        char pr[4096];
        snprintf(pr, sizeof pr, "%s", project_root);
        char prf[4096];
        snprintf(prf, sizeof prf, "%s/.exo/project.dat", pr);
        char prd[4096];
        snprintf(prd, sizeof prd, "%s/.exo", pr);
        if (mkdir_p(prd) != 0)
            fprintf(stderr, "exomind: cannot create project memory dir %s\n",
                    prd);
        store_t *proj = store_open(prf);
        http_set_project(proj, pr);
        fprintf(stderr, "exomind: project memory %s (root %s)\n", prf, pr);
    } else {
        char cwd[4096];
        char *found = NULL;
        if (getcwd(cwd, sizeof cwd)) {
            char probe[4096];
            for (char *d = cwd;;) {
                snprintf(probe, sizeof probe, "%s/.git", d);
                struct stat st;
                if (stat(probe, &st) == 0) {
                    found = d;
                    break;
                }
                snprintf(probe, sizeof probe, "%s/.exo", d);
                if (stat(probe, &st) == 0) {
                    found = d;
                    break;
                }
                char *slash = strrchr(d, '/');
                if (!slash)
                    break;
                if (slash == d)
                    break;
                *slash = 0;
            }
        }
        if (found) {
            char prf[4096];
            snprintf(prf, sizeof prf, "%s/.exo/project.dat", found);
            char prd[4096];
            snprintf(prd, sizeof prd, "%s/.exo", found);
            if (mkdir_p(prd) != 0)
                fprintf(stderr, "exomind: cannot create project memory dir %s\n",
                        prd);
            store_t *proj = store_open(prf);
            http_set_project(proj, found);
            fprintf(stderr, "exomind: project memory %s (root %s)\n", prf,
                    found);
        }
    }
    if (backup_dir) {
        http_set_backup_dir(backup_dir);
        char berr[256];
        if (exo_backup_now(s, berr, sizeof berr) != 0)
            fprintf(stderr, "exomind: initial backup failed: %s\n", berr);
        else
            fprintf(stderr, "exomind: initial backup written to %s\n",
                    backup_dir);
    }
    if (mandate_text) {
        http_set_mandate(mandate_text);
        store_set(s, "mandate", 7, mandate_text, strlen(mandate_text), 0, 0);
        store_sync(s);
        fprintf(stderr, "exomind: mandate stored (%zu bytes)\n",
                strlen(mandate_text));
    }
    if (token)
        http_set_token(token);
    if (tokens_file) {
        int n = http_load_tokens(tokens_file);
        if (n < 0) {
            store_close(s);
            return 1;
        }
        fprintf(stderr, "exomind: loaded %d scoped token(s) from %s\n", n,
                tokens_file);
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        fprintf(stderr, "exomind: socket failed: %s\n", strerror(errno));
        store_close(s);
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!inet_pton(AF_INET, host, &addr.sin_addr)) {
        fprintf(stderr, "exomind: bad host %s\n", host);
        close(lfd);
        store_close(s);
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "exomind: cannot bind %s:%d: %s\n", host, port,
                strerror(errno));
        close(lfd);
        store_close(s);
        return 1;
    }
    if (listen(lfd, 64) < 0) {
        fprintf(stderr, "exomind: listen failed: %s\n", strerror(errno));
        close(lfd);
        store_close(s);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "exomind v%s listening on http://%s:%d (data: %s%s%s)\n",
            EXOMIND_VERSION, host, port, data,
            token ? ", auth: bearer token" : "",
            tokens_file ? ", scoped tokens: yes" : "");

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
        a->s = s;
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

    store_close(s);
    fprintf(stderr, "exomind: shutdown complete\n");
    return 0;
}
