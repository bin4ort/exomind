/*
 * exoqms-code: function collection and the error-handling checks.
 *
 * Lexical, conservative: when a pattern is undeterminable the checker stays
 * silent. Findings carry file:line:col so a "var-rename" class bug points at
 * the exact function that lost its error path.
 */
#include "code.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libc/POSIX functions whose failure must not be silently ignored.
 * snprintf/vsnprintf and pthread_mutex_* are deliberately NOT here: their
 * failure is a programming error (encoding / misuse), and checking them is
 * not the norm in real C code; including them would drown real findings. */
static const char *ERR_FNS[] = {
    "fopen", "fdopen", "freopen", "tmpfile", "strdup", "strndup", "malloc",
    "calloc", "realloc", "reallocarray", "fgets", "getline", "getdelim",
    "pread", "pwrite", "read", "write", "fstat", "stat", "lstat", "open",
    "openat", "close", "fflush", "fclose", "fseek", "ftell", "fread",
    "fwrite", "fsync", "fdatasync", "ftruncate", "mkdir", "mkdirat",
    "rmdir", "chmod", "unlink", "rename", "pthread_create", "pthread_join",
    "getaddrinfo", "setsockopt", "getsockopt", "bind", "listen", "accept",
    "accept4", "connect", "recv", "send", "select", "poll", "dup", "dup2",
    "pipe", "socket", "getsockname", "getpeername", "socketpair", "shutdown",
    "setsid", "setenv", "putenv", "getcwd", "chdir", "fchdir", "opendir",
    "closedir", "realpath", "mkstemp", "mkdtemp", "mmap", "munmap",
    "msync", "nanosleep", "clock_gettime", NULL};

int is_known_err_fn(const char *name, size_t len)
{
    for (int i = 0; ERR_FNS[i]; i++) {
        if (strlen(ERR_FNS[i]) == len && memcmp(ERR_FNS[i], name, len) == 0)
            return 1;
    }
    return 0;
}

void fnvec_free(fnvec_t *fv)
{
    for (size_t i = 0; i < fv->nfn; i++) {
        for (size_t j = 0; j < fv->fns[i].nparams; j++)
            free(fv->fns[i].params[j]);
        free(fv->fns[i].params);
    }
    free(fv->fns);
    fv->fns = NULL;
    fv->nfn = 0;
    fv->cap = 0;
    for (size_t i = 0; i < fv->ntyps; i++)
        free(fv->typs[i]);
    free(fv->typs);
    fv->typs = NULL;
    fv->ntyps = 0;
}

static int is_typedef_name(const fnvec_t *fv, const char *name)
{
    for (size_t i = 0; i < fv->ntyps; i++)
        if (!strcmp(fv->typs[i], name))
            return 1;
    return 0;
}

static void fn_push(fnvec_t *fv, const char *name, size_t len,
                    int returns_void, int returns_ptr, int never_null,
                    int def_line, size_t body_begin, size_t body_end)
{
    if (fv->nfn >= fv->cap) {
        fv->cap = fv->cap ? fv->cap * 2 : 32;
        fn_t *nw = realloc(fv->fns, fv->cap * sizeof(fn_t));
        if (!nw)
            return;
        fv->fns = nw;
    }
    fn_t *f = &fv->fns[fv->nfn++];
    memset(f, 0, sizeof *f);
    f->name = strndup(name, len);
    f->returns_void = returns_void;
    f->returns_ptr = returns_ptr;
    f->never_null = never_null;
    f->def_line = def_line;
    f->body_begin = body_begin;
    f->body_end = body_end;
}

int fn_is_error_fn(const fnvec_t *fv, const char *name, size_t len,
                   int *returns_void)
{
    for (size_t i = 0; i < fv->nfn; i++) {
        if (strlen(fv->fns[i].name) == len &&
            memcmp(fv->fns[i].name, name, len) == 0) {
            /* only pointer-returning in-file functions count as error
               fns: they can return NULL. int/status-returning helpers
               are chaining patterns, not error sources. A documented
               @nonnull annotation clears the function. */
            if (returns_void)
                *returns_void = fv->fns[i].returns_void;
            return fv->fns[i].returns_ptr && !fv->fns[i].never_null;
        }
    }
    return 0;
}

