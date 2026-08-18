#ifndef EXOCONTEXT_H
#define EXOCONTEXT_H

#include <stddef.h>
#include <stdint.h>

#define EXOCONTEXT_VERSION "0.4.0-alpha.1"
#define MAX_CONTEXT_BUDGET (256u * 1024u)

/* ---- session auto-compression ---- */
#define CTX_SUMMARY_SUFFIX ":summary"
#define CTX_BUDGET_DEFAULT (16u * 1024u)
#define CTX_SUMMARY_MAX_LINES 64

/* ---- util.c ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int64_t now_ms(void);
int64_t now_epoch(void);
int64_t mono_ns(void);
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
int exo_persist(exo_t *e, const char *key, const char *value, long ttl,
                char *err, size_t errsz);
int exo_del(exo_t *e, const char *key, int *existed, char *err, size_t errsz);
int exo_note(exo_t *e, const char *text, char *err, size_t errsz);
int exo_list(exo_t *e, const char *prefix, char ***keys, size_t *n,
             char *err, size_t errsz);
int exo_batch_get(exo_t *e, char **keys, size_t n, char ***vals,
                  char *err, size_t errsz);

/* ---- context.c / main.c ---- */
void ctx_build(exo_t *e, const char *agent, size_t budget, buf_t *out,
               char *err, size_t errsz);
int ctx_compress(exo_t *e, const char *agent, char *err, size_t errsz);
int http_handle_conn(int fd, exo_t *e);
void http_set_token(const char *tok);

#endif
