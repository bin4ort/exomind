/*
 * exokit C runner shim for the exogit behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * `#include`ing the REAL exogit sources (github.c/json.c/util.c/net.c), so
 * the static pure functions are dispatched to their actual implementations.
 *
 * Protocol: read one line `fn<TAB>args` on stdin, print one result line.
 * Multi-line I/O uses the exokit escape convention: the runner decodes
 * literal `\n` / `\t` in `args` to real bytes, and re-escapes real
 * newlines/tabs in the output so the ledger stays single-line TSV.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include "exogit/src/github.c"
#include "exogit/src/json.c"
#include "exogit/src/util.c"
#include "exogit/src/net.c"

/* decode literal "\n"/"\t" (two chars) in place -> real bytes */
static void unescape(char *s)
{
    if (!s) return;
    size_t w = 0;
    for (size_t r = 0; s[r]; r++) {
        if (s[r] == '\\' && s[r + 1] == 'n') { s[w++] = '\n'; r++; }
        else if (s[r] == '\\' && s[r + 1] == 't') { s[w++] = '\t'; r++; }
        else s[w++] = s[r];
    }
    s[w] = 0;
}

/* print a buffer, re-escaping real newlines/tabs as \n / \t */
static void esc_print(const char *s)
{
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n') fputs("\\n", stdout);
        else if (s[i] == '\t') fputs("\\t", stdout);
        else fputc(s[i], stdout);
    }
    fputc('\n', stdout);
}

static void dispatch(const char *fn, char *args)
{
    if (strcmp(fn, "qp") == 0) {
        char *bar = strchr(args, '|');
        if (!bar) { printf("error: need name|query\n"); return; }
        *bar = 0;
        char *name = args, *query = bar + 1;
        unescape(query);
        char out[4096];
        if (qp(query, name, out, sizeof out)) printf("%s\n", out);
        else printf("error: not found\n");
    } else if (strcmp(fn, "url_encode") == 0) {
        unescape(args);
        char out[8192];
        url_encode(args, out, sizeof out);
        printf("%s\n", out);
    } else if (strcmp(fn, "url_encode_slash") == 0) {
        unescape(args);
        char out[8192];
        url_encode_slash(args, out, sizeof out);
        printf("%s\n", out);
    } else if (strcmp(fn, "parse_repo_ref") == 0) {
        unescape(args);
        char h[256], o[256], r[256], b[256], p[256];
        int gl = 0;
        if (parse_repo_ref(args, h, sizeof h, o, sizeof o, r, sizeof r,
                           b, sizeof b, p, sizeof p, &gl))
            printf("%s|%s|%s|%s|%s|%d\n", h, o, r, b, p, gl);
        else printf("error: not found\n");
    } else if (strcmp(fn, "json_str") == 0) {
        char *bar = strchr(args, '|');
        if (!bar) { printf("error: need key|doc\n"); return; }
        *bar = 0;
        char *key = args, *doc = bar + 1;
        unescape(doc);
        char out[8192];
        if (json_str(doc, key, out, sizeof out)) printf("%s\n", out);
        else printf("error: not found\n");
    } else if (strcmp(fn, "json_find_value") == 0) {
        char *bar = strchr(args, '|');
        if (!bar) { printf("error: need key|doc\n"); return; }
        *bar = 0;
        char *key = args, *doc = bar + 1;
        unescape(doc);
        size_t s = 0, e = 0;
        if (json_find_value(doc, 0, strlen(doc), key, &s, &e)) {
            char slice[8192];
            size_t n = e - s < sizeof slice - 1 ? e - s : sizeof slice - 1;
            memcpy(slice, doc + s, n);
            slice[n] = 0;
            esc_print(slice);
        } else printf("error: not found\n");
    } else if (strcmp(fn, "match_close") == 0) {
        char *bar = strrchr(args, '|');
        if (!bar) { printf("error: need doc|index\n"); return; }
        *bar = 0;
        char *doc = args;
        long idx = strtol(bar + 1, NULL, 10);
        unescape(doc);
        size_t r = match_close(doc, (size_t)idx);
        if (r == (size_t)-1) printf("error: not found\n");
        else printf("%zu\n", r);
    } else if (strcmp(fn, "html_to_text") == 0) {
        unescape(args);
        char out[8192];
        size_t n = html_to_text(args, strlen(args), out, sizeof out);
        (void)n;
        esc_print(out);
    } else if (strcmp(fn, "strip_diff_context") == 0) {
        unescape(args);
        buf_t b = {0}; buf_init(&b, 32768);
        strip_diff_context(args, &b);
        esc_print(b.p ? b.p : "");
        buf_free(&b);
    } else {
        printf("error: no such fn %s\n", fn);
    }
}

int main(void)
{
    char line[65536];
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
