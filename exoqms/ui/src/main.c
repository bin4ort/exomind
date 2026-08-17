/* main.c — exoqms-ui CLI: batch UI quality auditor.
 *
 * usage: exoqms-ui <target> [--json] [--no-emoji] [--emoji-allowlist <chars>]
 *
 * target = HTML file, or directory (all .html files recursively, each with
 * its linked .css files and <style> blocks). Plain output is one finding
 * per line, then the exact summary line "=== findings: N (M major) ===".
 * Exit 0 when there are no findings, 1 when there are, 2 on usage/IO error.
 */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *f)
{
    fprintf(f,
        "exoqms-ui v%s — UI quality auditor for the exomind stack.\n"
        "usage: exoqms-ui <target> [options]\n"
        "  <target>            HTML file, or directory (audits all .html +\n"
        "                      their linked .css files recursively)\n"
        "  --json              machine-readable JSON array output\n"
        "  --no-emoji          skip the emoji-icon check\n"
        "  --emoji-allowlist <chars>\n"
        "                      allowlist of emoji characters (default empty)\n"
        "  --help              this text\n"
        "  --version           print version\n"
        "operation (same /op?k=v console grammar as the stack):\n"
        "  exoqms-ui /audit?file=<target>&json=1&no_emoji=1&\n"
        "             emoji_allowlist=<chars>\n"
        "\n"
        "checks:\n"
        "  emoji-icon       emoji in interactive/icon contexts (use SVG icons)\n"
        "  overlap          intersecting interactive element boxes\n"
        "  misalign         siblings that should share an edge but don't\n"
        "  corner-mismatch  rounded corner meets square neighbor on a shared edge\n"
        "  background       no affordance, bg == page bg, off-palette colors\n"
        "  sdk-default      interactive element with zero CSS rules targeting it\n"
        "  contrast         text vs background below WCAG AA 4.5:1 (3:1 large)\n"
        "\n"
        "exit codes: 0 no findings, 1 findings, 2 usage/IO error.\n"
        "Static analyzer with a simplified layout model; geometry findings\n"
        "are approximate when marked. See exoqms/ui/README.md.\n",
        EXOQMS_VERSION);
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

/* strip the last path component ("/a/b/x.html" -> "/a/b") */
static void path_dir(const char *path, char *out, size_t outsz)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, outsz, ".");
        return;
    }
    if (slash == path)
        snprintf(out, outsz, "/");
    else
        snprintf(out, outsz, "%.*s", (int)(slash - path), path);
}

/* resolve a possibly relative css href against the html's directory,
 * handling "." and ".." path components */
static void join_path(const char *dir, const char *name, char *out,
                      size_t outsz)
{
    char *parts[64];
    int nparts = 0;
    char *copy = xstrdup(name);
    char *t;
    size_t dlen = strlen(dir);
    buf_t b = {0};
    int i;
    snprintf(out, outsz, "%s", dir);
    t = strtok(copy, "/");
    while (t) {
        if (strcmp(t, ".") == 0 || strcmp(t, "") == 0) {
            t = strtok(NULL, "/");
            continue;
        }
        if (strcmp(t, "..") == 0) {
            if (nparts > 0)
                nparts--;
        } else if (nparts < 64) {
            parts[nparts++] = xstrdup(t);
        }
        t = strtok(NULL, "/");
    }
    free(copy);
    buf_puts(&b, dir);
    if (dlen > 0 && b.len > 0 && b.p[b.len - 1] != '/')
        buf_puts(&b, "/");
    for (i = 0; i < nparts; i++) {
        buf_puts(&b, parts[i]);
        free(parts[i]);
        if (i + 1 < nparts)
            buf_puts(&b, "/");
    }
    snprintf(out, outsz, "%s", b.p ? b.p : ".");
    buf_free(&b);
}

static int tag_is_name(const node_t *n, const char *name)
{
    return n->tag && strcmp(n->tag, name) == 0;
}

