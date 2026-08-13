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

static void usage(const char *argv0)
{
    printf("exomind v" EXOMIND_VERSION
           " - external memory for AI agents\n"
           "usage: %s [options]\n"
           "  --host <addr>    bind address (default 127.0.0.1)\n"
           "  --port <n>       port (default 7654)\n"
           "  --data <file>    data file (default exomind.dat)\n"
           "  --token <t>      require 'Authorization: Bearer <t>'\n"
           "  --help           show this help\n"
           "  --version        show version\n"
           "env: EXOMIND_TOKEN same as --token\n",
           argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int port = 7654;
    const char *data = "exomind.dat";
    const char *token = getenv("EXOMIND_TOKEN");

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
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
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

    store_t *s = store_open(data);
    if (token)
        http_set_token(token);

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

    fprintf(stderr, "exomind v%s listening on http://%s:%d (data: %s%s)\n",
            EXOMIND_VERSION, host, port, data,
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
