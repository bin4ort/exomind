/*
 * exoqms-code: static C analyzer for error-handling and declaration quality.
 * QMS field module (code safety). Zero dependencies.
 *
 * Checks:
 *   unchecked-return       call to an error-returning function ignored
 *   missing-error-path     error-returning result used without an if-not branch
 *   empty-error-branch     "if (!x) { }" - failure branch present but empty
 *   uninitialized-use      local declared but read before assignment
 *   swallowed-error        "if (err != 0) { }" or "if (err);" - error eaten
 *   unchecked-deref-alloc  malloc/calloc/realloc result dereferenced unguarded
 *
 * Lexical analysis, no full parse. Undeterminable -> stay silent.
 * False positives are the enemy: when unsure, no finding.
 */
#ifndef EXOQMS_CODE_H
#define EXOQMS_CODE_H

#include <stddef.h>
#include <stdint.h>

#define MAX_LINE 65536
#define MAX_TOKENS (1u << 20)

typedef enum {
    T_IDENT = 1,
    T_NUMBER,
    T_STRING,
    T_CHAR,
    T_KEYWORD,
    T_OP,
    T_PUNCT,
    T_EOF
} tok_type;

typedef struct {
    tok_type type;
    char text[64];
    size_t len;
    int line;
    int col;
} tok_t;

typedef struct {
    tok_t *toks;
    size_t ntok;
    size_t cap;
} tokvec_t;

typedef struct {
    char *name;
    int returns_void;
    int returns_ptr;  /* return type contains '*' -> can be NULL */
    int never_null;   /* documented with an @nonnull annotation before the def */
    int def_line;
    size_t body_begin; /* token index of '{' */
    size_t body_end;   /* token index just past matching '}' */
    char **params;
    size_t nparams;
} fn_t;

typedef struct {
    fn_t *fns;
    size_t nfn;
    size_t cap;
    char **typs;  /* typedef names collected at file scope */
    size_t ntyps;
} fnvec_t;

typedef struct {
    char *path;
    char *dir;
} ignore_t;

void tokvec_init(tokvec_t *tv);
int tokenize_file(const char *path, tokvec_t *tv);
void tokvec_free(tokvec_t *tv);

int collect_functions(tokvec_t *tv, fnvec_t *fv);
void fnvec_free(fnvec_t *fv);
int fn_is_error_fn(const fnvec_t *fv, const char *name, size_t len,
                   int *returns_void);

typedef struct {
    const char *check;
    const char *severity; /* "major" or "minor" */
    int line;
    int col;
    char reason[512];
} finding_t;

typedef struct {
    finding_t *f;
    size_t nf;
    size_t cap;
} findvec_t;

void analyze_file(const char *path, tokvec_t *tv, const fnvec_t *fv,
                  findvec_t *out);
void findvec_free(findvec_t *fv);

int is_known_err_fn(const char *name, size_t len);

#endif
