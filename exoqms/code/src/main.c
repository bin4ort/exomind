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

static int is_c_file(const char *p)
{
    size_t n = strlen(p);
    return (n > 2 && !strcmp(p + n - 2, ".c")) ||
           (n > 2 && !strcmp(p + n - 2, ".h"));
}

static void walk_dir(const char *dir, strvec_t *files)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            walk_dir(path, files);
        } else if (is_c_file(e->d_name)) {
            sv_push(files, path);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    strvec_t paths = {0};
    strvec_t ignores = {0};
    int json = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json"))
            json = 1;
        else if (!strcmp(argv[i], "--ignore") && i + 1 < argc)
            sv_push(&ignores, argv[++i]);
        else if (!strcmp(argv[i], "--version")) {
            printf("exoqms-code v0.1.0\n");
            return 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf(
                "exoqms-code: static C analyzer for error-handling quality\n\n"
                "usage: exoqms-code <file-or-dir>... [--json] [--ignore <glob>]\n\n"
                "checks:\n"
                "  unchecked-return       result of an error-returning call dropped\n"
                "  missing-error-path     error result used without an if-not branch\n"
                "  empty-error-branch     failure branch present but empty\n"
                "  uninitialized-use      local read before assignment\n"
                "  swallowed-error        if (err != 0) { } - error eaten\n"
                "  unchecked-deref-alloc  malloc family result dereferenced unguarded\n");
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

    strvec_t files = {0};
    for (size_t i = 0; i < paths.n; i++) {
        if (is_c_file(paths.items[i])) {
            sv_push(&files, paths.items[i]);
        } else {
            walk_dir(paths.items[i], &files);
        }
    }

    size_t nf = 0, n_major = 0;
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

        tokvec_t tv;
        tokvec_init(&tv);
        if (tokenize_file(path, &tv) != 0) {
            tokvec_free(&tv);
            continue;
        }
        fnvec_t fv = {0};
        collect_functions(&tv, &fv);
        findvec_t out = {0};
        analyze_file(path, &tv, &fv, &out);

        for (size_t k = 0; k < out.nf; k++) {
            finding_t *f = &out.f[k];
            if (json) {
                printf("%s{\"check\":\"%s\",\"severity\":\"%s\",\"file\":\"%s\",\"line\":%d,\"col\":%d,\"reason\":\"%s\"}",
                       k == 0 && fi == 0 ? "" : ",", f->check, f->severity,
                       path, f->line, f->col, f->reason);
            } else {
                printf("%s %s %s:%d:%d %s\n", f->severity, f->check, path,
                       f->line, f->col, f->reason);
            }
            if (!strcmp(f->severity, "major"))
                n_major++;
            nf++;
        }
        findvec_free(&out);
        fnvec_free(&fv);
        tokvec_free(&tv);
    }

    if (json)
        printf("]\n");
    else
        printf("=== findings: %zu (%zu major) ===\n", nf, n_major);
    sv_free(&files);
    sv_free(&paths);
    sv_free(&ignores);
    return nf ? 1 : 0;
}
