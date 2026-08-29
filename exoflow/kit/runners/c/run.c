/* exokit C runner shim for the exoflow behavioral contract.
 *
 * Anti-drift design: compiled with the project root on the include path and
 * #include-ing the REAL exoflow sources (flows.c/util.c). flow_id_ok and
 * safe_token are static, so including the .c gives the runner access to their
 * actual implementations. The exomind-client calls that other functions in
 * flows.c make (exo_persist, xs_remind, ...) are never reached by the two
 * curated pure functions, so stub implementations satisfy the linker without
 * pulling in the network client.
 *
 * Protocol: read one line `fn<TAB>args` on stdin, print one result line.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "exoflow/src/flows.c"
#include "exoflow/src/util.c"

/* ---- stubs for exoflow's exomind client (unused by curated fns) ---- */
int cli_init(cli_t *c, const char *url, char *err, size_t errsz) { (void)c;(void)url;(void)err;(void)errsz; return -1; }
int cli_request(cli_t *c, const char *method, const char *target,
                const char *body, size_t blen, int json_ct, char **out,
                size_t *outlen, int *status, char *err, size_t errsz)
{ (void)c;(void)method;(void)target;(void)body;(void)blen;(void)json_ct;(void)out;(void)outlen;(void)status;(void)err;(void)errsz; return -1; }
int exo_persist(cli_t *e, const char *key, const char *value, long ttl, char *err, size_t errsz) { (void)e;(void)key;(void)value;(void)ttl;(void)err;(void)errsz; return -1; }
int exo_del(cli_t *e, const char *key, int *existed, char *err, size_t errsz) { (void)e;(void)key;(void)existed;(void)err;(void)errsz; return -1; }
int exo_note(cli_t *e, const char *text, char *err, size_t errsz) { (void)e;(void)text;(void)err;(void)errsz; return -1; }
int exo_list(cli_t *e, const char *prefix, char ***keys, size_t *n, char *err, size_t errsz) { (void)e;(void)prefix;(void)keys;(void)n;(void)err;(void)errsz; return -1; }
int exo_batch_get(cli_t *e, char **keys, size_t n, char ***vals, char *err, size_t errsz) { (void)e;(void)keys;(void)n;(void)vals;(void)err;(void)errsz; return -1; }
int xs_remind(cli_t *x, int64_t at_epoch, const char *msg, char *err, size_t errsz) { (void)x;(void)at_epoch;(void)msg;(void)err;(void)errsz; return -1; }

static void dispatch(const char *fn, const char *args)
{
    if (strcmp(fn, "flow_id_ok") == 0) {
        printf("%d\n", flow_id_ok(args));
    } else if (strcmp(fn, "safe_token") == 0) {
        /* args = "token|max"; '|' never appears in a valid token so it is
         * a safe separator even when the token itself contains a space. */
        char tok[4096];
        size_t max = 0;
        char *bar = strchr(args, '|');
        if (bar && bar != args) {
            size_t tl = (size_t)(bar - args);
            if (tl < sizeof tok) {
                memcpy(tok, args, tl);
                tok[tl] = 0;
                if (sscanf(bar + 1, "%zu", &max) == 1)
                    printf("%d\n", safe_token(tok, max));
                else
                    printf("error: bad args\n");
            } else {
                printf("error: bad args\n");
            }
        } else {
            printf("error: bad args\n");
        }
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
