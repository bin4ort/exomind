/* exokit C runner shim for the exosched behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * #include-ing the REAL exosched sources (schedule.c/util.c), so the static
 * pure functions are dispatched to their actual implementations.
 *
 * Protocol: read one line `fn<TAB>args` on stdin, print one result line.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "exosched/src/schedule.c"
#include "exosched/src/util.c"

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "parse_duration") == 0) {
        const char *p = args;
        const char *e = args + strlen(args);
        int64_t s = 0;
        char err[256];
        if (parse_duration(&p, e, &s, err, sizeof err) == 0)
            printf("%lld\n", (long long)s);
        else
            printf("error: %s\n", err);
    } else if (strcmp(fn, "parse_schedule") == 0) {
        int64_t fire = 0, rep = 0, until = 0;
        int rc = 0;
        char *msg = NULL;
        char err[256];
        int r = parse_schedule(args, strlen(args), &fire, &rep, &until,
                                &rc, &msg, err, sizeof err);
        if (r == 0)
            /* fire is relative (non-deterministic); report the stable
             * fields so the ledger stays portable across runs/machines */
            printf("rep=%lld msg=%s\n", (long long)rep, msg ? msg : "");
        else
            printf("error: %s\n", err);
        if (msg)
            free(msg);
    } else {
        printf("error: no such fn %s\n", fn);
    }
}

int main(void)
{
    char line[8192];
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
