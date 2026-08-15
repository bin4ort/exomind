/* exoflow: the orchestrator for agent swarms. Durable state lives in
 * exomind; deadlines are scheduled on exosched. */
#include "exoflow.h"

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
    cli_t *xm;
    cli_t *xs;
} conn_args_t;

static void *conn_thread(void *arg)
{
    conn_args_t *a = arg;
    if (http_handle_conn(a->fd, a->xm, a->xs) == 0)
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
    printf("exoflow v" EXOFLOW_VERSION
           " - the orchestrator for agent swarms\n"
           "usage: %s [options]\n"
           "  --host <addr>      bind address (default 127.0.0.1)\n"
           "  --port <n>         port (default 7656)\n"
           "  --exomind <url>    storage backend (default http://127.0.0.1:7654)\n"
           "  --exosched <url>   deadline scheduler (default http://127.0.0.1:7655)\n"
           "  --token <t>        require 'Authorization: Bearer <t>'\n"
           "  --help             show this help\n"
           "  --version          show version\n"
           "env: EXOFLOW_TOKEN same as --token\n",
           argv0);
}

/* keeps retrying the startup reload until exomind answers; runs detached so
 * the daemon serves requests even while its storage backend is down */
static void *reload_thread(void *arg)
{
    cli_t *e = arg;
    for (;;) {
        sleep(1);
        if (flows_reload(e) == 0)
            break;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 7656;
    const char *exomind_url = "http://127.0.0.1:7654";
    const char *exosched_url = "http://127.0.0.1:7655";
    const char *token = getenv("EXOFLOW_TOKEN");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--host") && i + 1 < argc)
            host = argv[++i];
        else if (!strcmp(a, "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(a, "--exomind") && i + 1 < argc)
            exomind_url = argv[++i];
        else if (!strcmp(a, "--exosched") && i + 1 < argc)
            exosched_url = argv[++i];
        else if (!strcmp(a, "--token") && i + 1 < argc)
            token = argv[++i];
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("exoflow v%s\n", EXOFLOW_VERSION);
            return 0;
        } else {
            fprintf(stderr, "exoflow: unknown argument %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    cli_t xm, xs;
    char err[256];
    if (cli_init(&xm, exomind_url, err, sizeof err) != 0) {
        fprintf(stderr, "exoflow: %s\n", err);
        return 1;
    }
    if (cli_init(&xs, exosched_url, err, sizeof err) != 0) {
        fprintf(stderr, "exoflow: %s\n", err);
        return 1;
    }
    if (token)
        http_set_token(token);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        fprintf(stderr, "exoflow: socket failed: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!inet_pton(AF_INET, host, &addr.sin_addr)) {
        fprintf(stderr, "exoflow: bad host %s\n", host);
        close(lfd);
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "exoflow: cannot bind %s:%d: %s\n", host, port,
                strerror(errno));
        close(lfd);
        return 1;
    }
    if (listen(lfd, 64) < 0) {
        fprintf(stderr, "exoflow: listen failed: %s\n", strerror(errno));
        close(lfd);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int loaded = flows_reload(&xm) == 0;
    if (!loaded) {
        fprintf(stderr,
                "exoflow: exomind down at startup; serving with an empty "
                "registry and retrying reload in the background\n");
        pthread_t rt;
        if (pthread_create(&rt, NULL, reload_thread, &xm) != 0)
            fprintf(stderr, "exoflow: cannot start reload thread\n");
        else
            pthread_detach(rt);
    }

    fprintf(stderr,
            "exoflow v%s listening on http://%s:%d (exomind: %s, exosched: "
            "%s%s)\n",
            EXOFLOW_VERSION, host, port, exomind_url, exosched_url,
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
        a->xm = &xm;
        a->xs = &xs;
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

    fprintf(stderr, "exoflow: shutdown complete\n");
    return 0;
}
