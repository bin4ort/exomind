#ifndef EXOFLOW_H
#define EXOFLOW_H

#include <stddef.h>
#include <stdint.h>

#define EXOFLOW_VERSION "0.2.0"
#define EXO_KEY_PREFIX "exoflow:flow:"
#define FLOW_ID_MAX 40
#define STEP_ID_MAX 64
#define OWNER_MAX 64
#define STATE_MAX 16
#define MAX_MSG (64u * 1024u)
#define RETRY_DELAY_NS (5LL * 1000000000LL)
#define FLOW_TSV_VERSION "2"

typedef struct step {
    char id[STEP_ID_MAX];
    char *desc;
    char **deps;       /* step ids this step depends on */
    size_t ndeps;
    char state[STATE_MAX]; /* pending|claimed|done|failed|overdue|cancelled */
    char owner[OWNER_MAX];
    int64_t deadline;  /* epoch seconds, 0 = none */
} step_t;

typedef struct flow {
    char id[FLOW_ID_MAX];
    char *name;
    step_t *steps;
    size_t nsteps;
    struct flow *next;
    /* loop fields (exoflow >= 0.2.0); zero when the flow does not loop.
     * every iteration of a loop is its own flow record; the first one has
     * no parent, every later one links `loop_parent` back to the first. */
    int loop_active;      /* this record belongs to a loop */
    int64_t loop_interval; /* seconds between iterations */
    int64_t loop_max;     /* max counted iterations, 0 = unlimited */
    int64_t loop_until;   /* epoch, 0 = none; no run starts at/after it */
    int64_t loop_iter;    /* iteration label of THIS record (1-based) */
    int64_t loop_budget;  /* counted iterations consumed so far incl. this
                             record unless it was cancelled */
    int64_t loop_next;    /* epoch of the next run, 0 = no more runs */
    char loop_parent[FLOW_ID_MAX]; /* id of the first iteration, or "" */
    int loop_stopped;     /* stop-loop halted future iterations */
} flow_t;

/* ---- util.c ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int64_t now_epoch(void);
uint32_t rand32(void);
int ci_prefix(const char *line, const char *prefix);
char *esc_line(const char *s, size_t n);
char *unesc_line(const char *s);
char *json_escape(const char *s, size_t n);

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

void buf_put(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);

/* ---- cli.c : HTTP client for exomind (storage) and exosched (deadlines) ---- */
typedef struct cli {
    char host[256];
    int port;
} cli_t;

int cli_init(cli_t *c, const char *url, char *err, size_t errsz);
int cli_request(cli_t *c, const char *method, const char *target,
                const char *body, size_t blen, int json_ct,
                char **out, size_t *outlen, int *status,
                char *err, size_t errsz);
int exo_persist(cli_t *e, const char *key, const char *value, long ttl,
                char *err, size_t errsz);
int exo_del(cli_t *e, const char *key, int *existed, char *err, size_t errsz);
int exo_note(cli_t *e, const char *text, char *err, size_t errsz);
int exo_list(cli_t *e, const char *prefix, char ***keys, size_t *n,
             char *err, size_t errsz);
int exo_batch_get(cli_t *e, char **keys, size_t n, char ***vals,
                  char *err, size_t errsz);
int xs_remind(cli_t *x, int64_t at_epoch, const char *msg,
              char *err, size_t errsz);

/* ---- flows.c : registry + model ---- */
void flows_init(void);
void flows_lock(void);
void flows_unlock(void);
flow_t *flow_find(const char *id);
flow_t *flows_first(void);
size_t flow_count(void);
int flows_reload(cli_t *e, cli_t *x);
char *flow_serialize(const flow_t *f);
int flow_create(cli_t *e, cli_t *x, const char *body, size_t blen,
                char *fid, size_t fidcap, size_t *nsteps_out,
                char *err, size_t errsz);
int flow_delete(cli_t *e, const char *id, int *existed, char *err, size_t errsz);
int flow_cancel(cli_t *e, const char *id, char *err, size_t errsz);
int flow_stop_loop(cli_t *e, const char *id, char *err, size_t errsz);
/* lazy loop scheduler: spawns the next iteration of a terminal looping
 * flow whose next_run is due. caller must hold the registry lock. */
int loop_tick(cli_t *e, cli_t *x, flow_t *f, char *err, size_t errsz);
int flow_is_loop(const flow_t *f);
int flow_sweep(cli_t *e, flow_t *f, char *err, size_t errsz);
int step_do(cli_t *e, flow_t *f, const char *sid, const char *action,
            const char *note, char *err, size_t errsz);
int flow_next(cli_t *e, flow_t *f, const char *worker, char *sid,
              size_t sidcap, char *err, size_t errsz);
const char *flow_status(const flow_t *f);

/* ---- http.c : exoflow's own HTTP layer ---- */
void http_set_token(const char *tok);
int http_handle_conn(int fd, cli_t *xm, cli_t *xs);

#endif
