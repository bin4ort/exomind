/* exogit: HTTP transport via the curl binary (modeled on exocrawl). */
#include "exogit.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *DEFAULT_UAS[] = {
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0 Safari/537.36",
    "curl/8.5.0",
    NULL};

void net_init(net_t *n)
{
    memset(n, 0, sizeof *n);
    for (int i = 0; DEFAULT_UAS[i]; i++) {
        n->uas = (char **)realloc(n->uas, (n->nuas + 1) * sizeof(char *));
        if (!n->uas)
            return;
        n->uas[n->nuas++] = strdup(DEFAULT_UAS[i]);
    }
    n->timeout_ms = 25000;
}

void net_free(net_t *n)
{
    for (size_t i = 0; i < n->nuas; i++)
        free(n->uas[i]);
    free(n->uas);
    n->uas = NULL;
    n->nuas = 0;
}

void resp_free(resp_t *r)
{
    free(r->body);
    r->body = NULL;
    r->blen = 0;
    r->status = 0;
    r->transport_ok = 0;
}

static const char *net_next_ua(net_t *n)
{
    if (n->nuas == 0)
        return "exogit/0.1";
    size_t idx = __sync_fetch_and_add(&n->ua_idx, 1) % n->nuas;
    return n->uas[idx];
}

static int curl_get(net_t *n, const char *url, const char *ua, resp_t *r,
                    char *err, size_t errsz)
{
    char tmpl[64];
    snprintf(tmpl, sizeof tmpl, "/tmp/exogit-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        snprintf(err, errsz, "mkstemp failed");
        return -1;
    }
    close(fd);
    char codefile[80];
    snprintf(codefile, sizeof codefile, "%s.code", tmpl);

    char auth[2048];
    if (n->token && n->token[0])
        snprintf(auth, sizeof auth,
                 " -H 'Authorization: Bearer %s'", n->token);
    else
        auth[0] = 0;

    char cmd[MAX_URL + 4096];
    snprintf(cmd, sizeof cmd,
             "curl -sS -m %d --connect-timeout 10 -o '%s' -w '%%{http_code}'"
             " -A '%s' --no-keepalive -H 'Accept: application/json,"
             "text/html;text/plain' -H 'Accept-Language: en' -e ''%s",
             n->timeout_ms / 1000, tmpl, ua, auth);
    if (n->proxy && n->proxy[0])
        snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd),
                 " -x '%s'", n->proxy);
    snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " '%s' > '%s' 2>/dev/null",
             url, codefile);

    int rc = system(cmd);
    r->status = 0;
    FILE *cf = fopen(codefile, "r");
    if (cf) {
        if (fscanf(cf, "%d", &r->status) != 1)
            r->status = 0;
        fclose(cf);
    }
    unlink(codefile);

    struct stat st;
    if (stat(tmpl, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        size_t sz = st.st_size > MAX_BODY ? MAX_BODY : (size_t)st.st_size;
        r->body = (char *)malloc(sz + 1);
        if (r->body) {
            FILE *f = fopen(tmpl, "rb");
            if (f) {
                r->blen = fread(r->body, 1, sz, f);
                fclose(f);
            }
            r->body[r->blen] = 0;
        }
        r->transport_ok = 1;
    } else {
        r->transport_ok = 0;
    }
    unlink(tmpl);
    if (rc != 0 && !r->transport_ok) {
        snprintf(err, errsz, "curl transport error (rc=%d)", rc);
        return -1;
    }
    return 0;
}

int net_fetch(net_t *n, const char *url, resp_t *r, char *err, size_t errsz)
{
    memset(r, 0, sizeof *r);
    const char *ua = net_next_ua(n);
    if (curl_get(n, url, ua, r, err, errsz) != 0)
        return -1;
    if (r->status == 403 || r->status == 429 || r->status >= 500) {
        for (int attempt = 0; attempt < 2; attempt++) {
            resp_t r2;
            const char *ua2 = net_next_ua(n);
            if (curl_get(n, url, ua2, &r2, err, errsz) != 0)
                break;
            resp_free(r);
            *r = r2;
            if (r->status == 200 || r->status == 404)
                break;
            struct timespec ns = {0, 500000000L * (attempt + 1)};
            nanosleep(&ns, NULL);
        }
    }
    if (r->status >= 400) {
        snprintf(err, errsz, "http %d", r->status);
        return 0;
    }
    return 0;
}
