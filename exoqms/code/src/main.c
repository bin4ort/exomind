/*
 * exoqms-code: CLI - analyze .c/.h files for error-handling quality.
 *
 *   exoqms-code <file-or-dir>... [--json] [--ignore <glob>]
 *
 * Findings: <severity> <check-id> <file:line:col> <reason>
 * Summary:  === findings: N (M major) ===
 * Exit:     0 = clean, 1 = findings.
 */
#include "code.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif

#define MAX_PATHS 4096

typedef struct {
    char *path;   /* file path substring */
    int line;     /* 0 = any */
    char *check;  /* NULL = any */
} allow_t;

typedef struct {
    allow_t *a;
    size_t n;
    size_t cap;
} allowvec_t;

static int allow_load(const char *path, allowvec_t *av)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (!l || line[0] == '#')
            continue;
        char *lp = strchr(line, ':');
        int ln = 0;
        char *ck = NULL;
        if (lp) {
            *lp = 0;
            char *rest = lp + 1;
            char *ckp = strchr(rest, ':');
            if (ckp) {
                *ckp = 0;
                ck = ckp + 1;
            }
            ln = atoi(rest);
        }
        if (av->n >= av->cap) {
            av->cap = av->cap ? av->cap * 2 : 16;
            allow_t *nw = realloc(av->a, av->cap * sizeof(allow_t));
            if (!nw)
                break;
            av->a = nw;
        }
        allow_t *al = &av->a[av->n++];
        al->path = strdup(line);
        al->line = ln;
        al->check = ck ? strdup(ck) : NULL;
    }
    fclose(f);
    return (int)av->n;
}

static int allow_match(const allowvec_t *av, const char *path, int line,
                       const char *check)
{
    for (size_t i = 0; i < av->n; i++) {
        if (strstr(path, av->a[i].path) &&
            (!av->a[i].line || av->a[i].line == line) &&
            (!av->a[i].check || !strcmp(av->a[i].check, check)))
            return 1;
    }
    return 0;
}

typedef struct {
    char **items;
    size_t n;
    size_t cap;
} strvec_t;

static void sv_push(strvec_t *sv, const char *s)
{
    if (sv->n >= sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 64;
        char **nw = realloc(sv->items, sv->cap * sizeof(char *));
        if (!nw)
            return;
        sv->items = nw;
    }
    sv->items[sv->n++] = strdup(s);
}

static void sv_free(strvec_t *sv)
{
    for (size_t i = 0; i < sv->n; i++)
        free(sv->items[i]);
    free(sv->items);
    sv->items = NULL;
    sv->n = 0;
    sv->cap = 0;
}


/* per-file language for --lang auto: extension based */
static const char *lang_of(const char *p, const char *forced)
{
    if (forced && strcmp(forced, "auto") != 0)
        return forced;
    size_t n = strlen(p);
    if (n > 4 && (!strcmp(p + n - 4, ".cpp") || !strcmp(p + n - 4, ".hpp") ||
                  !strcmp(p + n - 4, ".ccx")))
        return "cpp";
    if (n > 3 && (!strcmp(p + n - 3, ".cc") || !strcmp(p + n - 3, ".hh") ||
                  !strcmp(p + n - 3, ".cxx") || !strcmp(p + n - 3, ".hxx")))
        return "cpp";
    if (n > 2 && (!strcmp(p + n - 2, ".c") || !strcmp(p + n - 2, ".h")))
        return "c";
    if (n > 3 && (!strcmp(p + n - 3, ".sh") || !strcmp(p + n - 3, ".bs")))
        return "sh";
    if (n > 3 && !strcmp(p + n - 3, ".py"))
        return "py";
    if (n > 3 && (!strcmp(p + n - 3, ".go") || !strcmp(p + n - 3, ".rs")))
        return n > 3 && !strcmp(p + n - 3, ".go") ? "go" : "rust";
    if (n > 3 && (!strcmp(p + n - 3, ".js") || !strcmp(p + n - 3, ".ts") ||
                  !strcmp(p + n - 3, ".mjs")))
        return "js";
    if (n > 5 && !strcmp(p + n - 5, ".bash"))
        return "sh";
    if (strstr(p, "Dockerfile"))
        return "docker";
    return NULL;
}

static int is_scan_file(const char *p, const char *lang)
{
    if (lang && strcmp(lang, "rules") == 0)
        return 1; /* every text file is a rules target */
    if (lang && strcmp(lang, "auto") != 0)
        return lang_of(p, lang) != NULL; /* forced language */
    return lang_of(p, NULL) != NULL;     /* auto: any supported */
}