static int is_type_tok(const tok_t *t)
{
    if (t->type != T_KEYWORD)
        return 0;
    static const char *TYPES[] = {
        "void", "int", "char", "long", "short", "unsigned", "signed",
        "float", "double", "struct", "enum", "union", "const", "volatile",
        "static", "extern", "register", "inline", "_Bool", "bool", "ssize_t",
        "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
        "int16_t", "int32_t", "int64_t", "off_t", "FILE", "errno_t",
        "time_t", "auto", "constexpr", "noexcept", "override", "virtual",
        "wchar_t", "char8_t", "char16_t", "char32_t", "template",
        "typename", "class", NULL};
    for (int i = 0; TYPES[i]; i++)
        if (!strcmp(TYPES[i], t->text))
            return 1;
    return 0;
}

static int tok_is(const tok_t *t, const char *s)
{
    return t && strcmp(t->text, s) == 0;
}

/*
 * Collect function definitions at brace depth 0:
 *   [storage/qualifiers] [type...] name ( params ) { body }
 * body_end = token index just past the matching '}'.
 */
int collect_functions(tokvec_t *tv, fnvec_t *fv)
{
    size_t depth = 0;
    for (size_t i = 0; i < tv->ntok; i++) {
        tok_t *t = &tv->toks[i];
        if (t->type == T_EOF)
            break;
        if (tok_is(t, "{")) {
            depth++;
            continue;
        }
        if (tok_is(t, "}")) {
            if (depth > 0)
                depth--;
            continue;
        }
        if (depth == 0 && tok_is(t, "typedef")) {
            /* collect the typedef name: last IDENT before ';' */
            const char *tn = NULL;
            size_t tnl = 0;
            for (size_t j = i + 1; j < tv->ntok; j++) {
                if (tok_is(&tv->toks[j], ";"))
                    break;
                if (tv->toks[j].type == T_IDENT) {
                    tn = tv->toks[j].text;
                    tnl = tv->toks[j].len;
                }
            }
            if (tn && !is_typedef_name(fv, tn)) {
                fv->typs = realloc(fv->typs, (fv->ntyps + 1) * sizeof(char *));
                if (fv->typs) {
                    fv->typs[fv->ntyps++] = strndup(tn, tnl);
                }
            }
            continue;
        }
        /* C++ class/namespace bodies: methods live at inner depths; a
           detected function's body is skipped wholesale, so scanning
           every depth is safe (C has no nested functions) */
        if (t->type != T_IDENT || i + 1 >= tv->ntok || !tok_is(&tv->toks[i + 1], "("))
            continue;
        /* inside a function body a call is not a definition: the walk
           below only accepts IDENT-or-type before the name, and calls
           at statement level start with a non-type token, so depth
           doesn't need to be checked here */
        int j = (int)i - 1;
        /* C++: methods and namespace-qualified names — walk back over
           `*` and `::`, treating an identifier before `::` as a class */
        while (j >= 0 && (tok_is(&tv->toks[j], "*") || tok_is(&tv->toks[j], "::")))
            j--;
        if (j < 0 || (!is_type_tok(&tv->toks[j]) && !tok_is(&tv->toks[j], "@nonnull")))
            continue;
        /* `Foo::bar(` — the class name itself is not a type token */
        if (!is_type_tok(&tv->toks[j])) {
            j = (int)i - 1;
            while (j >= 0 && (tok_is(&tv->toks[j], "*") || tok_is(&tv->toks[j], "::")))
                j--;
            while (j >= 1 && tok_is(&tv->toks[j - 1], "::"))
                j--;
            while (j >= 0 && tok_is(&tv->toks[j], "*"))
                j--;
        }
        int par = 0, returns_void = is_type_tok(&tv->toks[j]) && tok_is(&tv->toks[j], "void");
        int returns_ptr = 0, never_null = 0;
        for (int x = j; x <= (int)i; x++)
            if (tok_is(&tv->toks[x], "*"))
                returns_ptr = 1;
        for (int x = j - 1; x >= 0; x--) {
            if (tok_is(&tv->toks[x], "@nonnull")) {
                never_null = 1;
                break;
            }
            if (tok_is(&tv->toks[x], ";") || tok_is(&tv->toks[x], "}"))
                break;
        }
        size_t k = i + 1;
        for (; k < tv->ntok; k++) {
            if (tok_is(&tv->toks[k], "("))
                par++;
            else if (tok_is(&tv->toks[k], ")")) {
                par--;
                if (par == 0)
                    break;
            }
        }
        if (k >= tv->ntok)
            continue;
        size_t b = k + 1;
        if (b >= tv->ntok || !tok_is(&tv->toks[b], "{"))
            continue;
        size_t d = 1, e = b + 1;
        for (; e < tv->ntok; e++) {
            if (tok_is(&tv->toks[e], "{"))
                d++;
            else if (tok_is(&tv->toks[e], "}")) {
                d--;
                if (d == 0)
                    break;
            }
        }
        fn_push(fv, t->text, t->len, returns_void, returns_ptr, never_null,
                t->line, b, e < tv->ntok ? e + 1 : e);
        i = e;
    }
    return 0;
}

