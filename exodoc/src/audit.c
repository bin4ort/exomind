/* exodoc audit core: manifest, doc scanning, checks, scoring, reporting. */
#include "exodoc.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *const METHODS[] = {"GET",   "POST", "PUT",  "DELETE",
                                      "PATCH", "HEAD", "OPTIONS"};
#define NMETHODS 7

typedef struct {
    size_t off; /* line start */
    size_t end; /* index of '\n' (or len) */
    int level;
    char norm[128];
    char raw[128];
} heading_t;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int word_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

static int is_ws_char(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* substring search over a buffer range */
static int range_str(const char *buf, size_t start, size_t end,
                     const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0 || nl > end - start)
        return 0;
    for (size_t i = start; i + nl <= end; i++)
        if (memcmp(buf + i, needle, nl) == 0)
            return 1;
    return 0;
}

/* read a file, stripping control bytes (NUL, etc), capping at `cap` */
static char *read_sanitized(const char *path, size_t *len, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    buf_t b = {0};
    char tmp[65536];
    size_t got;
    while (b.len < cap && (got = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (b.len + got > cap)
            got = cap - b.len;
        for (size_t i = 0; i < got; i++)
            if ((unsigned char)tmp[i] < 0x20 && tmp[i] != '\n' &&
                tmp[i] != '\r' && tmp[i] != '\t')
                tmp[i] = ' ';
        buf_put(&b, tmp, got);
    }
    fclose(f);
    *len = b.len;
    return b.p ? b.p : xstrdup("");
}

static void add_check(comp_t *c, const char *id, ck_status_t st,
                      const char *fmt, ...)
{
    if (c->nchecks >= NCHECK_MAX)
        return;
    check_t *ck = &c->checks[c->nchecks++];
    snprintf(ck->id, sizeof ck->id, "%s", id);
    ck->st = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ck->detail, sizeof ck->detail, fmt, ap);
    va_end(ap);
}

/* append text to a check's detail without overlapping snprintf sources */
static void detail_append(check_t *ck, const char *fmt, ...)
{
    char app[DETAIL_MAX];
    char tmp[DETAIL_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app, sizeof app, fmt, ap);
    va_end(ap);
    snprintf(tmp, sizeof tmp, "%.400s%.100s", ck->detail, app);
    snprintf(ck->detail, sizeof ck->detail, "%s", tmp);
}

static void comp_count(comp_t *c)
{    c->pass = c->fail = c->skip = 0;
    for (size_t i = 0; i < c->nchecks; i++) {
        switch (c->checks[i].st) {
        case CK_PASS: c->pass++; break;
        case CK_FAIL: c->fail++; break;
        case CK_SKIP: c->skip++; break;
        }
    }
    if (c->pass + c->fail == 0)
        c->score = 0;
    else
        c->score = (int)((c->pass * 100 + (c->pass + c->fail) / 2) /
                         (c->pass + c->fail));
}

/* ---------------- manifest ---------------- */

static char *tsv_field(char *line, size_t ll, size_t *n)
{
    char *t = line;
    size_t i = 0;
    while (i < ll && line[i] != '\t')
        i++;
    *n = i;
    return t;
}

