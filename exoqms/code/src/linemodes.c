/*
 * exoqms-code: line-based adapters (shell, python) and the generic
 * text rule engine. Zero dependencies. Rule files: one rule per file,
 * file name = check id; line 1 = severity (major|minor), line 2 = POSIX
 * extended regex matched per line. The special rule id `hygiene-no-eol`
 * is built in: it flags files whose last byte is not a newline.
 */
#include "code.h"

#include <ctype.h>
#include <dirent.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int regex_search(const char *line, const char *pat, size_t *col)
{
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0)
        return 0;
    regmatch_t m;
    int rc = regexec(&re, line, 1, &m, 0);
    if (rc == 0 && col)
        *col = (size_t)m.rm_so + 1;
    regfree(&re);
    return rc == 0;
}

static void fnd_add(findvec_t *out, const char *check, const char *sev,
                    int line, int col, const char *reason)
{
    if (out->nf >= out->cap) {
        out->cap = out->cap ? out->cap * 2 : 64;
        finding_t *nw = realloc(out->f, out->cap * sizeof(finding_t));
        if (!nw)
            return;
        out->f = nw;
    }
    finding_t *fd = &out->f[out->nf++];
    fd->check = check;
    fd->severity = sev;
    fd->line = line;
    fd->col = col;
    snprintf(fd->reason, sizeof fd->reason, "%s", reason);
}

/* ---------------- rules engine ---------------- */

static void rules_free(rulevec_t *rv)
{
    for (size_t i = 0; i < rv->n; i++)
        regfree(&rv->r[i].re);
    free(rv->r);
    rv->r = NULL;
    rv->n = 0;
    rv->cap = 0;
}

int rules_load_dir(const char *dir, void *rv_out, char *err, size_t errsz)
{
    rulevec_t *rv = rv_out;
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errsz, "cannot open rules dir %s", dir);
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char sev[64] = {0};
        char pat[4096] = {0};
        if (fgets(sev, sizeof sev, f) && fgets(pat, sizeof pat, f)) {
            sev[strcspn(sev, "\r\n")] = 0;
            pat[strcspn(pat, "\r\n")] = 0;
            if (rv->n >= rv->cap) {
                rv->cap = rv->cap ? rv->cap * 2 : 16;
                rule_t *nw = realloc(rv->r, rv->cap * sizeof(rule_t));
                if (!nw) {
                    fclose(f);
                    closedir(d);
                    return -1;
                }
                rv->r = nw;
            }
            rule_t *rl = &rv->r[rv->n++];
            memset(rl, 0, sizeof *rl);
            snprintf(rl->check, sizeof rl->check, "%.63s", e->d_name);
            snprintf(rl->severity, sizeof rl->severity, "%.7s",
                     sev[0] ? sev : "minor");
            if (strcmp(rl->check, "hygiene-no-eol") == 0) {
                rl->builtin_noeol = 1;
            } else {
                if (regcomp(&rl->re, pat, REG_EXTENDED | REG_NEWLINE) != 0) {
                    snprintf(err, errsz, "bad regex in rule %s", e->d_name);
                    fclose(f);
                    closedir(d);
                    return -1;
                }
            }
        }
        fclose(f);
    }
    closedir(d);
    if (rv->n == 0) {
        snprintf(err, errsz, "no rules found in %s", dir);
        return -1;
    }
    return 0;
}

/* match every rule against every line; the no-eol rule runs once at
 * the end (the last line without a trailing newline). */
static void match_line_rules(const char *path, const char *buf, size_t n,
                             const rulevec_t *rv, findvec_t *out)
{
    (void)path;
    size_t pos = 0, ln = 1;
    int noeol = n > 0 && buf[n - 1] != '\n';
    while (pos < n) {
        size_t eol = pos;
        while (eol < n && buf[eol] != '\n')
            eol++;
        size_t len = eol - pos;
        char *line = strndup(buf + pos, len);
        for (size_t i = 0; i < rv->n; i++) {
            rule_t *rl = &rv->r[i];
            if (rl->builtin_noeol)
                continue;
            regmatch_t m;
            if (regexec(&rl->re, line, 1, &m, 0) == 0) {
                char reason[256];
                size_t rl_ = m.rm_eo - m.rm_so;
                if (rl_ > 100)
                    rl_ = 100;
                snprintf(reason, sizeof reason, "%.*s", (int)rl_,
                         line + m.rm_so);
                fnd_add(out, rl->check, rl->severity, (int)ln,
                        (int)m.rm_so + 1, reason);
            }
        }
        ln++;
        free(line);
        pos = eol < n ? eol + 1 : eol;
    }
    if (noeol) {
        for (size_t i = 0; i < rv->n; i++) {
            if (rv->r[i].builtin_noeol)
                fnd_add(out, rv->r[i].check, rv->r[i].severity, (int)ln, 1,
                        "file does not end with a newline");
        }
    }
}