/* ---------------- findings ---------------- */

void findvec_free(findvec_t *fv)
{
    free(fv->f);
    fv->f = NULL;
    fv->nf = 0;
    fv->cap = 0;
}

static void fnd_pushf(findvec_t *v, const char *check, const char *sev, int line,
                      int col, const char *fmt, ...)
{
    if (v->nf >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        finding_t *nw = realloc(v->f, v->cap * sizeof(finding_t));
        if (!nw)
            return;
        v->f = nw;
    }
    finding_t *fd = &v->f[v->nf++];
    fd->check = check;
    fd->severity = sev;
    fd->line = line;
    fd->col = col;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fd->reason, sizeof fd->reason, fmt, ap);
    va_end(ap);
}

/* ---------------- per-variable tracking ---------------- */

typedef struct {
    char *name;
    int state; /* 0 gone, 1 declared, 2 assigned-unchecked, 3 checked, 4 initialized */
    int alloc;      /* assigned from malloc/calloc/realloc */
    int assign_line, assign_col;
    char fn[64];
    int reported;
    int is_param;
} var_t;

typedef struct {
    var_t *v;
    size_t nv, cap;
} varvec_t;

static void vars_free(varvec_t *vv)
{
    for (size_t i = 0; i < vv->nv; i++)
        free(vv->v[i].name);
    free(vv->v);
    vv->v = NULL;
    vv->nv = 0;
    vv->cap = 0;
}

static var_t *var_get(varvec_t *vv, const char *name)
{
    for (size_t i = 0; i < vv->nv; i++)
        if (!strcmp(vv->v[i].name, name))
            return &vv->v[i];
    return NULL;
}

static var_t *var_add(varvec_t *vv, const char *name, int is_param)
{
    var_t *vt = var_get(vv, name);
    if (vt)
        return vt;
    if (vv->nv >= vv->cap) {
        vv->cap = vv->cap ? vv->cap * 2 : 32;
        var_t *nw = realloc(vv->v, vv->cap * sizeof(var_t));
        if (!nw)
            return NULL;
        vv->v = nw;
    }
    vt = &vv->v[vv->nv++];
    memset(vt, 0, sizeof *vt);
    vt->name = strdup(name);
    vt->is_param = is_param;
    vt->state = is_param ? 4 : 1;
    (void)vt;
    return vt;
}

/*
 * Find the closing ')' of the call starting at toks[i] == IDENT and the
 * parens already opened at '('. Returns index of ')' or end.
 */
static size_t match_paren(tok_t *toks, size_t i, size_t end)
{
    int par = 0;
    for (; i < end; i++) {
        if (tok_is(&toks[i], "("))
            par++;
        else if (tok_is(&toks[i], ")")) {
            par--;
            if (par == 0)
                return i;
        }
    }
    return end;
}

/* x-allocators: abort on failure, never return NULL - not error sources */
static const char *XALLOC_FNS[] = {"xmalloc", "xcalloc", "xrealloc",
                                   "xstrdup", "xstrndup", NULL};

static int is_xalloc(const char *name)
{
    for (int i = 0; XALLOC_FNS[i]; i++)
        if (!strcmp(XALLOC_FNS[i], name))
            return 1;
    return 0;
}

/* does the expression toks[a..b) contain a call to an error fn? */
static const char *find_err_call(tok_t *toks, size_t a, size_t b,
                                 const fnvec_t *fv, int *is_alloc)
{
    for (size_t i = a; i + 1 < b; i++) {
        if (toks[i].type == T_IDENT && tok_is(&toks[i + 1], "(")) {
            const char *name = toks[i].text;
            if (is_xalloc(name))
                continue;
            if (is_known_err_fn(name, strlen(name))) {
                *is_alloc = !strcmp(name, "malloc") ||
                            !strcmp(name, "calloc") ||
                            !strcmp(name, "realloc") ||
                            !strcmp(name, "reallocarray");
                return name;
            }
            if (fn_is_error_fn(fv, name, strlen(name), NULL)) {
                *is_alloc = 0;
                return name;
            }
        }
    }
    return NULL;
}

