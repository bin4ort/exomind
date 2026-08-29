/* exokit C runner shim for the exoqms behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * #include-ing the REAL exoqms source (util.c). exoqms/src/util.c is
 * self-contained (its own xmalloc/buf/json helpers, no exomind-client deps),
 * so the curated pure functions are verified against the shipped code with no
 * stubs needed. Protocol: read one line `fn<TAB>args`, print one result line.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "exoqms/src/exoqms.h"
#include "exoqms/src/util.c"

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "ci_prefix") == 0) {
        char *bar = strchr(args, '|');
        if (bar) { *bar = 0; printf("%d\n", ci_prefix(args, bar + 1)); }
        else printf("error: need line|prefix\n");
    } else if (strcmp(fn, "esc_line") == 0) {
        char *r = esc_line(args, strlen(args));
        printf("%s\n", r ? r : "");
        free(r);
    } else if (strcmp(fn, "unesc_line") == 0) {
        char *r = unesc_line(args);
        printf("%s\n", r ? r : "");
        free(r);
    } else if (strcmp(fn, "json_escape") == 0) {
        char *r = json_escape(args, strlen(args));
        printf("%s\n", r ? r : "");
        free(r);
    } else if (strcmp(fn, "json_field") == 0) {
        char *bar = strchr(args, '|');
        if (bar) {
            *bar = 0;
            char *r = json_field(args, strlen(args), bar + 1);
            printf("%s\n", r ? r : "");
            free(r);
        } else printf("error: need json|field\n");
    } else if (strcmp(fn, "json_arr_strings") == 0) {
        size_t n = 0;
        char **arr = json_arr_strings(args, strlen(args), &n);
        for (size_t i = 0; i < n; i++) {
            if (i) putchar(',');
            fputs(arr[i], stdout);
        }
        putchar('\n');
        for (size_t i = 0; i < n; i++) free(arr[i]);
        free(arr);
    } else {
        printf("error: no such fn %s\n", fn);
    }
}

int main(void)
{
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *args = tab + 1;
        char *nl = strchr(args, '\n');
        if (nl) *nl = 0;
        dispatch(line, args);
        fflush(stdout);
    }
    return 0;
}
