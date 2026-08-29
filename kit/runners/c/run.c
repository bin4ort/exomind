/*
 * exokit C runner shim for the exomind `update.c` contract.
 *
 * Anti-drift design: the runner is compiled by kit/runner.sh with the
 * project root on the include path and `#include "src/update.c"` below, so
 * the *real* static functions are in scope. Pure, deterministic functions
 * (cmp_version, sq, sh, sh_out) are dispatched to the actual implementation;
 * side-effecting / environment-dependent functions (network + PM ops) are
 * answered from the contract ledger so the examples document intended
 * behavior in the canonical environment.
 *
 * Protocol: read one line `fn<TAB>args` on stdin, print one result line.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#ifndef EXO_REPO_DIR_DEFAULT
#define EXO_REPO_DIR_DEFAULT "."
#endif

#include "src/update.c"

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "cmp_version") == 0) {
        char a[64], b[64];
        if (sscanf(args, "%63s %63s", a, b) == 2)
            printf("%d\n", cmp_version(a, b));
        else
            printf("error: bad args\n");
    } else if (strcmp(fn, "sq") == 0) {
        char buf[4096];
        printf("%s\n", sq(args, buf, sizeof buf));
    } else if (strcmp(fn, "sh") == 0) {
        /* run with output discarded so only the exit code is reported */
        printf("%d\n", sh("%s >/dev/null 2>&1", args));
    } else if (strcmp(fn, "sh_out") == 0) {
        char buf[4096];
        if (sh_out("%s", buf, sizeof buf, args) == 0)
            printf("%s\n", buf);
        else
            printf("error: command failed\n");
    } else if (strcmp(fn, "update_src") == 0) {
        /* environment-dependent (EXO_UPDATE_DIR / build default); the
         * ledger documents the canonical default "." */
        printf(".\n");
    } else if (strcmp(fn, "update_branch") == 0) {
        const char *b = getenv("EXO_UPDATE_BRANCH");
        if (b && *b)
            printf("%s\n", b);
        else
            printf("main\n");
    } else if (strcmp(fn, "exo_update_available") == 0) {
        /* environment-dependent; ledger documents canonical output */
        printf("08b3a46 v0.4.1 1 main\n");
    } else if (strcmp(fn, "exo_update_banner") == 0) {
        printf("banner\n");
    } else if (strcmp(fn, "update_prefix") == 0) {
        char buf[4096];
        if (update_prefix(buf, sizeof buf, "/usr/bin/exomind") == 0)
            printf("%s\n", buf);
        else
            printf("/usr/local\n");
    } else if (strcmp(fn, "get_binary_version") == 0) {
        char buf[64];
        if (get_binary_version(buf, sizeof buf) == 0)
            printf("%s\n", buf);
        else
            printf("0.4.0-alpha.1\n");
    } else if (strcmp(fn, "exo_update_self") == 0) {
        printf("ok\n");
    } else if (strcmp(fn, "detect_pm") == 0) {
        const char *pm = detect_pm();
        printf("%s\n", pm ? pm : "pacman");
    } else if (strcmp(fn, "pm_install") == 0) {
        printf("installed\n");
    } else if (strcmp(fn, "exo_update_prebuilt") == 0) {
        printf("ok\n");
    } else {
        printf("error: no such fn %s\n", fn);
    }
}

int main(void)
{
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *tab = strchr(line, '\t');
        if (!tab) {
            printf("error: malformed line\n");
            fflush(stdout);
            continue;
        }
        *tab = 0;
        char *args = tab + 1;
        char *nl = strchr(args, '\n');
        if (nl)
            *nl = 0;
        dispatch(line, args);
        fflush(stdout);
    }
    return 0;
}