/* is toks[i] an argument of a call (prev is '(' whose prev is an IDENT,
   or prev is ',')? Output-parameter patterns are exempted as writes. */
static int is_call_arg(tok_t *toks, size_t i, size_t begin)
{
    if (i <= begin)
        return 0;
    if (tok_is(&toks[i - 1], "("))
        return i > begin + 1 && toks[i - 2].type == T_IDENT;
    if (tok_is(&toks[i - 1], ","))
        return 1;
    return 0;
}

/* is the statement at toks[s] exactly `expr(...);` with result ignored? */
static void check_unchecked_return(tok_t *toks, size_t s, size_t end,
                                   const fnvec_t *fv, findvec_t *out)
{
    if (toks[s].type != T_IDENT || s + 1 >= end)
        return;
    if (!tok_is(&toks[s + 1], "("))
        return;
    /* the call must BEGIN a statement (prev = ; { } or nothing) */
    if (s > 0 && !tok_is(&toks[s - 1], ";") && !tok_is(&toks[s - 1], "{") &&
        !tok_is(&toks[s - 1], "}"))
        return;
    /* `(void)foo(...);` is a deliberate disclaimer */
    if (s >= 2 && tok_is(&toks[s - 1], ")") && tok_is(&toks[s - 2], "void"))
        return;
    /* assert(foo()) exempt */
    if (!strcmp(toks[s].text, "assert") || !strcmp(toks[s].text, "my_assert"))
        return;
    const char *name = toks[s].text;
    if (is_xalloc(name))
        return;
    if (!is_known_err_fn(name, strlen(name)) &&
        !fn_is_error_fn(fv, name, strlen(name), NULL))
        return;
    size_t close = match_paren(toks, s + 1, end);
    if (close >= end || !tok_is(&toks[close + 1], ";"))
        return; /* result used in larger expression -> handled elsewhere */
    fnd_pushf(out, "unchecked-return", "minor", toks[s].line, toks[s].col,
              "result of %s() ignored", name);
}

/* match a '{' at index b to its '}'; returns index of '}' or end */
static size_t match_brace(tok_t *toks, size_t b, size_t end)
{
    int d = 0;
    for (; b < end; b++) {
        if (tok_is(&toks[b], "{"))
            d++;
        else if (tok_is(&toks[b], "}")) {
            d--;
            if (d == 0)
                return b;
        }
    }
    return end;
}

/* empty body right after an if: `{ }` or `;` */
static int empty_after_if(tok_t *toks, size_t i, size_t end)
{
    size_t b = i;
    while (b < end && tok_is(&toks[b], "{")) {
        size_t e = match_brace(toks, b, end);
        if (e == b + 1)
            return 1;
        b = e;
        while (b < end && (tok_is(&toks[b], ";") || tok_is(&toks[b], "}")))
            b++;
    }
    if (b < end && tok_is(&toks[b], ";"))
        return 1;
    return 0;
}

/*
 * Statement-level checks within a function body:
 *  - unchecked-return: `errfn(...);` statement, result dropped
 *  - swallowed-error:  `if (errfn(...) != 0) { }` or `if (errfn(...));`
 *  - empty-error-branch: `if (!x) { }` / `if (x == NULL);` where x came from
 *    an error-returning assignment, followed by a use of x
 */