int manifest_parse(const char *path, stack *st)
{
    size_t len = 0;
    char *body = read_sanitized(path, &len, MANIFEST_MAX);
    if (!body) {
        free(body);
        return -1;
    }
    size_t line = 1, i = 0;
    while (i < len && st->n < NCOMP_MAX) {
        size_t e = i;
        while (e < len && body[e] != '\n')
            e++;
        size_t nl = e < len ? e + 1 : e;
        char *l = body + i;
        size_t ll = e - i;
        while (ll > 0 && (l[ll - 1] == '\r' || l[ll - 1] == '\n'))
            l[--ll] = 0;
        char *c = l;
        while (*c == ' ' || *c == '\t')
            c++;
        if (*c != 0 && *c != '#') {
            char *f[6] = {0};
            size_t fl = 0, pos = 0, nf = 0;
            while (nf < 6 && pos <= ll) {
                char *t = tsv_field(l + pos, ll - pos, &fl);
                char *tf = xmalloc(fl + 1);
                memcpy(tf, t, fl);
                tf[fl] = 0;
                f[nf++] = tf;
                if (pos + fl >= ll)
                    break;
                pos += fl + 1;
            }
            if (nf < 3) {
                fprintf(stderr, "warning: %s:%zu: malformed line "
                                "(need name<TAB>dir<TAB>port)\n",
                        path, line);
            } else {
                comp_t *cp = &st->comps[st->n];
                memset(cp, 0, sizeof *cp);
                snprintf(cp->name, sizeof cp->name, "%s", f[0]);
                snprintf(cp->dir, sizeof cp->dir, "%s", f[1]);
                if (f[2][0]) {
                    cp->port = atoi(f[2]);
                    if (cp->port <= 0 || cp->port > 65535) {
                        fprintf(stderr,
                                "warning: %s:%zu: bad port '%s' (line "
                                "skipped)\n",
                                path, line, f[2]);
                        memset(cp, 0, sizeof *cp);
                        for (size_t k = 0; k < nf; k++)
                            free(f[k]);
                        i = nl;
                        line++;
                        continue;
                    }
                }
                if (nf > 3 && f[3][0])
                    snprintf(cp->build_cmd, sizeof cp->build_cmd, "%s", f[3]);
                if (nf > 4 && f[4][0])
                    snprintf(cp->test_cmd, sizeof cp->test_cmd, "%s", f[4]);
                snprintf(cp->version_flag, sizeof cp->version_flag, "%s",
                         nf > 5 && f[5][0] ? f[5] : "--version");
                char base_l[512], dir_l[256];
                snprintf(base_l, sizeof base_l, "%.250s", st->base);
                snprintf(dir_l, sizeof dir_l, "%s", cp->dir);
                if (base_l[0])
                    snprintf(cp->doc_path, sizeof cp->doc_path,
                             "%.240s/%.200s/README.md", base_l, dir_l);
                else
                    snprintf(cp->doc_path, sizeof cp->doc_path,
                             "%.200s/README.md", dir_l);
                st->n++;
            }
            for (size_t k = 0; k < nf; k++)
                free(f[k]);
        }
        i = nl;
        line++;
    }
    free(body);
    return 0;
}

/* ---------------- markdown scanning ---------------- */

static size_t count_headings(const char *buf, size_t len, heading_t *hs,
                             size_t maxh)
{
    size_t nh = 0, i = 0;
    while (i < len && nh < maxh) {
        size_t j = i;
        int lead = 0;
        while (j < len && lead < 3 && (buf[j] == ' ' || buf[j] == '\t')) {
            j++;
            lead++;
        }
        int n = 0;
        while (j < len && buf[j] == '#' && n < 7) {
            j++;
            n++;
        }
        if (n >= 1 && n <= 6 &&
            (j >= len || is_ws_char(buf[j]))) {
            heading_t *hd = &hs[nh++];
            hd->off = i;
            size_t e = j;
            while (e < len && buf[e] != '\n')
                e++;
            hd->end = e;
            hd->level = n;
            size_t k = j, o = 0;
            while (k < e && (buf[k] == ' ' || buf[k] == '\t'))
                k++;
            while (k < e && o < sizeof hd->norm - 1) {
                char c = buf[k];
                if (c == '`')
                    k++;
                else {
                    hd->norm[o] = (char)tolower((unsigned char)c);
                    hd->raw[o] = c;
                    o++;
                    k++;
                }
            }
            while (o > 0 && (hd->norm[o - 1] == ' ' || hd->norm[o - 1] == '\t'))
                o--;
            hd->norm[o] = 0;
            hd->raw[o] = 0;
        }
        while (i < len && buf[i] != '\n')
            i++;
        if (i < len)
            i++;
    }
    return nh;
}

/* does the normalized heading match the synonym list (word starts-with or
 * phrase)? */
static int heading_matches(const char *norm, const char *const *words,
                           const char *phrase)
{
    if (phrase && strstr(norm, phrase))
        return 1;
    size_t i = 0;
    while (norm[i]) {
        while (norm[i] && !word_char((unsigned char)norm[i]))
            i++;
        if (!norm[i])
            break;
        size_t s = i;
        while (norm[i] && word_char((unsigned char)norm[i]))
            i++;
        for (size_t w = 0; words[w]; w++) {
            size_t wl = strlen(words[w]);
            if (i - s >= wl && strncmp(norm + s, words[w], wl) == 0)
                return 1;
        }
    }
    return 0;
}

