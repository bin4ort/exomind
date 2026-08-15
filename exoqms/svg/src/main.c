/* main.c — exoqms-svg CLI: batch asset-logic auditor for generated SVG.
 *
 * usage: exoqms-svg <target> [--shape tree|auto] [--json]
 *
 * target = .svg file, or directory (all .svg files recursively). Plain
 * output is one finding per line ("<severity> <check-id> <reason>"),
 * then the exact summary line "=== findings: N (M major) ===".
 * Exit 0 when there are no findings, 1 when there are, 2 on usage/IO
 * error. With --shape auto and no tree hint the file prints a "skip"
 * line and does not count (exit stays 0).
 */
#include "svg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(FILE *f)
{
    fprintf(f,
        "exoqms-svg v%s — asset-logic auditor for generated SVG (QMS).\n"
        "usage: exoqms-svg <target> [--shape tree|auto] [--json]\n"
        "  <target>            .svg file, or directory (audits all .svg\n"
        "                      files recursively)\n"
        "  --shape tree|auto   rule-set to apply; auto detects via the\n"
        "                      root <svg> data-shape attribute or a\n"
        "                      filename containing \"tree\"\n"
        "  --json              machine-readable JSON array output\n"
        "  --help              this text\n"
        "  --version           print version\n"
        "\n"
        "checks (tree rule-set):\n"
        "  stem-taper       trunk width ratio top/bottom outside [0.15, 0.9]\n"
        "  stem-missing     no trunk elements below the crown region\n"
        "  crown-roundness  crown aspect outside [0.75, 1.5], convexity < 0.8,\n"
        "                   or a box-shaped crown\n"
        "  proportions      trunk/crown height ratio or crown width/total\n"
        "                   height ratio outside their ranges (minor)\n"
        "  symmetry         crown left/right area balance < 0.6 about the\n"
        "                   trunk axis (minor)\n"
        "  empty-shape      total painted area < 0.5%% of the bbox (major)\n"
        "  fragmented       more than 8 disconnected stroke groups (minor)\n"
        "  out-of-bounds    element entirely outside the svg viewBox (minor)\n"
        "\n"
        "exit codes: 0 no findings, 1 findings, 2 usage/IO error.\n"
        "auto with no shape hint prints a 'skip' line and exits 0.\n"
        "Static analyzer with a simplified geometry model; limitations in\n"
        "exoqms/svg/README.md.\n",
        SVG_VERSION);
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void emit_json(const vec_t *findings)
{
    size_t i;
    printf("[");
    for (i = 0; i < findings->len; i++) {
        finding_t *f = findings->it[i];
        char *fs = json_escape(f->file, strlen(f->file));
        char *ss = json_escape(f->shape, strlen(f->shape));
        char *rs = json_escape(f->reason, strlen(f->reason));
        printf("%s\n{\"file\":\"%s\",\"shape\":\"%s\",\"severity\":\"%s\","
               "\"check\":\"%s\",\"reason\":\"%s\"}",
               i ? "," : "", fs, ss, f->major ? "major" : "minor",
               f->check, rs);
        free(fs);
        free(ss);
        free(rs);
    }
    printf("%s]\n", findings->len ? "\n" : "");
}

/* audit one file; findings are moved into *findings; returns 0 on
 * success. A skipped file (auto, no shape hint) prints a "skip" line
 * in plain mode and increments *n_skip. */
static int audit_file(const char *path, int shape_mode, int json, int multi,
                      vec_t *findings, int *n_skip)
{
    size_t len = 0;
    char err[256] = {0};
    char *src = file_read(path, &len, err, sizeof err);
    svgdoc_t *d;
    audit_result_t res;
    size_t i;
    if (!src) {
        fprintf(stderr, "error: %s\n", err);
        return -1;
    }
    d = svg_parse(src, len, path);
    audit_run(d, path, shape_mode, &res);
    if (multi && !json)
        printf("file: %s\n", path);
    if (res.kind == RES_SKIP) {
        if (!json)
            printf("skip unknown-shape %s: no tree hint (no data-shape=\"tree\" on root <svg>, filename has no \"tree\")\n",
                   path);
        (*n_skip)++;
    } else {
        for (i = 0; i < res.findings.len; i++)
            vec_push(findings, res.findings.it[i]);
        if (!json)
            for (i = 0; i < res.findings.len; i++) {
                finding_t *f = res.findings.it[i];
                printf("%s %s %s\n", f->major ? "major" : "minor", f->check,
                       f->reason);
            }
    }
    audit_result_free(&res);
    svg_free(d);
    free(src);
    return 0;
}

int main(int argc, char **argv)
{
    const char *target = NULL;
    const char *shape_arg = NULL;
    int shape_mode = SHAPE_AUTO;
    int json = 0;
    int i;
    vec_t findings = {0};
    int total = 0, majors = 0, n_skip = 0;
    int rc;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("exoqms-svg %s\n", SVG_VERSION);
            return 0;
        }
        if (strcmp(a, "--json") == 0) {
            json = 1;
            continue;
        }
        if (strcmp(a, "--shape") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --shape needs an argument\n");
                return 2;
            }
            shape_arg = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] == '-' && a[1] != 0) {
            fprintf(stderr, "error: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
        if (!target)
            target = a;
        else {
            fprintf(stderr, "error: exactly one target expected\n");
            return 2;
        }
    }
    if (!target) {
        usage(stderr);
        return 2;
    }
    if (shape_arg) {
        if (strcmp(shape_arg, "tree") == 0)
            shape_mode = SHAPE_TREE;
        else if (strcmp(shape_arg, "auto") == 0)
            shape_mode = SHAPE_AUTO;
        else {
            fprintf(stderr, "error: unknown shape %s (supported: tree, auto)\n",
                    shape_arg);
            return 2;
        }
    }
    if (is_dir(target)) {
        vec_t files = {0};
        size_t n;
        if (dir_walk_svg(target, &files) != 0 || files.len == 0) {
            fprintf(stderr, "error: no .svg files found under %s\n", target);
            return 2;
        }
        n = files.len;
        for (i = 0; i < (int)n; i++) {
            const char *f = files.it[i];
            if (audit_file(f, shape_mode, json, 1, &findings, &n_skip) != 0)
                fprintf(stderr, "error: skipping %s\n", f);
            free(files.it[i]);
        }
        free(files.it);
        for (i = 0; i < (int)findings.len; i++) {
            finding_t *f = findings.it[i];
            total++;
            if (f->major)
                majors++;
        }
        if (json)
            emit_json(&findings);
        else
            printf("=== findings: %d (%d major) ===\n", total, majors);
    } else if (is_file(target)) {
        if (audit_file(target, shape_mode, json, 0, &findings, &n_skip) != 0) {
            findings_free(&findings);
            return 2;
        }
        for (i = 0; i < (int)findings.len; i++) {
            finding_t *f = findings.it[i];
            total++;
            if (f->major)
                majors++;
        }
        if (json)
            emit_json(&findings);
        else
            printf("=== findings: %d (%d major) ===\n", total, majors);
    } else {
        fprintf(stderr, "error: %s: no such file or directory\n", target);
        return 2;
    }
    rc = total > 0 ? 1 : 0;
    findings_free(&findings);
    return rc;
}