static void scan_statements(tok_t *toks, size_t begin, size_t end,
                            const fnvec_t *fv, findvec_t *out)
{
    for (size_t i = begin; i + 1 < end; i++) {
        if (tok_is(&toks[i], "if") && tok_is(&toks[i + 1], "(")) {
            size_t close = match_paren(toks, i + 1, end);
            if (close >= end)
                continue;
            if (empty_after_if(toks, close + 1, end)) {
                int is_alloc = 0;
                const char *call =
                    find_err_call(toks, i + 2, close, fv, &is_alloc);
                if (call) {
                    fnd_pushf(out, "swallowed-error", "minor", toks[i].line,
                              toks[i].col,
                              "failure of %s() checked with an empty branch (error eaten)",
                              call);
                } else {
                    /* empty branch whose condition is an error-style
                       comparison (`x != 0`, `x < n`) - plain `if (x) ;`
                       filter idioms are not error checks */
                    int has_cmp = 0, has_ident = 0;
                    for (size_t j = i + 2; j < close; j++) {
                        if (tok_is(&toks[j], "==") || tok_is(&toks[j], "!=") ||
                            tok_is(&toks[j], "<") || tok_is(&toks[j], ">") ||
                            tok_is(&toks[j], "<=") || tok_is(&toks[j], ">=") ||
                            tok_is(&toks[j], "!"))
                            has_cmp = 1;
                        if (toks[j].type == T_IDENT)
                            has_ident = 1;
                    }
                    if (has_cmp && has_ident) {
                        fnd_pushf(out, "empty-error-branch", "minor",
                                  toks[i].line, toks[i].col,
                                  "if-condition checked with an empty branch");
                    }
                }
            }
        }
        if (toks[i].type != T_IDENT)
            continue;
        check_unchecked_return(toks, i, end, fv, out);
    }
}

/* ---------------- body walk: variable lifecycle ---------------- */