static const char *const W_BUILD[] = {"build", "quickstart", NULL};
static const char *const W_RUN[] = {"run", "usage", "quickstart", NULL};
static const char *const W_API[] = {"api", "endpoint", NULL};
static const char *const W_STATE[] = {"internals", "architecture", "design",
                                      "durability", "storage", "state", NULL};
static const char *const W_TESTS[] = {"test", NULL};
static const char *const W_HONESTY[] = {"limitation", "roadmap", NULL};

typedef struct {
    const char *id;
    const char *label;
    const char *const *words;
    const char *phrase;
} section_t;

static const section_t SECTIONS[] = {
    {"build", "## Build", W_BUILD, NULL},
    {"run", "## Run", W_RUN, NULL},
    {"api", "## API", W_API, NULL},
    {"state", "## Internals", W_STATE, "data model"},
    {"tests", "## Tests", W_TESTS, NULL},
    {"honesty", "## Limitations", W_HONESTY, NULL},
};

static int range_nonempty(const char *buf, size_t start, size_t end)
{
    for (size_t i = start; i < end; i++)
        if (!is_ws_char(buf[i]))
            return 1;
    return 0;
}

/* find section range [start,end) for heading index h: to the next heading
 * of the same or higher level */
static void section_range(const heading_t *hs, size_t nh, size_t h,
                          size_t len, size_t *start, size_t *end)
{
    *start = hs[h].end + 1;
    if (*start > len)
        *start = len;
    *end = len;
    for (size_t k = h + 1; k < nh; k++) {
        if (hs[k].level <= hs[h].level) {
            *end = hs[k].off;
            return;
        }
    }
}

/* ---------------- endpoint extraction ---------------- */

static int ep_exists(const endpoint_t *eps, size_t n, const char *method,
                     const char *path)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(eps[i].method, method) == 0 && strcmp(eps[i].path, path) == 0)
            return 1;
    return 0;
}

static int method_len(const char *buf, size_t len, size_t i, const char *m)
{
    size_t ml = strlen(m);
    if (i + ml > len)
        return 0;
    if (memcmp(buf + i, m, ml) != 0)
        return 0;
    if (i > 0 && word_char((unsigned char)buf[i - 1]))
        return 0;
    if (i + ml < len && word_char((unsigned char)buf[i + ml]))
        return 0;
    return 1;
}

static void collect_endpoints(const char *buf, size_t start, size_t end,
                              endpoint_t *eps, size_t *n)
{
    for (size_t i = start; i < end && *n < NEP_MAX; i++) {
        for (size_t m = 0; m < NMETHODS; m++) {
            if (!method_len(buf, end, i, METHODS[m]))
                continue;
            size_t j = i + strlen(METHODS[m]);
            while (j < end && (buf[j] == ' ' || buf[j] == '\t'))
                j++;
            if (j < end && buf[j] == '|') {
                j++;
                while (j < end && (buf[j] == ' ' || buf[j] == '\t'))
                    j++;
            }
            if (j < end && buf[j] == '`')
                j++; /* markdown-inline-coded paths */
            if (j >= end || buf[j] != '/')
                continue;
            size_t ps = j;
            while (j < end && !is_ws_char(buf[j]) && buf[j] != '|')
                j++;
            size_t pl = j - ps;
            if (pl == 0 || pl >= 128)
                continue;
            char path[128];
            memcpy(path, buf + ps, pl);
            path[pl] = 0;
            lc(path);
            char *q = strchr(path, '?');
            if (q)
                *q = 0;
            while (pl > 1) {
                char c = path[pl - 1];
                if (c == ':' || c == '.' || c == ',' || c == ';' ||
                    c == ')' || c == ']' || c == '}' || c == '`')
                    path[--pl] = 0;
                else
                    break;
            }
            if (!ep_exists(eps, *n, METHODS[m], path)) {
                snprintf(eps[*n].method, sizeof eps[*n].method, "%s",
                         METHODS[m]);
                snprintf(eps[*n].path, sizeof eps[*n].path, "%s", path);
                (*n)++;
            }
            i = j;
            break;
        }
    }
}

