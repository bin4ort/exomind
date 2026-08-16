#ifndef EXOKIT_H
#define EXOKIT_H

#include <stddef.h>
#include <stdint.h>

#define EXOKIT_VERSION "0.1.0"
#define MAX_LINE (64u * 1024u)
#define MAX_FN 256
#define MAX_EXAMPLES 2000

/* ---- util.c ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int64_t now_ms(void);
int64_t now_epoch(void);
uint32_t rand32(void);
int ci_prefix(const char *line, const char *prefix);
char *esc_line(const char *s, size_t n);
char *unesc_line(const char *s);

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

void buf_put(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);

/* ---- kit model ---- */
typedef struct {
    char fn[MAX_FN];
    char *sig;   /* one-line signature */
    int pure;    /* 1 = pure (no side effects) */
    char *side_effects;
    char *notes;
} cfn_t;

typedef struct {
    char fn[MAX_FN];
    char *args;      /* raw arg text passed to the runner */
    char *expected;  /* expected stdout (after unescape) */
    char *desc;
    int is_error;    /* example expects an error: response */
} example_t;

typedef struct {
    char *path;      /* the kit dir */
    cfn_t *fns;
    size_t nfns;
    example_t *exs;
    size_t nexs;
    char runner_cmd[1024]; /* default runner command from kit/config */
} kit_t;

/* ---- main.c ---- */
void kit_load(kit_t *k, const char *dir, char *err, size_t errsz);
void kit_free(kit_t *k);
int cmd_init(int argc, char **argv);
int cmd_extract(int argc, char **argv);
int cmd_verify(int argc, char **argv);
int cmd_diff(int argc, char **argv);
int cmd_audit(int argc, char **argv);

#endif
