/* project.c: <repo>/.exoqms.json — the universal project configuration.
 * Any project can configure its own QMS without touching the daemon:
 * rule toggles, thresholds, whole-project test commands, ignore globs,
 * language override and required docs. All keys are optional; absent
 * keys fall back to defaults. Malformed JSON never crashes the daemon:
 * it warns to stderr and keeps the defaults. */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void pcfg_defaults(pcfg_t *p)
{
    memset(p, 0, sizeof *p);
    p->rules_debt = 1;
    p->rules_hygiene = 1;
    p->rules_secrets = 1;
    p->rules_codesafety = 1;
    p->debt_threshold = 10;
}

void pcfg_free(pcfg_t *p)
{
    for (size_t i = 0; i < p->n_test; i++)
        free(p->test_cmds[i]);
    free(p->test_cmds);
    for (size_t i = 0; i < p->n_docs; i++)
        free(p->docs[i]);
    free(p->docs);
    for (size_t i = 0; i < p->n_ignore; i++)
        free(p->ignore[i]);
    free(p->ignore);
    for (size_t i = 0; i < p->n_languages; i++)
        free(p->languages[i]);
    free(p->languages);
    memset(p, 0, sizeof *p);
}

static char **read_str_array(const char *json, size_t len, size_t *n)
{
    *n = 0;
    if (!json)
        return NULL;
    return json_arr_strings(json, len, n);
}

/* a boolean field inside a nested object (rules.* / thresholds.*) */
static int obj_bool(const char *json, size_t len, const char *field,
                    int dflt)
{
    char *raw = json_field(json, len, field);
    if (!raw)
        return dflt;
    int v = (strcmp(raw, "false") == 0) ? 0 : 1;
    free(raw);
    return v;
}

int pcfg_load(pcfg_t *p, const char *repo)
{
    pcfg_defaults(p);
    char path[2048];
    snprintf(path, sizeof path, "%s/.exoqms.json", repo);
    FILE *f = fopen(path, "r");
    if (!f)
        return 1; /* absent: defaults */
    size_t cap = 0, len = 0;
    char *buf = NULL;
    char chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + got + 1 > cap) {
            cap = (len + got + 1) * 2;
            buf = xrealloc(buf, cap);
        }
        memcpy(buf + len, chunk, got);
        len += got;
    }
    fclose(f);
    if (buf)
        buf[len] = 0;

    if (!buf || len == 0) {
        free(buf);
        fprintf(stderr, "exoqms: warning: %s empty or unreadable; using "
                        "defaults\n", path);
        return -1;
    }

    /* rules: {"debt": true, "hygiene": true, "secrets": true,
     * "code-safety": true} */
    char *rules = json_field(buf, len, "rules");
    if (rules) {
        p->rules_debt = obj_bool(rules, strlen(rules), "debt",
                                 p->rules_debt);
        p->rules_hygiene = obj_bool(rules, strlen(rules), "hygiene",
                                    p->rules_hygiene);
        p->rules_secrets = obj_bool(rules, strlen(rules), "secrets",
                                    p->rules_secrets);
        p->rules_codesafety = obj_bool(rules, strlen(rules), "code-safety",
                                       p->rules_codesafety);
        free(rules);
    }

    /* thresholds: {"debt": N} — a non-negative number only */
    char *thr = json_field(buf, len, "thresholds");
    if (thr) {
        char *d = json_field(thr, strlen(thr), "debt");
        if (d) {
            char *end = NULL;
            long v = strtol(d, &end, 10);
            if (end != d && *end == 0 && v >= 0 && v < 1000000)
                p->debt_threshold = (int)v;
            free(d);
        }
        free(thr);
    }

    /* string arrays: test / docs / ignore / languages */
    char *raw = json_field(buf, len, "test");
    p->test_cmds = read_str_array(raw, raw ? strlen(raw) : 0, &p->n_test);
    free(raw);
    raw = json_field(buf, len, "docs");
    p->docs = read_str_array(raw, raw ? strlen(raw) : 0, &p->n_docs);
    free(raw);
    raw = json_field(buf, len, "ignore");
    p->ignore = read_str_array(raw, raw ? strlen(raw) : 0, &p->n_ignore);
    free(raw);
    raw = json_field(buf, len, "languages");
    p->languages = read_str_array(raw, raw ? strlen(raw) : 0, &p->n_languages);
    free(raw);

    free(buf);
    return 0;
}
