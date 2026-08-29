/* exokit C runner shim for the exocrawl behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * #include-ing the REAL exocrawl sources for the curated pure functions:
 *   - resolve_url  (html.c)
 *   - strip_html   (search.c)
 * url_authority lives in main.c (which defines main()), so it cannot be
 * #included into a runner; it is intentionally excluded. The network calls
 * that other (unused) search.c functions make (net_fetch/pace_wait/resp_free)
 * are stubbed — never reached by the curated functions.
 *
 * Argument encoding: '|' separates base/href for resolve_url.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "exocrawl/src/html.c"
#include "exocrawl/src/util.c"
#include "exocrawl/src/search.c"

/* ---- stubs for exocrawl network deps (unused by curated fns) ---- */
int net_fetch(net_t *n, const char *url, resp_t *r, char *err, size_t errsz)
{ (void)n;(void)url;(void)r;(void)err;(void)errsz; return -1; }
void resp_free(resp_t *r) { (void)r; }
int64_t pace_wait(const char *host, int min_ms) { (void)host;(void)min_ms; return 0; }

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "resolve_url") == 0) {
        char base[8192], href[8192], out[8192];
        char *bar = strchr(args, '|');
        if (bar) {
            size_t bl = (size_t)(bar - args);
            if (bl < sizeof base) {
                memcpy(base, args, bl);
                base[bl] = 0;
                snprintf(href, sizeof href, "%s", bar + 1);
            } else {
                printf("error: bad args\n");
                return;
            }
        } else {
            snprintf(base, sizeof base, "%s", args);
            href[0] = 0;
        }
        resolve_url(base, href, out, sizeof out);
        printf("%s\n", out);
    } else if (strcmp(fn, "strip_html") == 0) {
        char buf[8192];
        snprintf(buf, sizeof buf, "%s", args);
        strip_html(buf);
        printf("%s\n", buf);
    } else {
        printf("error: no such fn %s\n", fn);
    }
}

int main(void)
{
    char line[16384];
    while (fgets(line, sizeof line, stdin)) {
        char *tab = strchr(line, '\t');
        if (!tab) { printf("error: malformed line\n"); fflush(stdout); continue; }
        *tab = 0;
        char *args = tab + 1;
        char *nl = strchr(args, '\n');
        if (nl) *nl = 0;
        dispatch(line, args);
        fflush(stdout);
    }
    return 0;
}
