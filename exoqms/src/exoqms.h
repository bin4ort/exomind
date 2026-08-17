#ifndef EXOQMS_H
#define EXOQMS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define EXOQMS_VERSION "0.4.0-alpha.1"
#define OBJ_KEY_PREFIX "exoqms:obj:"
#define NC_KEY_PREFIX "exoqms:nc:"
#define AUDIT_KEY_PREFIX "exoqms:audit:"
#define AGENTS_KEY "exoqms:config:agents"
#define NOTES24_KEY "exoqms:config:notes24h"
#define ID_MAX 48
#define STATUS_MAX 16
#define SEV_MAX 8
#define CHECK_TIMEOUT_S 5
#define MAX_FIELD (64u * 1024u)

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
void trim_crlf(char *s);
int tab_split(char *s, char **f, int maxf);

/* ---- minimal JSON (pattern: exomind src/util.c) ---- */
char *json_field(const char *json, size_t len, const char *field);
int json_array_each(const char *json, size_t len, size_t *pos,
                    const char **elem, size_t *elen);
char **json_arr_strings(const char *elem, size_t elen, size_t *n);

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

void buf_put(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);

/* ---- exomind.c : HTTP client for the exomind storage backend ---- */
typedef struct exo {
    char host[256];
    int port;
} exo_t;

int exo_init(exo_t *e, const char *url, char *err, size_t errsz);
int exo_request(exo_t *e, const char *method, const char *target,
                const char *body, size_t blen, int json_ct,
                char **out, size_t *outlen, int *status,
                char *err, size_t errsz);
int exo_persist(exo_t *e, const char *key, const char *value,
                char *err, size_t errsz);
int exo_get(exo_t *e, const char *key, char **out, char *err, size_t errsz);
int exo_note(exo_t *e, const char *text, char *err, size_t errsz);
int exo_list(exo_t *e, const char *prefix, char ***keys, size_t *n,
             char *err, size_t errsz);

/* ---- model.c : durable registries (objectives, NCs, audits) ---- */
typedef struct obj {
    char id[ID_MAX];
    char *desc;
    char *metric;
    char *target;
    char *period;
    int64_t created;
} obj_t;

typedef struct nc {
    char id[ID_MAX];
    char *title;
    char *desc;
    char *source;
    char *caction;
    char *evidence;
    char *closed_by;
    char sev[SEV_MAX];
    char status[STATUS_MAX];
    int64_t detected_at;
    int64_t closed_at;
} nc_t;

typedef struct audit {
    char id[ID_MAX];
    char *name;
    char *criteria;
    int64_t scheduled;
    char status[STATUS_MAX];
    int score;
    char *findings; /* raw blob: one "check<TAB>result<TAB>evidence" per line */
} audit_t;

typedef struct {
    obj_t *objs;
    size_t n_objs;
    nc_t *ncs;
    size_t n_ncs;
    audit_t *audits;
    size_t n_audits;
    pthread_mutex_t mu;
} qms_t;

void qms_init(qms_t *q);
void qms_free(qms_t *q);
int qms_reload(qms_t *q, exo_t *e, char **agents, int *notes24h,
               char *err, size_t errsz);
int obj_create(qms_t *q, exo_t *e, const char *desc, const char *metric,
               const char *target, const char *period, char *outid,
               size_t outsz, char *err, size_t errsz);
int nc_create(qms_t *q, exo_t *e, const char *title, const char *sev,
              const char *desc, const char *source, char *outid,
              size_t outsz, char *err, size_t errsz);
int nc_transition(qms_t *q, exo_t *e, const char *id, const char *action,
                  const char *body, char *outstatus, size_t outsz,
                  char *err, size_t errsz);
int audit_save(qms_t *q, exo_t *e, const char *id, const char *name,
               const char *criteria, const char *findings, int score,
               char *err, size_t errsz);
obj_t *obj_find(qms_t *q, const char *id);
nc_t *nc_find(qms_t *q, const char *id);
/* detection registry: upsert issue:<check> on each audit finding */
int issue_register(exo_t *e, const char *check, int is_fail,
                   const char *evidence, char *err, size_t errsz);
audit_t *audit_find(qms_t *q, const char *id);

/* ---- checks.c : the ISO 19011 audit program checklist ---- */
typedef enum { R_PASS, R_FAIL, R_SKIP } res_t;

typedef struct {
    char id[32];
    res_t res;
    char evidence[4096];
} finding_t;

/* ---- project.c : <repo>/.exoqms.json universal project config ---- */
typedef struct {
    int rules_debt;       /* 1 = enabled (default) */
    int rules_hygiene;
    int rules_secrets;
    int rules_codesafety;
    int debt_threshold;   /* default 10 */
    char **test_cmds;     /* whole-project test commands (no manifest mode) */
    size_t n_test;
    char **docs;          /* required docs (no manifest doc-compliance) */
    size_t n_docs;
    char **ignore;        /* ignore globs passed to the analysis tools */
    size_t n_ignore;
    char **languages;     /* language override for code-safety */
    size_t n_languages;
    char **secrets_allow; /* path substrings excluded from secrets */
    size_t n_secrets_allow;
} pcfg_t;

typedef struct cfg {
    char exodoc_path[1024];
    char ui_path[1024];
    char code_path[1024];
    char kit_path[1024];
    char svg_path[1024];
    char rules_path[1024];
    char repo[1024];
    char agents[1024];
    int notes24h;
    char exosched_url[256];
    pcfg_t pcfg;
} cfg_t;

typedef struct {
    cfg_t *cfg;
    exo_t *exo;
    const char *target; /* ui-audit target, may be NULL */
    const char *agents; /* dogfood agent override, may be NULL */
    /* memo for the shared exoqms-code --rules scan (debt/hygiene/secrets) */
    char *rules_json;
    size_t rules_len;
    int rules_cached;
} check_ctx_t;

int check_run(const char *id, check_ctx_t *ctx, finding_t *f);
int run_child(char *const argv[], const char *cwd, long timeout_s,
              char **out, size_t *outlen, char *err, size_t errsz);
void cfg_defaults(cfg_t *cfg);
void ctx_cleanup(check_ctx_t *ctx);
int trend_values(exo_t *e, int64_t **vals, int *n, char **list, size_t *llen);
const char *trend_verdict(int64_t *vals, int n, int *flag);

void pcfg_defaults(pcfg_t *p);
void pcfg_free(pcfg_t *p);
/* load <repo>/.exoqms.json into *p (defaults first). Returns 0 on
 * success, 1 when absent, -1 when malformed (defaults kept, warned to
 * stderr). Never crashes on malformed input. */
int pcfg_load(pcfg_t *p, const char *repo);
/* ---- http.c : the API ---- */
void http_set_token(const char *tok);
int http_handle_conn(int fd, exo_t *e, cfg_t *cfg, qms_t *q);
/* internal dispatch for the console one-shot operations (no HTTP auth) */
int http_dispatch(const char *method, const char *path, const char *query,
                  const char *body, size_t body_len, buf_t *out,
                  int *status, const char **ctype, exo_t *e, cfg_t *cfg,
                  qms_t *q);
const char *http_spec_text(void);
extern int g_rate_limit_active;

#endif