/* collect <style> contents and linked stylesheets into css_buf */
static void css_links(node_t *n, const char *html_path, buf_t *css_buf)
{
    size_t i;
    if (n->tag) {
        if (tag_is_name(n, "style")) {
            size_t k;
            for (k = 0; k < n->nkids; k++)
                if (n->kids[k]->text)
                    buf_puts(css_buf, n->kids[k]->text);
            buf_append(css_buf, "\n", 1);
        } else if (tag_is_name(n, "link")) {
            const char *rel = node_attr(n, "rel");
            const char *href = node_attr(n, "href");
            if (rel && href && strstr(rel, "stylesheet")) {
                char path[4096];
                char dir[4096];
                size_t len = 0;
                char err[256] = {0};
                char *data;
                int remote = strncmp(href, "http://", 7) == 0 ||
                             strncmp(href, "https://", 8) == 0 ||
                             strncmp(href, "//", 2) == 0 ||
                             strncmp(href, "data:", 5) == 0;
                if (!remote) {
                    path_dir(html_path, dir, sizeof dir);
                    join_path(dir, href, path, sizeof path);
                    data = file_read(path, &len, err, sizeof err);
                    if (data) {
                        buf_append(css_buf, data, len);
                        buf_append(css_buf, "\n", 1);
                        free(data);
                    } else {
                        fprintf(stderr, "note: %s: %s\n", html_path, err);
                    }
                } else {
                    fprintf(stderr, "note: %s: skipping remote css <%s>\n",
                            html_path, href);
                }
            }
        }
    }
    for (i = 0; i < n->nkids; i++)
        css_links(n->kids[i], html_path, css_buf);
}

/* audit one html file; returns 0 on success (findings may be empty) */
static int audit_file(const char *path, int json, const char *emoji_allow,
                      int no_emoji, int multi, vec_t *findings)
{
    size_t hlen = 0;
    char err[256] = {0};
    char *html = file_read(path, &hlen, err, sizeof err);
    int perr = 0;
    node_t *root;
    layout_t L;
    buf_t cssbuf = {0};
    css_t css;
    vec_t outs = {0};
    size_t i;
    if (!html) {
        fprintf(stderr, "error: %s\n", err);
        return -1;
    }
    root = html_parse(html, hlen, &perr);
    css_links(root, path, &cssbuf);
    css = css_parse(cssbuf.p ? cssbuf.p : "", cssbuf.len);
    memset(&L, 0, sizeof L);
    layout_build(&L, root, &css);
    checks_run(&L, path, emoji_allow, no_emoji, &outs);
    for (i = 0; i < outs.len; i++)
        vec_push(findings, outs.it[i]);
    free(outs.it);
    if (css.skipped_at || css.skipped_rules) {
        fprintf(stderr,
                "note: %s: %d @-rule(s) and %d rule(s) skipped "
                "(unsupported syntax)\n",
                path, css.skipped_at, css.skipped_rules);
    }
    if (cssbuf.p)
        buf_free(&cssbuf);
    css_free(&css);
    free(L.st);
    free(L.comp);
    html_free(root);
    free(html);
    (void)json;
    (void)multi;
    return 0;
}

static void emit_json(const vec_t *findings)
{
    size_t i;
    printf("[");
    for (i = 0; i < findings->len; i++) {
        finding_t *f = findings->it[i];
        char *fs = json_escape(f->file, strlen(f->file));
        char *ss = json_escape(f->sel, strlen(f->sel));
        char *rs = json_escape(f->reason, strlen(f->reason));
        printf("%s\n{\"file\":\"%s\",\"line\":%d,\"severity\":\"%s\","
               "\"check\":\"%s\",\"selector\":\"%s\",\"reason\":\"%s\"}",
               i ? "," : "", fs, f->line, f->major ? "major" : "minor",
               f->check, ss, rs);
        free(fs);
        free(ss);
        free(rs);
    }
    printf("%s]\n", findings->len ? "\n" : "");
}

static void emit_plain(const vec_t *findings, int multi)
{
    size_t i;
    const char *curfile = NULL;
    for (i = 0; i < findings->len; i++) {
        finding_t *f = findings->it[i];
        if (multi && (!curfile || strcmp(curfile, f->file) != 0)) {
            printf("file: %s\n", f->file);
            curfile = f->file;
        }
        printf("%s %s %s %s\n", f->major ? "major" : "minor", f->check,
               f->sel, f->reason);
    }
}

static int is_op(const char *a)
{
    return a[0] == '/' && (strchr(a, '?') != NULL || !strcmp(a, "/audit"));
}

static int flag_is_off(const char *v)
{
    return !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no") ||
           !strcmp(v, "off");
}

/* console-mode operation: /audit?file=<target>&json=1&no_emoji=1&
 * emoji_allowlist=<chars> — the /op?k=v grammar shared by the stack,
 * mapped onto the target-file audit. Values live in the static buf. */
