/* exoqms: the Quality Management System for the AI-native stack.
 * Durable state lives in exomind; the audit program runs exodoc and
 * exoqms-ui as child processes. */
#include "exoqms.h"

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
           "  --host <addr>      bind address (default 127.0.0.1)\n"
           "  --port <n>         port (default 7657)\n"
           "  --exomind <url>    storage backend (default http://127.0.0.1:7654)\n"
           "  --exosched <url>   scheduler health probe (default http://127.0.0.1:7655)\n"
            "  --exodoc <path>    exodoc binary for doc-compliance (default exodoc)\n"
            "  --ui <path>        exoqms-ui binary for ui-audit (default none)\n"
            "  --code <path>      exoqms-code binary for code-safety (default none)\n"
            "  --svg <path>       exoqms-svg binary for asset-logic (default none)\n"
           "  --repo <dir>       stack repo root for docs/stack.tsv (default .)\n"
           "  --agents <a,b,c>   active agent ids for the dogfood check\n"
           "  --notes24h <n>     min notes in last 24h for the dogfood check\n"
           "  --token <t>        require 'Authorization: Bearer <t>'\n"
           "  --help             show this help\n"
           "  --version          show version\n"
           "env: EXOQMS_TOKEN same as --token\n",
           argv0);
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

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 7657;
    const char *exomind_url = "http://127.0.0.1:7654";
    const char *token = getenv("EXOQMS_TOKEN");
    cfg_t cfg;
    cfg_defaults(&cfg);
    char *agents = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--host") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(a, "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(a, "--exomind") && i + 1 < argc)
            exomind_url = argv[++i];
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
        else if (!strcmp(a, "--svg") && i + 1 < argc)
            snprintf(cfg.svg_path, sizeof cfg.svg_path, "%s", argv[++i]);
        else if (!strcmp(a, "--repo") && i + 1 < argc)
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
    free(agents);

    fprintf(stderr, "exoqms: shutdown complete\n");
    return 0;
}
