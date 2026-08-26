/* exogit: GitHub/GitLab code-research daemon — main, dispatch, console + server. */
#include "exogit.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_QUERY 8192

/* ---------------- spec (GET / and the no-arg guide) ---------------- */

const char *exogit_spec(void)
{
    return
"exogit v" EXO_VERSION " - AI-native code-research daemon (GitHub / GitLab)\n"
"plain text, lowercase ok/error replies, token-efficient\n"
"GET / - this specification\n\n"
"GET /search?q=<terms>[&lang=c][&n=10]\n"
"  find code on GitHub. With EXOGIT_TOKEN: clean API search (full_name,\n"
"  path, html_url). Without: best-effort web scrape of /blob/ links +\n"
"  result snippets. Narrow the prompt, then read the real implementations.\n\n"
"GET /file?url=<raw-or-repo-url>  (or repo=<o/r>&path=<file>&branch=)\n"
"  fetch ONE source file. Line-numbered, truncated at max= bytes (default\n"
"  60000). Add linenos=0 to drop line numbers. Token-efficient by default.\n\n"
"GET /repo?repo=<o/r>[&branch=main][&path=dir]\n"
"  list a repository (or subdirectory) tree: name + type.\n\n"
"GET /commits?repo=<o/r>&path=<file>[&branch=main][&n=10]\n"
"  THE DESIGN-EVOLUTION EXTRACTOR. Returns the commit history of ONE file,\n"
"  each commit with its message and the diff STRIPPED to only the changed\n"
"  lines (context dropped). Lets an agent read how a feature evolved from\n"
"  'introduced' to 'working' without wading through unchanged code.\n\n"
"console: exogit /search?q=... | /file?... | /repo?... | /commits?...\n"
"  one op, one process, no port bound (body on --body or stdin).\n"
"server: only with --serve (or --port <n>); opt-in.\n\n"
"auth: EXOGIT_TOKEN (GitHub/GitLab PAT) raises API rate limits and enables\n"
"structured /search. Set it via env or --token. No cookies, no JS, no accts.\n";
}

/* ---------------- dout ---------------- */

void dout_init(dout_t *d, size_t cap)
{
    memset(d, 0, sizeof *d);
    d->status = 200;
    d->ctype = "text/plain";
    buf_init(&d->body, cap);
}

void dout_free(dout_t *d)
{
    buf_free(&d->body);
}

/* ---------------- dispatch ---------------- */

/* offline parser self-test (no network). Returns 0 if all pass. */
static int selftest(dout_t *d)
{
    int fails = 0;
    char host[256], owner[256], repo[256], branch[256], path[256];
    int is_gl = 0;
    #define CK(cond, msg) do { if (!(cond)) { fails++; \
        buf_printf(&d->body, "FAIL: %s\n", msg); } } while (0)

    parse_repo_ref("https://github.com/octocat/Hello-World/blob/main/README.md",
                   host, sizeof host, owner, sizeof owner, repo, sizeof repo,
                   branch, sizeof branch, path, sizeof path, &is_gl);
    CK(strcmp(owner, "octocat") == 0, "gh owner");
    CK(strcmp(repo, "Hello-World") == 0, "gh repo");
    CK(strcmp(branch, "main") == 0, "gh branch");
    CK(strcmp(path, "README.md") == 0, "gh path");
    CK(is_gl == 0, "gh is_gitlab");

    parse_repo_ref("torvalds/linux", host, sizeof host, owner, sizeof owner,
                   repo, sizeof repo, branch, sizeof branch, path,
                   sizeof path, &is_gl);
    CK(strcmp(owner, "torvalds") == 0, "shorthand owner");
    CK(strcmp(repo, "linux") == 0, "shorthand repo");

    parse_repo_ref("https://gitlab.com/gnu/emacs/-/blob/master/lisp/subr.el",
                   host, sizeof host, owner, sizeof owner, repo, sizeof repo,
                   branch, sizeof branch, path, sizeof path, &is_gl);
    CK(strcmp(owner, "gnu") == 0, "gl owner");
    CK(strcmp(repo, "emacs") == 0, "gl repo");
    CK(strcmp(branch, "master") == 0, "gl branch");
    CK(strcmp(path, "lisp/subr.el") == 0, "gl path");
    CK(is_gl == 1, "gl is_gitlab");

    const char *doc = "[{\"sha\":\"abc\",\"commit\":{\"message\":\"fix it\"}},"
                      "{\"sha\":\"def\",\"commit\":{\"message\":\"add x\"}}]";
    size_t cur = 0, s = 0, e = 0, cnt = 0;
    char sha[64], msg[128];
    while (json_array_object(doc, &cur, &s, &e)) {
        cnt++;
        if (cnt == 1) {
            json_str_in(doc, s, e, "sha", sha, sizeof sha);
            CK(strcmp(sha, "abc") == 0, "json sha");
            size_t cs, ce;
            if (json_find_slice(doc, s, e, "commit", &cs, &ce))
                json_str_in(doc, cs, ce, "message", msg, sizeof msg);
            CK(strcmp(msg, "fix it") == 0, "json nested message");
        }
    }
    CK(cnt == 2, "json two objects");

    #undef CK
    if (fails == 0)
        buf_puts(&d->body, "selftest: ok\n");
    return fails;
}