/* ---------------- shell adapter ---------------- */

static void shell_scan(const char *buf, size_t n, findvec_t *out)
{
    size_t pos = 0, ln = 1;
    int first = 1;
    while (pos < n) {
        size_t eol = pos;
        while (eol < n && buf[eol] != '\n')
            eol++;
        size_t len = eol - pos;
        char *line = strndup(buf + pos, len);
        if (first && len > 0 && !(line[0] == '#' && len > 1 && line[1] == '!'))
            fnd_add(out, "shell-no-shebang", "minor", 1, 1, "no shebang");
        first = 0;
        size_t c;
        if (regex_search(line, "rm[[:space:]]+-[a-zA-Z]*[rf][a-zA-Z]*[[:space:]]+\\$", &c))
            fnd_add(out, "shell-unquoted-rm", "major", (int)ln, (int)c,
                    "rm with unquoted variable");
        if (regex_search(line, "\\[\\[[[:space:]]+\\$", &c) ||
            regex_search(line, "\\[[[:space:]]+\\$", &c))
            fnd_add(out, "shell-unquoted-test", "minor", (int)ln, (int)c,
                    "unquoted variable in [ ] test");
        if (regex_search(line, "^[[:space:]]*cd[[:space:]]+[^&|;]*$", &c))
            fnd_add(out, "shell-cd-unchecked", "minor", (int)ln, (int)c,
                    "cd without failure handling");
        if (strstr(line, "`"))
            fnd_add(out, "shell-backtick", "minor", (int)ln,
                    (int)(strstr(line, "`") - line) + 1,
                    "backticks instead of $()");
        ln++;
        free(line);
        pos = eol < n ? eol + 1 : eol;
    }
}

/* ---------------- python adapter ---------------- */

static void python_scan(const char *buf, size_t n, findvec_t *out)
{
    size_t pos = 0, ln = 1;
    while (pos < n) {
        size_t eol = pos;
        while (eol < n && buf[eol] != '\n')
            eol++;
        size_t len = eol - pos;
        char *line = strndup(buf + pos, len);
        if (regex_search(line, "^[[:space:]]*except[[:space:]]*:", NULL))
            fnd_add(out, "py-bare-except", "major", (int)ln, 1,
                    "bare except: catches every error silently");
        if (strstr(line, "def ") && (strstr(line, "=[]") ||
                                     strstr(line, "={}") ||
                                     strstr(line, "=set(")))
            fnd_add(out, "py-mutable-default", "minor", (int)ln, 1,
                    "mutable default argument");
        size_t c;
        if (regex_search(line, "^[[:space:]]*assert[[:space:]]+", &c))
            fnd_add(out, "py-assert-validation", "minor", (int)ln, (int)c,
                    "assert in production code (vanishes with -O)");
        if (strstr(line, "os.system(") || strstr(line, "os.popen("))
            fnd_add(out, "py-os-system", "minor", (int)ln, 1,
                    "os.system/os.popen: prefer subprocess");
        ln++;
        free(line);
        pos = eol < n ? eol + 1 : eol;
    }
}

/* is the file text (binary detection: NUL byte in the first 8 KiB)? */
static int looks_binary(const char *buf, size_t n)
{
    size_t lim = n < 8192 ? n : 8192;
    for (size_t i = 0; i < lim; i++)
        if (buf[i] == 0)
            return 1;
    return 0;
}

/* entry point: analyze one text file by language. lang: "sh", "py",
 * or "rules" (rv must hold the loaded rules). Returns 0. */
int analyze_text_file(const char *path, const char *lang, const char *buf,
                      size_t n, void *rv, findvec_t *out)
{
    (void)path;
    if (looks_binary(buf, n))
        return 0;
    if (strcmp(lang, "sh") == 0)
        shell_scan(buf, n, out);
    else if (strcmp(lang, "py") == 0)
        python_scan(buf, n, out);
    else if (strcmp(lang, "rules") == 0 && rv)
        match_line_rules(path, buf, n, rv, out);
    return 0;
}

void rules_free_all(void *rv)
{
    rules_free(rv);
}