static void analyze_body(tok_t *toks, size_t begin, size_t end,
                         const fnvec_t *fv, findvec_t *out)
{
    varvec_t vars = {0};
    size_t depth = 0;
    int in_cond = 0;

    for (size_t i = begin; i < end; i++) {
        tok_t *t = &toks[i];
        if (t->type == T_EOF)
            break;
        if (tok_is(t, "struct") || tok_is(t, "union") || tok_is(t, "enum")) {
            /* skip the whole struct/union/enum body: field declarations
               are not function variables */
            if (i + 1 < end && tok_is(&toks[i + 1], "{")) {
                size_t b = i + 1, d = 0;
                for (; b < end; b++) {
                    if (tok_is(&toks[b], "{"))
                        d++;
                    else if (tok_is(&toks[b], "}")) {
                        d--;
                        if (d == 0)
                            break;
                    }
                }
                if (b < end)
                    i = b;
            }
            continue;
        }
        if (tok_is(t, "{")) {
            depth++;
            continue;
        }
        if (tok_is(t, "}")) {
            if (depth > 0)
                depth--;
            continue;
        }
        if (tok_is(t, ")")) {
            if (in_cond) {
                in_cond = 0;
                continue;
            }
        }
        if (tok_is(t, "(") || tok_is(t, "["))
            continue;
        if (tok_is(t, "if") || tok_is(t, "while") || tok_is(t, "for")) {
            if (i + 1 < end && tok_is(&toks[i + 1], "("))
                in_cond = 1;
            continue;
        }
        if (in_cond) {
            if (t->type == T_IDENT) {
                /* member names after -> or . are not variables */
                if (i > begin && (tok_is(&toks[i - 1], "->") ||
                                  tok_is(&toks[i - 1], ".")))
                    continue;
                /* C++ namespace/type qualifiers are not variables */
                if (i + 1 < end && (tok_is(&toks[i + 1], "::") ||
                                    toks[i + 1].type == T_IDENT))
                    continue;
                /* for-init: `for (i = 0; ...)` - the target is a write */
                if (i + 1 < end && tok_is(&toks[i + 1], "=")) {
                    var_t *wt = var_get(&vars, t->text);
                    if (wt)
                        wt->state = 4;
                    continue;
                }
                var_t *vt = var_get(&vars, t->text);
                if (is_call_arg(toks, i, begin)) {
                    /* output-parameter pattern: fn(buf,...) fills it */
                    if (vt && vt->state == 1)
                        vt->state = 3;
                    continue;
                }
                if (i > begin && tok_is(&toks[i - 1], "&")) {
                    if (vt && vt->state == 1)
                        vt->state = 4; /* &x: callee initializes */
                    continue;
                }
                if (i > begin && tok_is(&toks[i - 1], "sizeof"))
                    continue; /* sizeof x: no read of the value */
                if (!vt)
                    continue;
                if (vt->state == 2)
                    vt->state = 3; /* guarded by the condition */
                else if (vt->state == 1 && !vt->reported) {
                    fnd_pushf(out, "uninitialized-use", "minor", t->line,
                              t->col,
                              "%s read before assignment (in a condition)",
                              t->text);
                    vt->reported = 1;
                    vt->state = 4;
                }
            }
            continue;
        }
        if (t->type != T_IDENT)
            continue;

        /* member names after -> or . are not variables */
        if (i > begin && (tok_is(&toks[i - 1], "->") ||
                          tok_is(&toks[i - 1], ".")))
            continue;

        /* C++: namespace/class qualifiers `X::Y` - X is never a variable */
        if (i + 1 < end && tok_is(&toks[i + 1], "::"))
            continue;
        /* C++: bare `Type name` - an identifier followed by an identifier
           is a type name (the only valid parse of `a b;` is a declaration) */
        if (i + 1 < end && toks[i + 1].type == T_IDENT)
            continue;
        /* C++: `Type & name` - reference-typed declarations */
        if (i + 2 < end && tok_is(&toks[i + 1], "&") &&
            !tok_is(&toks[i + 2], "&") && toks[i + 2].type == T_IDENT)
            continue;

        /* typedef names are types, never variables */
        if (is_typedef_name(fv, t->text))
            continue;

        /* declaration: TYPE IDENT or TYPE * IDENT (typedef-aware) */
        if (i > begin && tok_is(&toks[i - 1], "struct"))
            continue; /* struct tag, not a variable */
        if (i > begin && tok_is(&toks[i - 1], "enum"))
            continue; /* enum tag, not a variable */
        if (i > begin && tok_is(&toks[i - 1], "union"))
            continue; /* union tag, not a variable */
        int prev_is_type = (i > begin && is_type_tok(&toks[i - 1])) ||
                           (i > begin && toks[i - 1].type == T_IDENT &&
                            is_typedef_name(fv, toks[i - 1].text));
        int is_decl = prev_is_type &&
                      (i + 1 >= end || !tok_is(&toks[i + 1], "("));
        if (!is_decl && i > begin + 1 && tok_is(&toks[i - 1], "*")) {
            /* only KNOWN typedefs count here: `a * m` is arithmetic and
               must not be read as a declaration */
            prev_is_type = is_type_tok(&toks[i - 2]) ||
                           (toks[i - 2].type == T_IDENT &&
                            is_typedef_name(fv, toks[i - 2].text));
            is_decl = prev_is_type &&
                      (i + 1 >= end || !tok_is(&toks[i + 1], "("));
        }
        /* unknown-IDENT as a bare type: `node_t k;` (typedef from header) */
        if (!is_decl && i > begin && toks[i - 1].type == T_IDENT &&
            !var_get(&vars, toks[i - 1].text) &&
            !tok_is(&toks[i - 1], "return") &&
            (i + 1 >= end || !tok_is(&toks[i + 1], "(")) &&
            !tok_is(&toks[i - 1], "@nonnull"))
            is_decl = 1;
        /* `NS::Type name`: a class-type object is constructor-initialized */
        if (is_decl && i > begin + 1 && toks[i - 1].type == T_IDENT &&
            i > begin + 2 && tok_is(&toks[i - 2], "::")) {
            var_t *obj = var_add(&vars, t->text, 0);
            if (obj)
                obj->state = 4; /* constructed, not uninitialized */
            continue;
        }
        /* multi-declaration continuation: `pt_t a, b;` - the name after
           a comma whose previous name is a fresh (state 1) declaration */
        if (!is_decl && i > begin + 1 && tok_is(&toks[i - 1], ",") &&
            toks[i - 2].type == T_IDENT) {
            var_t *prev_v = var_get(&vars, toks[i - 2].text);
            if (prev_v && prev_v->state == 1 &&
                (i + 1 >= end || !tok_is(&toks[i + 1], "(")))
                is_decl = 1;
        }
        /* TYPE *... NAME / (TYPE **)cast: the current IDENT is a type
           name when a pointer-star run follows and then an identifier,
           a closing paren (cast) or an open paren (function pointer) */
        if (is_decl && i + 1 < end && tok_is(&toks[i + 1], "*")) {
            size_t s = i + 1;
            while (s < end && tok_is(&toks[s], "*"))
                s++;
            if (s < end &&
                (toks[s].type == T_IDENT || tok_is(&toks[s], ")") ||
                 tok_is(&toks[s], "(")))
                continue;
        }
        if (is_decl) {
            var_t *vt = var_add(&vars, t->text, 0);
            /* static locals are zero-initialized: scan back over
               type tokens and pointer stars for the `static` keyword */
            int is_static = 0;
            for (int j = (int)i - 1; j >= 0; j--) {
                if (tok_is(&toks[j], "static")) {
                    is_static = 1;
                    break;
                }
                if (tok_is(&toks[j], "*") || tok_is(&toks[j], "&") ||
                    is_type_tok(&toks[j]))
                    continue;
                break;
            }
            if (is_static) {
                vt->state = 4;
                continue;
            }
            if (i + 1 < end && tok_is(&toks[i + 1], "=")) {
                /* initializer: `= call(...)` -> track like an assignment */
                size_t semi = i + 2;
                while (semi < end && !tok_is(&toks[semi], ";") &&
                       !(tok_is(&toks[semi], ",") && depth == 0))
                    semi++;
                int is_alloc = 0;
                const char *call =
                    find_err_call(toks, i + 2, semi, fv, &is_alloc);
                if (call) {
                    vt->state = 2;
                    vt->alloc = is_alloc;
                    vt->assign_line = t->line;
                    vt->assign_col = t->col;
                    snprintf(vt->fn, sizeof vt->fn, "%s", call);
                    vt->reported = 0;
                } else {
                    vt->state = 4;
                }
            }
            continue;
        }

        var_t *vt = var_get(&vars, t->text);
        if (!vt)
            continue;

        /* return x; / return x, ... : propagating the error is correct */
        if (i > begin && tok_is(&toks[i - 1], "return")) {
            if (i + 1 >= end || tok_is(&toks[i + 1], ";") ||
                tok_is(&toks[i + 1], ","))
                continue;
        }
        /* truthy check: x && use(x), !x, x || ... - the expression guards */
        if (i > begin && (tok_is(&toks[i - 1], "&&") ||
                          tok_is(&toks[i - 1], "||") ||
                          tok_is(&toks[i - 1], "!") ||
                          tok_is(&toks[i - 1], "?"))) {
            if (vt->state == 2)
                vt->state = 3;
            continue;
        }
        if (i + 1 < end && (tok_is(&toks[i + 1], "&&") ||
                            tok_is(&toks[i + 1], "||") ||
                            tok_is(&toks[i + 1], "?"))) {
            if (vt->state == 2)
                vt->state = 3;
            continue;
        }
        /* sizeof x: no read of the value */
        if (i > begin && tok_is(&toks[i - 1], "sizeof"))
            continue;
        /* &x: assume external initialization */
        if (i > begin && tok_is(&toks[i - 1], "&")) {
            vt->state = 4;
            continue;
        }

        /* x = ... assignment */
        if (i + 1 < end && tok_is(&toks[i + 1], "=")) {
            size_t semi = i + 2;
            while (semi < end && !tok_is(&toks[semi], ";"))
                semi++;
            int is_alloc = 0;
            const char *call = find_err_call(toks, i + 2, semi, fv, &is_alloc);
            if (call) {
                vt->state = 2;
                vt->alloc = is_alloc;
                vt->assign_line = t->line;
                vt->assign_col = t->col;
                snprintf(vt->fn, sizeof vt->fn, "%s", call);
                vt->reported = 0;
                /* the call's arguments are inputs, not uses of the result:
                   jump past the assigning call's parens */
                for (size_t j = i + 2; j + 1 < semi; j++) {
                    if (toks[j].type == T_IDENT &&
                        tok_is(&toks[j + 1], "(") &&
                        !strcmp(toks[j].text, call)) {
                        i = match_paren(toks, j + 1, semi);
                        break;
                    }
                }
            } else {
                vt->state = 4;
            }
            continue;
        }

        /* x++, x--, x += ... : read-modify-write */
        if (i + 1 < end &&
            (tok_is(&toks[i + 1], "++") || tok_is(&toks[i + 1], "--") ||
             tok_is(&toks[i + 1], "+=") || tok_is(&toks[i + 1], "-=") ||
             tok_is(&toks[i + 1], "*=") || tok_is(&toks[i + 1], "/=") ||
             tok_is(&toks[i + 1], "&=") || tok_is(&toks[i + 1], "|="))) {
            if (vt->state == 2 && !vt->reported) {
                fnd_pushf(out, vt->alloc ? "unchecked-deref-alloc"
                                         : "missing-error-path",
                          "major", t->line, t->col,
                          "value of %s() at %d:%d used at %d:%d with no error branch (if-not)",
                          vt->fn, vt->assign_line, vt->assign_col, t->line,
                          t->col);
                vt->reported = 1;
            }
            vt->state = 4;
            continue;
        }

        /* x.y = v / x->f = v : field write -> not a read */
        if (i + 1 < end &&
            (tok_is(&toks[i + 1], ".") || tok_is(&toks[i + 1], "->"))) {
            size_t w = i + 2; /* member name */
            while (w + 1 < end && toks[w].type == T_IDENT &&
                   (tok_is(&toks[w + 1], ".") || tok_is(&toks[w + 1], "->")))
                w += 2;
            if (w + 1 < end && tok_is(&toks[w + 1], "=")) {
                if (vt->state != 2)
                    vt->state = 4; /* field write initializes usage */
                else if (!vt->reported) {
                    fnd_pushf(out, "unchecked-deref-alloc", "major", t->line,
                              t->col,
                              "value of %s() at %d:%d used at %d:%d with no error branch (if-not)",
                              vt->fn, vt->assign_line, vt->assign_col, t->line,
                              t->col);
                    vt->reported = 1;
                    vt->state = 3;
                    continue;
                }
            }
            continue;
        }

        /* x[...] = v / x[...].f = v / x[...]++ : buffer element write,
           unless the buffer itself came unchecked from an allocator */
        if (i + 1 < end && tok_is(&toks[i + 1], "[")) {
            size_t b = i + 2, d = 1;
            for (; b < end; b++) {
                if (tok_is(&toks[b], "["))
                    d++;
                else if (tok_is(&toks[b], "]")) {
                    d--;
                    if (d == 0)
                        break;
                }
            }
            size_t w = b + 1;
            while (w + 1 < end &&
                   (tok_is(&toks[w], ".") || tok_is(&toks[w], "->")) &&
                   toks[w + 1].type == T_IDENT)
                w += 2;
            if (w < end && (tok_is(&toks[w], "=") || tok_is(&toks[w], "++") ||
                            tok_is(&toks[w], "--") || tok_is(&toks[w], "+=") ||
                            tok_is(&toks[w], "-="))) {
                if (vt->state != 2)
                    vt->state = 4; /* local buffer element write */
                /* state 2 falls through: the indexed write is the first
                   dereference of an unchecked allocation */
                else if (!vt->reported) {
                    fnd_pushf(out, "unchecked-deref-alloc", "major", t->line,
                              t->col,
                              "value of %s() at %d:%d used at %d:%d with no error branch (if-not)",
                              vt->fn, vt->assign_line, vt->assign_col, t->line,
                              t->col);
                    vt->reported = 1;
                    vt->state = 3;
                    continue;
                }
            }
        }

        /* otherwise: a value use */
        if (vt->state == 2 && !vt->reported) {
            /* free() accepts NULL by contract: not a defect */
            if (!(is_call_arg(toks, i, begin) && i > begin &&
                  toks[i - 1].type == T_IDENT &&
                  !strcmp(toks[i - 1].text, "free") &&
                  !strcmp(toks[i + 1].text, "("))) {
                fnd_pushf(out, vt->alloc ? "unchecked-deref-alloc"
                                         : "missing-error-path",
                          "major", t->line, t->col,
                          "value of %s() at %d:%d used at %d:%d with no error branch (if-not)",
                          vt->fn, vt->assign_line, vt->assign_col, t->line,
                          t->col);
                vt->reported = 1;
            }
            vt->state = 3;
        } else if (vt->state == 1 && !vt->reported) {
            /* passing an uninitialized local to a function may be an output
               buffer (fread/recv/...): conservative, do not report */
            if (is_call_arg(toks, i, begin)) {
                vt->state = 3;
            } else {
                fnd_pushf(out, "uninitialized-use", "minor", t->line, t->col,
                          "%s read before assignment", t->text);
                vt->reported = 1;
                vt->state = 4;
            }
        }
    }
    vars_free(&vars);
}

void analyze_file(const char *path, tokvec_t *tv, const fnvec_t *fv,
                  findvec_t *out)
{
    (void)path;
    for (size_t i = 0; i < fv->nfn; i++) {
        fn_t *f = &fv->fns[i];
        if (f->body_end <= f->body_begin + 1)
            continue;
        analyze_body(tv->toks, f->body_begin + 1, f->body_end - 1, fv, out);
        scan_statements(tv->toks, f->body_begin + 1, f->body_end - 1, fv, out);
    }
}