/* ---------------- version helpers ---------------- */

static int ver_eq(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V')
        a++;
    if (*b == 'v' || *b == 'V')
        b++;
    return strcmp(a, b) == 0;
}

/* run a binary with one flag argument, capturing stdout, with a 5s cap */
int run_cmd_out(const char *bin, const char *flag, char *out, size_t outsz)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(bin, bin, flag, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t got = 0;
    int64_t deadline = now_ms() + 5000;
    out[0] = 0;
    for (;;) {
        int64_t remain = deadline - now_ms();
        if (remain <= 0)
            break;
        struct pollfd p = {.fd = fds[0], .events = POLLIN};
        int rc = poll(&p, 1, (int)remain);
        if (rc < 0)
            break;
        if (rc == 0)
            break;
        if (p.revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(fds[0], out + got, outsz - got - 1);
            if (r <= 0)
                break;
            got += (size_t)r;
            if (got >= outsz - 1)
                break;
        } else if (p.revents & (POLLERR | POLLNVAL)) {
            break;
        }
    }
    close(fds[0]);
    out[got] = 0;
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
    return (got > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* ---------------- per-component audit ---------------- */

static void do_doc_checks(comp_t *c, const char *body, size_t len)
{
    heading_t hs[256];
    size_t nh = count_headings(body, len, hs, 256);

    /* c1 purpose: top-level heading naming the component + intro */
    size_t h1 = nh;
    for (size_t k = 0; k < nh; k++)
        if (hs[k].level == 1) {
            h1 = k;
            break;
        }
    if (h1 == nh) {
        add_check(c, "purpose", CK_FAIL, "missing top-level '# <name>' heading");
    } else {
        char name_l[NAME_MAX];
        snprintf(name_l, sizeof name_l, "%s", c->name);
        lc(name_l);
        if (!strstr(hs[h1].norm, name_l)) {
            add_check(c, "purpose", CK_FAIL,
                      "heading '# %s' does not identify component '%s'",
                      hs[h1].raw, c->name);
        } else {
            size_t s = hs[h1].end + 1;
            if (s > len)
                s = len;
            const char *p = NULL;
            while (s < len) {
                size_t e = s;
                while (e < len && body[e] != '\n')
                    e++;
                char c0 = body[s];
                if (c0 != '\n' && c0 != '\r' && !is_ws_char(c0) &&
                    !(c0 == '`' && e > s + 2 &&
                      (memcmp(body + s, "```", 3) == 0 ||
                       memcmp(body + s, "~~~", 3) == 0))) {
                    p = body + s;
                    break;
                }
                s = e < len ? e + 1 : len;
            }
            if (!p || (nh > 1 && (size_t)(p - body) >= hs[1].off)) {
                add_check(c, "purpose", CK_FAIL,
                          "heading '# %s' has no intro paragraph",
                          hs[h1].raw);
            } else {
                add_check(c, "purpose", CK_PASS,
                          "heading '# %s' + intro paragraph", hs[h1].raw);
            }
        }
    }

    /* c2-c7 sections */
    for (size_t s = 0; s < sizeof SECTIONS / sizeof SECTIONS[0]; s++) {
        const section_t *sec = &SECTIONS[s];
        size_t found = nh, frange_s = 0, frange_e = 0;
        for (size_t k = 0; k < nh; k++) {
            if (hs[k].level == 2 || hs[k].level == 3) {
                if (heading_matches(hs[k].norm, sec->words, sec->phrase)) {
                    found = k;
                    section_range(hs, nh, k, len, &frange_s, &frange_e);
                    break;
                }
            }
        }
        if (found == nh) {
            add_check(c, sec->id, CK_FAIL, "missing '%s' heading (or synonym)",
                      sec->label);
            continue;
        }
        if (!range_nonempty(body, frange_s, frange_e)) {
            add_check(c, sec->id, CK_FAIL, "section '## %s' is empty",
                      hs[found].raw);
            continue;
        }
        if (s == 0 && c->build_cmd[0]) {
            if (!range_str(body, frange_s, frange_e, c->build_cmd))
                add_check(c, sec->id, CK_FAIL,
                          "section '## %s' lacks build command '%s'",
                          hs[found].raw, c->build_cmd);
            else
                add_check(c, sec->id, CK_PASS,
                          "section '## %s' + command '%s'", hs[found].raw,
                          c->build_cmd);
            continue;
        }
        if (s == 4 && c->test_cmd[0]) {
            if (!range_str(body, frange_s, frange_e, c->test_cmd))
                add_check(c, sec->id, CK_FAIL,
                          "section '## %s' lacks test command '%s'",
                          hs[found].raw, c->test_cmd);
            else
                add_check(c, sec->id, CK_PASS,
                          "section '## %s' + command '%s'", hs[found].raw,
                          c->test_cmd);
            continue;
        }
        add_check(c, sec->id, CK_PASS, "section '## %s'", hs[found].raw);
    }

    /* api endpoints from the doc's API section */
    size_t api_h = nh;
    for (size_t k = 0; k < nh; k++)
        if (hs[k].level == 2 || hs[k].level == 3) {
            if (heading_matches(hs[k].norm, W_API, NULL)) {
                api_h = k;
                break;
            }
        }
    if (api_h != nh) {
        size_t as, ae;
        section_range(hs, nh, api_h, len, &as, &ae);
        collect_endpoints(body, as, ae, c->doc_eps, &c->ndoc_eps);
    }

    /* c8 version: presence always; match when --live */
    if (scan_version(body, len, c->doc_version, sizeof c->doc_version) == 0) {
        add_check(c, "version", CK_FAIL, "no version token (v?X.Y.Z) in doc");
    } else {
        add_check(c, "version", CK_PASS, "version token %s present",
                  c->doc_version);
    }
}

static void do_live_checks(comp_t *c, const char *spec, size_t splen,
                           int have_spec, const char *host)
{
    check_t *vck = NULL;
    for (size_t i = 0; i < c->nchecks; i++)
        if (strcmp(c->checks[i].id, "version") == 0)
            vck = &c->checks[i];

    if (c->port <= 0) {
        if (vck && vck->st == CK_PASS)
            detail_append(vck, " (no live daemon; presence only)");
        add_check(c, "api-conformance", CK_SKIP, "no live daemon in manifest");
        return;
    }

    /* c9 api-conformance */
    if (!have_spec) {
        add_check(c, "api-conformance", CK_SKIP,
                  "daemon unreachable (%s:%d)", host, c->port);
    } else {
        collect_endpoints(spec, 0, splen, c->live_eps, &c->nlive_eps);
        for (size_t i = 0; i < c->nlive_eps; i++) {
            if (!ep_exists(c->doc_eps, c->ndoc_eps, c->live_eps[i].method,
                           c->live_eps[i].path) &&
                c->nlive_only < NEP_MAX)
                c->live_only[c->nlive_only++] = c->live_eps[i];
        }
        for (size_t i = 0; i < c->ndoc_eps; i++) {
            if (!ep_exists(c->live_eps, c->nlive_eps, c->doc_eps[i].method,
                           c->doc_eps[i].path) &&
                c->ndoc_only < NEP_MAX)
                c->doc_only[c->ndoc_only++] = c->doc_eps[i];
        }
        if (c->nlive_only == 0 && c->ndoc_only == 0) {
            add_check(c, "api-conformance", CK_PASS,
                      "live spec and doc agree on %zu endpoints",
                      c->nlive_eps);
        } else {
            buf_t d = {0};
            for (size_t i = 0; i < c->nlive_only && i < 8; i++)
                buf_printf(&d, "%slive-only %s %s", i ? "; " : "",
                           c->live_only[i].method, c->live_only[i].path);
            if (c->nlive_only > 8)
                buf_printf(&d, "; +%zu more", c->nlive_only - 8);
            for (size_t i = 0; i < c->ndoc_only && i < 8; i++)
                buf_printf(&d, "%sdoc-only %s %s",
                           d.len ? "; " : "", c->doc_only[i].method,
                           c->doc_only[i].path);
            if (c->ndoc_only > 8)
                buf_printf(&d, "; +%zu more", c->ndoc_only - 8);
            add_check(c, "api-conformance", CK_FAIL, "endpoint mismatch: %s",
                      d.len ? d.p : "none");
            buf_free(&d);
        }
    }

    /* c8 version match against authoritative source */
    if (!vck || vck->st != CK_PASS)
        return;
    char auth[32] = {0};
    const char *src = NULL;
    /* binary path: <base>/<dir>/build/<name> */
    char *bp = xstrdup(c->doc_path);
    char *slash = strrchr(bp, '/');
    char binpath[512];
    if (slash) {
        *slash = 0;
        snprintf(binpath, sizeof binpath, "%s/build/%s", bp, c->name);
    } else {
        snprintf(binpath, sizeof binpath, "%s/build/%s", c->dir, c->name);
    }
    free(bp);
    char binout[OUT_MAX];
    if (access(binpath, X_OK) == 0) {
        if (run_cmd_out(binpath, c->version_flag, binout, sizeof binout) == 0 &&
            scan_version(binout, strlen(binout), auth, sizeof auth) > 0) {
            src = "binary";
        }
    }
    if (!src && have_spec) {
        if (scan_version(spec, splen, auth, sizeof auth) > 0)
            src = "live spec";
    }
    if (!src) {
        detail_append(vck, "; daemon unreachable, no local binary (%.120s)",
                      binpath);
        vck->st = CK_SKIP;
        return;
    }
    if (ver_eq(c->doc_version, auth)) {
        snprintf(vck->detail, sizeof vck->detail, "doc %s matches %s %s",
                 c->doc_version, src, auth);
    } else {
        vck->st = CK_FAIL;
        snprintf(vck->detail, sizeof vck->detail,
                 "doc %s != %s %s", c->doc_version, src, auth);
    }
}

static int fetch_spec(comp_t *c, const char *host, char **spec, size_t *slen)
{
    char *body = NULL;
    size_t blen = 0;
    int status = 0;
    char err[256];
    if (http_get(host, c->port, "/", &body, &blen, &status, err,
                 sizeof err) != 0)
        return -1;
    if (status != 200) {
        free(body);
        return -1;
    }
    *spec = body;
    *slen = blen;
    return 0;
}

int audit_components(stack *st, int live)
{
    for (size_t i = 0; i < st->n; i++) {
        comp_t *c = &st->comps[i];
        size_t len = 0;
        char *body = read_sanitized(c->doc_path, &len, DOC_MAX);
        if (!body) {
            for (size_t k = 0; k < sizeof SECTIONS / sizeof SECTIONS[0]; k++)
                add_check(c, SECTIONS[k].id, CK_FAIL,
                          "no document at %s", c->doc_path);
            add_check(c, "version", CK_FAIL, "no document at %s", c->doc_path);
            add_check(c, "api-conformance", CK_SKIP, "no document at %s",
                      c->doc_path);
            comp_count(c);
            continue;
        }
        do_doc_checks(c, body, len);
        if (live) {
            char *spec = NULL;
            size_t splen = 0;
            int have = fetch_spec(c, "127.0.0.1", &spec, &splen) == 0;
            do_live_checks(c, spec, splen, have, "127.0.0.1");
            free(spec);
        } else {
            check_t *vck = NULL;
            for (size_t k = 0; k < c->nchecks; k++)
                if (strcmp(c->checks[k].id, "version") == 0)
                    vck = &c->checks[k];
            if (vck && vck->st == CK_PASS)
                detail_append(vck, " (use --live to verify)");
            add_check(c, "api-conformance", CK_SKIP, "requires --live");
        }
        free(body);
        comp_count(c);
    }
    return 0;
}

int stack_totals(stack *st, int *pass, int *fail, int *skip)
{
    *pass = *fail = *skip = 0;
    for (size_t i = 0; i < st->n; i++) {
        *pass += st->comps[i].pass;
        *fail += st->comps[i].fail;
        *skip += st->comps[i].skip;
    }
    return *pass + *fail == 0 ? 0
                              : (int)((*pass * 100 + (*pass + *fail) / 2) /
                                      (*pass + *fail));
}

/* ---------------- reporting ---------------- */

static const char *st_str(ck_status_t st)
{
    switch (st) {
    case CK_PASS: return "PASS";
    case CK_FAIL: return "FAIL";
    case CK_SKIP: return "SKIP";
    }
    return "?";
}

static const char *st_lstr(ck_status_t st)
{
    switch (st) {
    case CK_PASS: return "pass";
    case CK_FAIL: return "fail";
    case CK_SKIP: return "skip";
    }
    return "?";
}

void report_human(stack *st, int live, FILE *f)
{
    fprintf(f, "exodoc v%s audit\nmanifest: %s\nbase: %s\nlive: %s\ntime: "
               "%ld\n",
            EXODOC_VERSION, st->manifest_path, st->base, live ? "on" : "off",
            (long)time(NULL));
    for (size_t i = 0; i < st->n; i++) {
        comp_t *c = &st->comps[i];
        fprintf(f, "\n[%s]\n", c->name);
        for (size_t k = 0; k < c->nchecks; k++)
            fprintf(f, "%s %s: %s: %s\n", st_str(c->checks[k].st), c->name,
                    c->checks[k].id, c->checks[k].detail);
        fprintf(f, "-- %s: %d pass, %d fail, %d skip (score %d%%)\n", c->name,
                c->pass, c->fail, c->skip, c->score);
    }
    int p, fa, s;
    int sc = stack_totals(st, &p, &fa, &s);
    fprintf(f, "\n=== audit: %d pass, %d fail (score %d%%) ===\n", p, fa, sc);
}

void report_json(stack *st, int live, FILE *f)
{
    buf_t b = {0};
    buf_printf(&b, "{\n  \"tool\": \"exodoc\",\n  \"version\": \"%s\",\n",
               EXODOC_VERSION);
    buf_printf(&b, "  \"timestamp\": %ld,\n  \"live\": %s,\n",
               (long)time(NULL), live ? "true" : "false");
    char *m = json_escape(st->manifest_path, strlen(st->manifest_path));
    char *ba = json_escape(st->base, strlen(st->base));
    buf_printf(&b, "  \"manifest\": \"%s\",\n  \"base\": \"%s\",\n", m, ba);
    free(m);
    free(ba);
    int p, fa, s;
    int sc = stack_totals(st, &p, &fa, &s);
    buf_printf(&b,
               "  \"summary\": {\"pass\": %d, \"fail\": %d, \"skip\": %d, "
               "\"score\": %d},\n  \"components\": [\n",
               p, fa, s, sc);
    for (size_t i = 0; i < st->n; i++) {
        comp_t *c = &st->comps[i];
        char *nm = json_escape(c->name, strlen(c->name));
        char *dr = json_escape(c->dir, strlen(c->dir));
        char *dp = json_escape(c->doc_path, strlen(c->doc_path));
        buf_printf(&b,
                   "    {\"name\": \"%s\", \"dir\": \"%s\", \"port\": %d, "
                   "\"doc\": \"%s\", \"pass\": %d, \"fail\": %d, \"skip\": %d, "
                   "\"score\": %d,\n     \"checks\": [",
                   nm, dr, c->port, dp, c->pass, c->fail, c->skip, c->score);
        free(nm);
        free(dr);
        free(dp);
        for (size_t k = 0; k < c->nchecks; k++) {
            check_t *ck = &c->checks[k];
            char *det = json_escape(ck->detail, strlen(ck->detail));
            buf_printf(&b, "%s{\"id\": \"%s\", \"status\": \"%s\", "
                           "\"detail\": \"%s\"}",
                       k ? ", " : "", ck->id, st_lstr(ck->st), det);
            free(det);
        }
        buf_puts(&b, "],\n     \"endpoint_mismatch\": {\"live_only\": [");
        for (size_t k = 0; k < c->nlive_only; k++)
            buf_printf(&b, "%s\"%s %s\"", k ? ", " : "", c->live_only[k].method,
                       c->live_only[k].path);
        buf_puts(&b, "], \"doc_only\": [");
        for (size_t k = 0; k < c->ndoc_only; k++)
            buf_printf(&b, "%s\"%s %s\"", k ? ", " : "", c->doc_only[k].method,
                       c->doc_only[k].path);
        buf_puts(&b, "]}}");
        buf_puts(&b, i + 1 < st->n ? ",\n" : "\n");
    }
    buf_puts(&b, "  ]\n}\n");
    fputs(b.p ? b.p : "", f);
    buf_free(&b);
}
