/* exodoc — the documentation auditor for the AI-native stack. */
#ifndef EXODOC_H
#define EXODOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define EXODOC_VERSION "0.1.0"
#define DOC_MAX (16u * 1024u * 1024u)   /* doc/manifest/spec truncation cap */
#define MANIFEST_MAX (4u * 1024u * 1024u)
#define HTTP_MAX (4u * 1024u * 1024u)
#define HTTP_TIMEOUT_S 5
#define NCOMP_MAX 64
#define NCHECK_MAX 9
#define NEP_MAX 128
#define NAME_MAX 64
#define DIR_MAX 256
#define CMD_MAX 256
#define DETAIL_MAX 512
#define OUT_MAX 4096

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} buf_t;

typedef enum { CK_PASS, CK_FAIL, CK_SKIP } ck_status_t;

typedef struct {
    char id[32];
    ck_status_t st;
    char detail[DETAIL_MAX];
} check_t;

typedef struct {
    char method[8];
    char path[128];
} endpoint_t;

typedef struct {
    char name[NAME_MAX];
    char dir[DIR_MAX];
    int port;                   /* 0 = not a live daemon */
    char build_cmd[CMD_MAX];
    char test_cmd[CMD_MAX];
    char version_flag[64];
    char doc_path[512];
    check_t checks[NCHECK_MAX];
    size_t nchecks;
    int pass, fail, skip;
    int score;                  /* percent, excludes skip; -1 if none */
    endpoint_t doc_eps[NEP_MAX];
    size_t ndoc_eps;
    endpoint_t live_eps[NEP_MAX];
    size_t nlive_eps;
    endpoint_t live_only[NEP_MAX];
    size_t nlive_only;
    endpoint_t doc_only[NEP_MAX];
    size_t ndoc_only;
    char doc_version[32];
} comp_t;

typedef struct {
    comp_t comps[NCOMP_MAX];
    size_t n;
    char manifest_path[512];
    char base[512];
} stack;

/* ---- util.c ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void buf_put(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);
char *json_escape(const char *s, size_t n);
int ci_prefix(const char *line, const char *prefix);
void lc(char *s);
size_t scan_version(const char *s, size_t n, char *out, size_t outsz);

/* ---- http.c ---- */
int http_get(const char *host, int port, const char *path, char **body,
             size_t *blen, int *status, char *err, size_t errsz);
int http_post_json(const char *host, int port, const char *path,
                   const char *body, size_t blen, char **resp, size_t *rlen,
                   int *status, char *err, size_t errsz);

/* ---- audit.c ---- */
int manifest_parse(const char *path, stack *st);
int audit_components(stack *st, int live);
int run_cmd_out(const char *bin, const char *flag, char *out, size_t outsz);
void report_human(stack *st, int live, FILE *f);
void report_json(stack *st, int live, FILE *f);
int stack_totals(stack *st, int *pass, int *fail, int *skip);

/* ---- main.c ---- */
int exo_parse_url(const char *url, char *host, size_t hostsz, int *port);
int exo_persist(const char *url, const char *key, const char *value,
                char *err, size_t errsz);
int exo_note(const char *url, const char *text, char *err, size_t errsz);

#endif