static int console_op(const char *spec, char *av[], int max)
{
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s", spec);
    char *q = strchr(buf, '?');
    if (q)
        *q = 0;
    if (strcmp(buf, "/audit") != 0) {
        fprintf(stderr, "exoqms-ui: unknown operation %s\n", buf);
        return -1;
    }
    int n = 0;
    if (n >= max)
        return -1;
    av[n++] = "";
    if (q) {
        for (char *kv = strtok(q + 1, "&"); kv; kv = strtok(NULL, "&")) {
            char *eq = strchr(kv, '=');
            if (eq)
                *eq = 0;
            const char *k = kv;
            const char *v = eq ? eq + 1 : "1";
            if (!strcmp(k, "file")) {
                if (n + 1 >= max)
                    return -1;
                av[n++] = (char *)v;
            } else if (!strcmp(k, "json")) {
                if (!flag_is_off(v)) {
                    if (n + 1 >= max)
                        return -1;
                    av[n++] = "--json";
                }
            } else if (!strcmp(k, "no_emoji")) {
                if (!flag_is_off(v)) {
                    if (n + 1 >= max)
                        return -1;
                    av[n++] = "--no-emoji";
                }
            } else if (!strcmp(k, "emoji_allowlist")) {
                if (n + 2 >= max)
                    return -1;
                av[n++] = "--emoji-allowlist";
                av[n++] = (char *)v;
            } else {
                fprintf(stderr, "exoqms-ui: /audit: unknown parameter %s\n",
                        k);
                return -1;
            }
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && is_op(argv[1])) {
        static char *opav[32];
        int on = console_op(argv[1], opav, 32);
        if (on < 0)
            return 2;
        argv = opav;
        argc = on;
    }
    const char *target = NULL;
    const char *emoji_allow = NULL;
    int json = 0;
    int no_emoji = 0;
    int multi_mode = 0;
    int i, j;
    vec_t findings = {0};
    int total = 0;
    int majors = 0;
    int rc;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("exoqms-ui %s\n", EXOQMS_VERSION);
            return 0;
        }
        if (strcmp(a, "--json") == 0) {
            json = 1;
            continue;
        }
        if (strcmp(a, "--no-emoji") == 0) {
            no_emoji = 1;
            continue;
        }
        if (strcmp(a, "--emoji-allowlist") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --emoji-allowlist needs an argument\n");
                return 2;
            }
            emoji_allow = argv[++i];
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
    if (is_dir(target)) {
        vec_t files = {0};
        size_t n;
        dir_walk_html(target, &files);
        n = files.len;
        if (n == 0) {
            fprintf(stderr, "error: no .html files found under %s\n", target);
            return 2;
        }
        multi_mode = 1;
        for (i = 0; i < (int)n; i++) {
            const char *f = files.it[i];
            vec_t per = {0};
            if (audit_file(f, json, emoji_allow, no_emoji, multi_mode, &per) != 0) {
                fprintf(stderr, "error: skipping %s\n", f);
            } else if (json) {
                for (j = 0; j < (int)per.len; j++)
                    vec_push(&findings, per.it[j]);
            } else {
                size_t k;
                printf("file: %s\n", f);
                for (k = 0; k < per.len; k++) {
                    finding_t *fd = per.it[k];
                    total++;
                    if (fd->major)
                        majors++;
                    printf("%s %s %s %s\n", fd->major ? "major" : "minor",
                           fd->check, fd->sel, fd->reason);
                }
            }
            free(per.it);
            free(files.it[i]);
        }
        free(files.it);
        if (!json)
            printf("=== findings: %d (%d major) ===\n", total, majors);
    } else if (is_file(target)) {
        if (audit_file(target, json, emoji_allow, no_emoji, 0, &findings) != 0)
            return 2;
        for (i = 0; i < (int)findings.len; i++) {
            finding_t *f = findings.it[i];
            total++;
            if (f->major)
                majors++;
        }
        if (json)
            emit_json(&findings);
        else {
            emit_plain(&findings, 0);
            printf("=== findings: %d (%d major) ===\n", total, majors);
        }
    } else {
        fprintf(stderr, "error: %s: no such file or directory\n", target);
        return 2;
    }
    rc = total > 0 ? 1 : 0;
    findings_free(&findings);
    return rc;
}