static void walk_dir(const char *dir, strvec_t *files, const char *lang,
                     strvec_t *ignores)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, ".git") || !strcmp(e->d_name, "fixtures"))
            continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        int ign = 0;
        for (size_t j = 0; j < ignores->n && !ign; j++) {
            /* strip a trailing slash: "build/" means any build segment */
            const char *pat = ignores->items[j];
            size_t pl = strlen(pat);
            char patbuf[512];
            if (pl > 1 && pat[pl - 1] == '/') {
                snprintf(patbuf, sizeof patbuf, "%.*s", (int)(pl - 1), pat);
                pat = patbuf;
                pl--;
            }
            if (fnmatch(pat, path, 0) == 0 ||
                fnmatch(pat, e->d_name, 0) == 0) {
                ign = 1;
                break;
            }
            /* segment match: /dir/build/ or dir/build in any position */
            char seg[1024];
            snprintf(seg, sizeof seg, "/%s/", pat);
            if (strstr(path, seg)) {
                ign = 1;
                break;
            }
        }
        if (ign)
            continue;
        if (e->d_type == DT_DIR) {
            walk_dir(path, files, lang, ignores);
        } else if (is_scan_file(path, lang)) {
            sv_push(files, path);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    strvec_t paths = {0};
    strvec_t ignores = {0};
    allowvec_t allows = {0};
    int json = 0;
    const char *lang = "auto";
    const char *rules_dir_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json"))
            json = 1;
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc)
            lang = argv[++i];
        else if (!strcmp(argv[i], "--rules") && i + 1 < argc)
            rules_dir_path = argv[++i];
        else if (!strcmp(argv[i], "--ignore") && i + 1 < argc)
            sv_push(&ignores, argv[++i]);
        else if (!strcmp(argv[i], "--allow") && i + 1 < argc) {
            if (allow_load(argv[++i], &allows) < 0) {
                fprintf(stderr, "exoqms-code: cannot open allow file\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--version")) {
            printf("exoqms-code v0.4.0-alpha.1\n");
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf(
                "exoqms-code: multi-language quality analyzer\n\n"
                "usage: exoqms-code <file-or-dir>... [--lang c|cpp|sh|py|rules|auto]\n"
                "       [--rules <dir>] [--json] [--ignore <glob>] [--allow <file>]\n\n"
                "c/cpp: unchecked-return, missing-error-path, empty-error-branch,\n"
                "       uninitialized-use, swallowed-error, unchecked-deref-alloc\n"
                "sh:    shell-unquoted-rm, shell-unquoted-test, shell-no-shebang,\n"
                "       shell-cd-unchecked, shell-backtick\n"
                "py:    py-bare-except, py-mutable-default, py-assert-validation,\n"
                "       py-os-system\n"
                "go:    go-unchecked-err, go-ignored-defer (line-based, no toolchain)\n"
                "rust:  rust-unwrap, rust-expect, rust-unreachable (line-based)\n"
                "js:    js-eval, js-innerhtml, js-console-log (line-based)\n"
                "docker: docker-latest, docker-unpinned, docker-add\n"
                "--rules <dir>: generic rule engine - one rule per file, file name =\n"
                "  check id, line 1 severity, line 2 POSIX ERE matched per line.\n"
                "  Special id hygiene-no-eol flags files without trailing newline.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "exoqms-code: unknown option %s\n", argv[i]);
            return 2;
        } else {
            sv_push(&paths, argv[i]);
        }
    }
    if (paths.n == 0) {
        fprintf(stderr, "exoqms-code: no input\n");
        return 2;
    }
    if (lang && strcmp(lang, "c") != 0 && strcmp(lang, "cpp") != 0 &&
        strcmp(lang, "sh") != 0 && strcmp(lang, "py") != 0 &&
        strcmp(lang, "go") != 0 && strcmp(lang, "rust") != 0 &&
        strcmp(lang, "js") != 0 && strcmp(lang, "docker") != 0 &&
        strcmp(lang, "rules") != 0 && strcmp(lang, "auto") != 0) {
        fprintf(stderr, "exoqms-code: unknown language %s\n", lang);
        return 2;
    }
    /* rules engine: load rules once; with --rules and no explicit
       language, every text file is a target */
    rulevec_t rv = {0};
    char rerr[256] = {0};
    int rules_mode = rules_dir_path && strcmp(rules_dir_path, "-") != 0;
    if (rules_mode && strcmp(lang, "auto") == 0)
        lang = "rules";
    if (rules_mode &&
        rules_load_dir(rules_dir_path, &rv, rerr, sizeof rerr) != 0) {
        fprintf(stderr, "exoqms-code: %s\n", rerr);
        return 2;
    }

    strvec_t files = {0};
    for (size_t i = 0; i < paths.n; i++) {
        struct stat st;
        int is_dir = stat(paths.items[i], &st) == 0 && S_ISDIR(st.st_mode);
        if (is_dir) {
            walk_dir(paths.items[i], &files, lang, &ignores);
        } else if (is_scan_file(paths.items[i], lang)) {
            sv_push(&files, paths.items[i]);
        }
    }

    size_t nf = 0, n_major = 0;
    int printed_any = 0;
    if (json)
        printf("[");

    for (size_t fi = 0; fi < files.n; fi++) {
        const char *path = files.items[fi];
        int ignored = 0;
        for (size_t j = 0; j < ignores.n; j++) {
            if (fnmatch(ignores.items[j], path, 0) == 0) {
                ignored = 1;
                break;
            }
        }
        if (ignored)
            continue;

        findvec_t out = {0};
        const char *flang = rules_mode ? "rules" : lang_of(path, lang);
        if (flang && (strcmp(flang, "sh") == 0 || strcmp(flang, "py") == 0 ||
                      strcmp(flang, "go") == 0 || strcmp(flang, "rust") == 0 ||
                      strcmp(flang, "js") == 0 || strcmp(flang, "docker") == 0 ||
                      strcmp(flang, "rules") == 0)) {
            FILE *f = fopen(path, "rb");
            if (f) {
                static const size_t CAP = 16u * 1024u * 1024u;
                char *buf = malloc(CAP + 1);
                if (buf) {
                    size_t n = fread(buf, 1, CAP, f);
                    if (n == 0 && ferror(f)) {
                        /* read error, not EOF: skip the file */
                        free(buf);
                        fclose(f);
                        continue;
                    }
                    analyze_text_file(path, flang, buf, n,
                                      rules_mode ? &rv : NULL, &out);
                    free(buf);
                }
                fclose(f);
            }
        } else if (flang && (strcmp(flang, "c") == 0 || strcmp(flang, "cpp") == 0)) {
            tokvec_t tv;
            tokvec_init(&tv);
            if (tokenize_file(path, &tv) == 0) {
                fnvec_t fv = {0};
                collect_functions(&tv, &fv);
                analyze_file(path, &tv, &fv, &out);
                fnvec_free(&fv);
            }
            tokvec_free(&tv);
        }

        for (size_t k = 0; k < out.nf; k++) {
            finding_t *f = &out.f[k];
            if (allow_match(&allows, path, f->line, f->check)) {
                if (!json)
                    printf("skip %s %s:%d:%d (allowlisted)\n", f->check,
                           path, f->line, f->col);
                continue;
            }
            if (json) {
                printf("%s{\"check\":\"%s\",\"severity\":\"%s\",\"file\":\"%s\",\"line\":%d,\"col\":%d,\"reason\":\"",
                       printed_any ? "," : "", f->check, f->severity,
                       path, f->line, f->col);
                for (const char *r = f->reason; *r; r++) {
                    unsigned char c = (unsigned char)*r;
                    if (c == '"' || c == '\\')
                        printf("\\%c", c);
                    else if (c == '\n')
                        printf("\\n");
                    else if (c == '\t')
                        printf("\\t");
                    else if (c < 0x20)
                        printf("\\u%04x", c);
                    else
                        putchar(c);
                }
                printf("\"}");
                printed_any = 1;
            } else {
                printf("%s %s %s:%d:%d %s\n", f->severity, f->check, path,
                       f->line, f->col, f->reason);
            }
            if (!strcmp(f->severity, "major"))
                n_major++;
            nf++;
        }
        findvec_free(&out);
    }
    rules_free_all(&rv);

    if (json)
        printf("]\n");
    else
        printf("=== findings: %zu (%zu major) ===\n", nf, n_major);
    sv_free(&files);
    sv_free(&paths);
    sv_free(&ignores);
    for (size_t i = 0; i < allows.n; i++) {
        free(allows.a[i].path);
        free(allows.a[i].check);
    }
    free(allows.a);
    return nf ? 1 : 0;
}
