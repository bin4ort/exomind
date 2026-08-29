/* exokit C runner shim for the exocontext behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * #include-ing the REAL exocontext source (util.c), which holds every curated
 * pure function. query_param/ctx_storage_budget live in main.c (which defines
 * main()) so they cannot be #included into a runner; excluded here.
 *
 * Argument encoding: '|' separates sub-fields for ci_prefix only.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "exocontext/src/util.c"

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "utf8_decode") == 0) {
        uint32_t cp = 0;
        int r = utf8_decode((const unsigned char *)args, strlen(args), &cp);
        if (r <= 0)
            printf("0 0\n");
        else
            printf("%d %x\n", r, (unsigned)cp);
    } else if (strcmp(fn, "ci_prefix") == 0) {
        char line[8192], prefix[8192];
        if (sscanf(args, "%8191[^|]|%8191[^\n]", line, prefix) == 2)
            printf("%d\n", ci_prefix(line, prefix));
        else
            printf("error: bad args\n");
    } else if (strcmp(fn, "esc_line") == 0) {
        char *o = esc_line(args, strlen(args));
        printf("%s\n", o ? o : "");
        free(o);
    } else if (strcmp(fn, "unesc_line") == 0) {
        char *o = unesc_line(args);
        printf("%s\n", o ? o : "");
        free(o);
    } else if (strcmp(fn, "json_escape") == 0) {
        char *o = json_escape(args, strlen(args));
        printf("%s\n", o ? o : "");
        free(o);
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
