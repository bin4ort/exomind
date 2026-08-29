/* exokit C runner shim for the exodoc behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * #include-ing the REAL exodoc source (audit.c, which holds all four curated
 * pure functions). word_char is also defined in util.c, so we intentionally
 * include only audit.c and stub the util.c helpers that other audit.c functions
 * reference — the curated functions never call them.
 *
 * Argument encoding: '|' separates sub-fields (never appears in valid tokens).
 *   ver_eq        a|b
 *   range_str     start end|needle|text
 *   word_char / is_ws_char   single char
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "exodoc/src/audit.c"

/* ---- stubs for exodoc util.c helpers (unused by curated fns) ---- */
void *xmalloc(size_t n) { (void)n; return NULL; }
char *xstrdup(const char *s) { (void)s; return NULL; }
void buf_put(buf_t *b, const void *d, size_t n) { (void)b;(void)d;(void)n; }
void buf_puts(buf_t *b, const char *s) { (void)b;(void)s; }
void buf_printf(buf_t *b, const char *fmt, ...) { (void)b;(void)fmt; }
void buf_free(buf_t *b) { (void)b; }
char *json_escape(const char *s, size_t n) { (void)s;(void)n; return NULL; }
size_t scan_version(const char *s, size_t n, char *out, size_t outsz)
{ (void)s;(void)n;(void)out;(void)outsz; return 0; }
int http_get(const char *host, int port, const char *path, char **body,
             size_t *blen, int *status, char *err, size_t errsz)
{ (void)host;(void)port;(void)path;(void)body;(void)blen;(void)status;(void)err;(void)errsz; return -1; }
void lc(char *s) { (void)s; }

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "ver_eq") == 0) {
        char a[256], b[256];
        if (sscanf(args, "%255[^|]|%255[^|]", a, b) == 2)
            printf("%d\n", ver_eq(a, b));
        else
            printf("error: bad args\n");
    } else if (strcmp(fn, "range_str") == 0) {
        size_t start = 0, end = 0;
        char needle[256], text[8192];
        if (sscanf(args, "%zu %zu|%255[^|]|%8191[^\n]", &start, &end, needle, text) == 4)
            printf("%d\n", range_str(text, start, end, needle));
        else
            printf("error: bad args\n");
    } else if (strcmp(fn, "word_char") == 0) {
        printf("%d\n", word_char((unsigned char)(args[0] ? args[0] : 0)));
    } else if (strcmp(fn, "is_ws_char") == 0) {
        printf("%d\n", is_ws_char(args[0] ? args[0] : 0));
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