static void dispatch(const char *path, const char *query, net_t *net,
                     dout_t *d)
{
    if (!strncmp(path, "/exoexogit", 10) && (path[10] == 0 || path[10] == '/'))
        path += 10;
    if (strcmp(path, "/") == 0)
        buf_puts(&d->body, exogit_spec());
    else if (strcmp(path, "/ping") == 0)
        buf_puts(&d->body, "pong\n");
    else if (strcmp(path, "/selftest") == 0) {
        if (selftest(d) != 0)
            d->status = 500;
    }
    else if (strcmp(path, "/search") == 0)
        exogit_search(d, net, query);
    else if (strcmp(path, "/file") == 0)
        exogit_file(d, net, query);
    else if (strcmp(path, "/repo") == 0)
        exogit_repo(d, net, query);
    else if (strcmp(path, "/commits") == 0)
        exogit_commits(d, net, query);
    else {
        d->status = 404;
        buf_puts(&d->body, "error: unknown path\n");
    }
}

/* ---------------- console mode ---------------- */

static int console_run(const char *path_arg, const char *body_arg, net_t *net)
{
    (void)body_arg;
    char pathbuf[512];
    char query[8192] = "";
    snprintf(pathbuf, sizeof pathbuf, "%s", path_arg);
    char *q = strchr(pathbuf, '?');
    if (q) {
        *q = 0;
        snprintf(query, sizeof query, "%s", q + 1);
    }
    if (!strncmp(pathbuf, "/exoexogit", 10) &&
        (pathbuf[10] == 0 || pathbuf[10] == '/')) {
        memmove(pathbuf, pathbuf + 10, strlen(pathbuf + 10) + 1);
        if (!pathbuf[0])
            snprintf(pathbuf, sizeof pathbuf, "/");
    }
    dout_t d;
    dout_init(&d, 1u << 20);
    dispatch(pathbuf, query, net, &d);
    if (d.status == 404) {
        fprintf(stderr, "exogit: unknown operation %s\n", path_arg);
        dout_free(&d);
        return 2;
    }
    if (d.status >= 400) {
        fprintf(stderr, "exogit: %s failed (%d)\n%s", pathbuf, d.status,
                d.body.p ? d.body.p : "");
        dout_free(&d);
        return 1;
    }
    fputs(d.body.p ? d.body.p : "", stdout);
    dout_free(&d);
    return 0;
}

/* ---------------- tiny HTTP server ---------------- */

static void send_all(int fd, const char *s, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) break;
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

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[65536];
    size_t n = 0;
    for (;;) {
        ssize_t r = recv(fd, buf + n, sizeof buf - n - 1, 0);
        if (r <= 0) break;
        n += (size_t)r;
        if (n + 1 >= sizeof buf || strstr(buf, "\r\n\r\n") ||
            (n > 4 && strstr(buf, "\n\n")))
            break;
    }
    buf[n] = 0;
    char method[16] = {0}, path[2048] = {0}, query[8192] = {0};
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end) { hdr_end = strstr(buf, "\n\n"); if (hdr_end) hdr_end += 1; }
    if (hdr_end) *hdr_end = 0;
    sscanf(buf, "%15s %2047s", method, path);
    char *q = strchr(path, '?');
    if (q) { snprintf(query, sizeof query, "%s", q + 1); *q = 0; }
    net_t net; net_init(&net);
    const char *tok = getenv("EXOGIT_TOKEN");
    net.token = tok && tok[0] ? tok : NULL;
    const char *px = getenv("EXOGIT_PROXY");
    net.proxy = px && px[0] ? px : NULL;
    dout_t d; dout_init(&d, 1u << 20);
    dispatch(path, query, &net, &d);
    http_out(fd, d.status, d.ctype, d.body.p ? d.body.p : "");
    dout_free(&d); net_free(&net);
    close(fd);
    return NULL;
}

/* ---------------- usage ---------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--serve] [--port 7660] [--token <pat>]\n"
            "       %s /search?q=... | /file?... | /repo?... | /commits?...\n"
            "       set EXOGIT_TOKEN for authenticated GitHub/GitLab API.\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    int port = 7660;
    int want_server = 0;
    const char *console_path = (argc >= 2 && argv[1][0] == '/') ? argv[1]
                                                                 : NULL;
    const char *token = getenv("EXOGIT_TOKEN");
    const char *proxy = getenv("EXOGIT_PROXY");
    const char *body_arg = "";
    for (int i = console_path ? 2 : 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--port") && i + 1 < argc) {
            port = atoi(argv[++i]); want_server = 1;
        } else if (!strcmp(a, "--serve")) want_server = 1;
        else if (!strcmp(a, "--token") && i + 1 < argc) token = argv[++i];
        else if (!strcmp(a, "--proxy") && i + 1 < argc) proxy = argv[++i];
        else if (!strcmp(a, "--body") && i + 1 < argc) body_arg = argv[++i];
        else if (!strcmp(a, "--version")) {
            printf("exogit v%s\n", EXO_VERSION); return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            printf("\n%s", exogit_spec());
            return 0;
        } else if (a[0] == '/' && !console_path) {
            console_path = a;
        } else {
            fprintf(stderr, "exogit: unknown argument %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    net_t net;
    net_init(&net);
    net.token = token && token[0] ? token : NULL;
    net.proxy = proxy && proxy[0] ? proxy : NULL;

    if (!want_server) {
        if (console_path) {
            int rc = console_run(console_path, body_arg, &net);
            net_free(&net);
            return rc;
        }
        printf("%s", exogit_spec());
        net_free(&net);
        return 0;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { fprintf(stderr, "exogit: socket: %s\n", strerror(errno)); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "exogit: bind %d: %s\n", port, strerror(errno));
        return 1;
    }
    listen(srv, 64);
    fprintf(stderr, "exogit v%s on %d\n", EXO_VERSION, port);
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pthread_t t;
        pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
    net_free(&net);
    return 0;
}
