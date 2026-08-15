/* exoqms checks: the ISO 19011 audit-program checklist. Each check returns
 * pass/fail/skip + an evidence line. Child processes (component tests,
 * exodoc, exoqms-ui) run under a hard timeout and are SIGKILLed by process
 * group on expiry. */
#include "exoqms.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OUT_CAP (1024u * 1024u)
#define EVID_MAX 4096

void cfg_defaults(cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->exodoc_path, sizeof cfg->exodoc_path, "exodoc");
    snprintf(cfg->repo, sizeof cfg->repo, ".");
    snprintf(cfg->agents, sizeof cfg->agents, "a,b,b1,b2,b3");
    cfg->notes24h = 5;
    snprintf(cfg->exosched_url, sizeof cfg->exosched_url,
             "http://127.0.0.1:7655");
}

static void set_finding(finding_t *f, const char *id, res_t r,
                        const char *fmt, ...)
{
    snprintf(f->id, sizeof f->id, "%s", id);
    f->res = r;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->evidence, EVID_MAX, fmt, ap);
    va_end(ap);
}

/* run a child process with a hard timeout; output collected into *out.
 * returns: 0 = exited 0, 1 = exited nonzero, -1 = error (exec failed /
 * internal), -2 = timed out and killed */
int run_child(char *const argv[], const char *cwd, long timeout_s,
              char **out, size_t *outlen, char *err, size_t errsz)
{
    int pfd[2];
    if (pipe(pfd) != 0) {
        snprintf(err, errsz, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork: %s", strerror(errno));
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        setsid();
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        if (cwd && chdir(cwd) != 0)
            _exit(126);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    buf_t b = {0};
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = 0;
    for (;;) {
        char tmp[8192];
        ssize_t got = read(pfd[0], tmp, sizeof tmp);
        if (got > 0) {
            if (b.len + (size_t)got <= OUT_CAP)
                buf_put(&b, tmp, (size_t)got);
        } else if (got < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            while (read(pfd[0], tmp, sizeof tmp) > 0)
                ;
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) > timeout_s) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            while (read(pfd[0], tmp, sizeof tmp) > 0)
                ;
            close(pfd[0]);
            if (out) {
                *out = b.p ? b.p : xstrdup("");
                *outlen = b.len;
                b.p = NULL;
            }
            buf_free(&b);
            snprintf(err, errsz, "child timed out after %lds and was killed",
                     timeout_s);
            return -2;
        }
        struct pollfd pf = {.fd = pfd[0], .events = POLLIN};
        if (poll(&pf, 1, 50) < 0 && errno != EINTR)
            break;
    }
    close(pfd[0]);
    if (out) {
        *out = b.p ? b.p : xstrdup("");
        *outlen = b.len;
        b.p = NULL;
    }
    buf_free(&b);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) {
            snprintf(err, errsz, "exec failed: %s", argv[0]);
            return -1;
        }
        return code == 0 ? 0 : 1;
    }
    snprintf(err, errsz, "child killed by signal %d", WTERMSIG(status));
    return -1;
}

/* ---------- check 1: component tests (build-warnings proxy) ---------- */

typedef struct {
    char name[128];
    char dir[256];
    char test_cmd[512];
} comp_t;

static int manifest_parse(const char *path, comp_t **comps, size_t *n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    comp_t *out = NULL;
    size_t cnt = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        trim_crlf(line);
        if (!line[0] || line[0] == '#')
            continue;
        char *col[8];
        int nc = tab_split(line, col, 8);
        if (nc < 5 || !col[4][0])
            continue; /* no test command -> not eligible */
        out = xrealloc(out, (cnt + 1) * sizeof(comp_t));
        comp_t *c = &out[cnt++];
        snprintf(c->name, sizeof c->name, "%s", col[0]);
        snprintf(c->dir, sizeof c->dir, "%s", col[1]);
        snprintf(c->test_cmd, sizeof c->test_cmd, "%s", col[4]);
    }
    fclose(f);
    *comps = out;
    *n = cnt;
    return 0;
}

static void check_component_tests(check_ctx_t *ctx, finding_t *f)
{
    char manifest[2048];
    snprintf(manifest, sizeof manifest, "%s/docs/stack.tsv", ctx->cfg->repo);
    comp_t *comps = NULL;
    size_t n = 0;
    if (manifest_parse(manifest, &comps, &n) != 0) {
        set_finding(f, "component-tests", R_SKIP,
                    "stack manifest not found: %s", manifest);
        return;
    }
    if (n == 0) {
        set_finding(f, "component-tests", R_SKIP,
                    "no component in %s declares a test command", manifest);
        free(comps);
        return;
    }
    size_t ran = 0, passed = 0;
    buf_t ev = {0};
    for (size_t i = 0; i < n; i++) {
        char *cdir = ctx->cfg->repo;
        char sub[2048];
        if (comps[i].dir[0]) {
            struct stat st;
            snprintf(sub, sizeof sub, "%s/%s", ctx->cfg->repo, comps[i].dir);
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
                cdir = sub;
        }
        char *argv[4] = {"sh", "-c", comps[i].test_cmd, NULL};
        char *out = NULL;
        size_t olen = 0;
        char err[256];
        int rc = run_child(argv, cdir, CHECK_TIMEOUT_S, &out, &olen, err,
                           sizeof err);
        char *summary = NULL;
        if (out) {
            char *save = NULL;
            for (char *l = strtok_r(out, "\n", &save); l;
                 l = strtok_r(NULL, "\n", &save))
                summary = l;
            if (summary && strlen(summary) > 200)
                summary[200] = 0;
        }
        ran++;
        if (rc == 0) {
            passed++;
            buf_printf(&ev, "%s: pass (%s)\n", comps[i].name,
                       summary && summary[0] ? summary : "exit 0");
        } else if (rc == -2) {
            buf_printf(&ev, "%s: FAIL timed out (killed)\n", comps[i].name);
        } else if (rc == 1) {
            buf_printf(&ev, "%s: FAIL exit %s\n", comps[i].name,
                       summary && summary[0] ? summary : "nonzero");
        } else {
            buf_printf(&ev, "%s: FAIL %s\n", comps[i].name, err);
        }
        free(out);
    }
    free(comps);
    if (ran == passed)
        set_finding(f, "component-tests", R_PASS, "%zu/%zu component test "
                    "commands passed", passed, ran);
    else
        set_finding(f, "component-tests", R_FAIL, "%zu/%zu component test "
                    "commands passed", passed, ran);
    if (ev.len + strlen(f->evidence) + 2 < EVID_MAX) {
        strncat(f->evidence, "\n", EVID_MAX - strlen(f->evidence) - 1);
        strncat(f->evidence, ev.p ? ev.p : "",
                EVID_MAX - strlen(f->evidence) - 1);
    }
    buf_free(&ev);
}

/* ---------- check 2: doc-compliance (exodoc) ---------- */

static void check_doc_compliance(check_ctx_t *ctx, finding_t *f)
{
    char manifest[2048];
    snprintf(manifest, sizeof manifest, "%s/docs/stack.tsv", ctx->cfg->repo);
    char exo_url[512];
    snprintf(exo_url, sizeof exo_url, "http://%s:%d", ctx->exo->host,
             ctx->exo->port);
    char *argv[16] = {"exodoc", "audit", "--live", "--stack", manifest,
                      "--base", ctx->cfg->repo, "--exomind", exo_url, NULL};
    argv[0] = ctx->cfg->exodoc_path;
    char *out = NULL;
    size_t olen = 0;
    char err[256];
    int rc = run_child(argv, ctx->cfg->repo, CHECK_TIMEOUT_S, &out, &olen,
                       err, sizeof err);
    int pass = -1, fail = -1, score = -1;
    if (out) {
        char *p = strstr(out, "=== audit:");
        if (p)
            sscanf(p, "=== audit: %d pass, %d fail (score %d%%)", &pass, &fail,
                   &score);
    }
    if (pass < 0) {
        set_finding(f, "doc-compliance", R_FAIL,
                    "no audit summary line (exit %s)", rc == -2 ? "timed out" :
                    rc == -1 ? err : rc == 0 ? "0" : "nonzero");
    } else if (fail == 0) {
        set_finding(f, "doc-compliance", R_PASS,
                    "%d pass, %d fail (score %d%%)", pass, fail, score);
    } else {
        set_finding(f, "doc-compliance", R_FAIL,
                    "%d pass, %d fail (score %d%%)", pass, fail, score);
    }
    free(out);
}

/* ---------- check 3: dogfood (swarm conventions) ---------- */

static void check_dogfood(check_ctx_t *ctx, finding_t *f)
{
    const char *agents = ctx->agents && ctx->agents[0] ? ctx->agents
                                                        : ctx->cfg->agents;
    char err[256];
    int missing = 0, total = 0;
    buf_t ev = {0};
    char *copy = xstrdup(agents);
    char *save = NULL;
    for (char *a = strtok_r(copy, ",", &save); a; a = strtok_r(NULL, ",", &save)) {
        while (*a == ' ')
            a++;
        if (!a[0])
            continue;
        total++;
        char key[512];
        snprintf(key, sizeof key, "agent:%s:status", a);
        char *v = NULL;
        if (exo_get(ctx->exo, key, &v, err, sizeof err) != 0) {
            free(v);
            set_finding(f, "dogfood", R_FAIL, "exomind unreachable: %s", err);
            free(copy);
            buf_free(&ev);
            return;
        }
        if (!v) {
            missing++;
            buf_printf(&ev, "missing agent:%s:status\n", a);
        } else {
            buf_printf(&ev, "agent:%s:status ok\n", a);
        }
        free(v);
    }
    free(copy);

    /* notes in the last 24h */
    int64_t cutoff = now_epoch() * 1000 - 86400000;
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    if (exo_request(ctx->exo, "GET", "/notes?limit=200", NULL, 0, 0, &resp,
                    &rlen, &status, err, sizeof err) != 0) {
        set_finding(f, "dogfood", R_FAIL, "exomind unreachable: %s", err);
        free(resp);
        buf_free(&ev);
        return;
    }
    char *lines = xstrdup(resp ? resp : "");
    free(resp);
    int fresh = 0;
    char *lsave = NULL;
    for (char *l = strtok_r(lines, "\n", &lsave); l;
         l = strtok_r(NULL, "\n", &lsave)) {
        if (strncmp(l, "note:", 5) != 0)
            continue;
        char *colon = strchr(l + 5, ':');
        if (!colon)
            continue;
        *colon = 0;
        long long ts = strtoll(l + 5, NULL, 10);
        if (ts >= cutoff)
            fresh++;
    }
    free(lines);
    int need = ctx->cfg->notes24h > 0 ? ctx->cfg->notes24h : 5;
    buf_printf(&ev, "%d notes in last 24h (need %d)\n", fresh, need);
    /* scheduler health as evidence only */
    exo_t sched;
    if (exo_init(&sched, ctx->cfg->exosched_url, err, sizeof err) == 0) {
        char *sresp = NULL;
        size_t slen = 0;
        int sst = 0;
        if (exo_request(&sched, "GET", "/ping", NULL, 0, 0, &sresp, &slen,
                        &sst, err, sizeof err) == 0 && sst == 200)
            buf_puts(&ev, "exosched pong\n");
        else
            buf_puts(&ev, "exosched unreachable\n");
        free(sresp);
    }
    if (missing > 0 || fresh < need)
        set_finding(f, "dogfood", R_FAIL, "%d/%d agents active, %d notes in "
                    "last 24h (need %d)", total - missing, total, fresh, need);
    else
        set_finding(f, "dogfood", R_PASS, "%d/%d agents active, %d notes in "
                    "last 24h (need %d)", total - missing, total, fresh, need);
    if (ev.len + strlen(f->evidence) + 2 < EVID_MAX) {
        strncat(f->evidence, "\n", EVID_MAX - strlen(f->evidence) - 1);
        strncat(f->evidence, ev.p ? ev.p : "",
                EVID_MAX - strlen(f->evidence) - 1);
    }
    buf_free(&ev);
}

/* ---------- check 4: ui-audit (exoqms-ui, built by B2) ---------- */

static void check_ui_audit(check_ctx_t *ctx, finding_t *f)
{
    if (!ctx->cfg->ui_path[0]) {
        set_finding(f, "ui-audit", R_SKIP, "no ui binary configured (--ui)");
        return;
    }
    if (!ctx->target || !ctx->target[0]) {
        set_finding(f, "ui-audit", R_SKIP, "no target given (audit ?target=)");
        return;
    }
    char *argv[4] = {NULL, (char *)ctx->target, "--json", NULL};
    argv[0] = ctx->cfg->ui_path;
    char *out = NULL;
    size_t olen = 0;
    char err[256];
    int rc = run_child(argv, NULL, CHECK_TIMEOUT_S, &out, &olen, err,
                       sizeof err);
    if (rc == 0) {
        set_finding(f, "ui-audit", R_PASS, "0 findings on %s", ctx->target);
    } else if (rc == 1) {
        int cnt = 0;
        if (out) {
            for (char *p = out; (p = strstr(p, "\"check\"")); p += 7)
                cnt++;
        }
        set_finding(f, "ui-audit", R_FAIL, "%d finding(s) on %s%s", cnt,
                    ctx->target,
                    out && out[0] ? ": " : "");
        if (out && out[0]) {
            char *o = xstrndup(out, olen < 200 ? olen : 200);
            trim_crlf(o);
            if (strlen(f->evidence) + strlen(o) + 2 < EVID_MAX) {
                strncat(f->evidence, o,
                        EVID_MAX - strlen(f->evidence) - 1);
            }
            free(o);
        }
    } else if (rc == -2) {
        set_finding(f, "ui-audit", R_FAIL, "timed out after %ds on %s",
                    CHECK_TIMEOUT_S, ctx->target);
    } else {
        set_finding(f, "ui-audit", R_FAIL, "%s", err);
    }
    free(out);
}
/* ---------- check 5: metrics trend (ISO 9004 sustained success) ---------- */

int trend_values(exo_t *e, int64_t **vals, int *n, char **list, size_t *llen)
{
    char err[256];
    char **keys = NULL;
    size_t nk = 0;
    if (exo_list(e, "metric:iter", &keys, &nk, err, sizeof err) != 0)
        return -1;
    int *nums = xcalloc(nk ? nk : 1, sizeof(int));
    char **ke = xcalloc(nk ? nk : 1, sizeof(char *));
    size_t cnt = 0;
    for (size_t i = 0; i < nk; i++) {
        const char *suf = ":tests_passing";
        size_t kl = strlen(keys[i]);
        size_t sl = strlen(suf);
        if (kl <= sl || strcmp(keys[i] + kl - sl, suf) != 0)
            continue;
        if (strncmp(keys[i], "metric:iter", 11) != 0)
            continue;
        char *dot = keys[i] + 11;
        if (!isdigit((unsigned char)dot[0]))
            continue;
        char *end = NULL;
        long it = strtol(dot, &end, 10);
        if (end == dot || *end != ':')
            continue;
        ke[cnt] = xstrdup(keys[i]);
        nums[cnt] = (int)it;
        cnt++;
    }
    for (size_t i = 0; i < nk; i++)
        free(keys[i]);
    free(keys);
    for (size_t i = 0; i < cnt; i++)
        for (size_t j = i + 1; j < cnt; j++)
            if (nums[j] < nums[i]) {
                int t = nums[i];
                nums[i] = nums[j];
                nums[j] = t;
                char *tk = ke[i];
                ke[i] = ke[j];
                ke[j] = tk;
            }
    int64_t *v = xcalloc(cnt ? cnt : 1, sizeof(int64_t));
    buf_t lst = {0};
    size_t got = 0;
    for (size_t i = 0; i < cnt; i++) {
        char *val = NULL;
        if (exo_get(e, ke[i], &val, err, sizeof err) != 0) {
            free(val);
            continue;
        }
        if (!val) {
            continue;
        }
        trim_crlf(val);
        char *end = NULL;
        long long x = strtoll(val, &end, 10);
        if (end == val) {
            free(val);
            continue;
        }
        v[got] = (int64_t)x;
        buf_printf(&lst, "%s\t%lld\n", ke[i], (long long)v[got]);
        got++;
        free(val);
    }
    for (size_t i = 0; i < cnt; i++)
        free(ke[i]);
    free(ke);
    free(nums);
    *vals = v;
    *n = (int)got;
    if (list) {
        *list = lst.p ? lst.p : xstrdup("");
        *llen = lst.len;
    } else {
        buf_free(&lst);
    }
    return 0;
}

/* trend verdict over the two most recent values; flag=1 = stagnation
 * (not improving) */
/* @nonnull */
const char *trend_verdict(int64_t *vals, int n, int *flag)
{
    if (flag)
        *flag = 0;
    if (n < 2)
        return "flat";
    int64_t a = vals[n - 2], b = vals[n - 1];
    const char *v;
    if (b < a)
        v = "down";
    else if (b > a)
        v = "up";
    else
        v = "flat";
    if (flag && strcmp(v, "up") != 0)
        *flag = 1;
    return v;
}

static void check_metrics(check_ctx_t *ctx, finding_t *f)
{
    int64_t *vals = NULL;
    int n = 0;
    char *list = NULL;
    size_t llen = 0;
    if (trend_values(ctx->exo, &vals, &n, &list, &llen) != 0) {
        set_finding(f, "metrics", R_FAIL, "exomind unreachable");
        return;
    }
    if (n < 2) {
        set_finding(f, "metrics", R_SKIP, "need at least 2 iterations of "
                    "metric:iterN:tests_passing (found %d)", n);
        free(vals);
        free(list);
        return;
    }
    int flag = 0;
    const char *v = trend_verdict(vals, n, &flag);
    char seq[1024];
    size_t off = 0;
    char *save = NULL;
    for (char *l = strtok_r(list, "\n", &save); l && off < sizeof seq - 32;
         l = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(l, '\t');
        if (tab) {
            *tab = 0;
            off += (size_t)snprintf(seq + off, sizeof seq - off, "%s=%s ",
                                    l, tab + 1);
        }
    }
    if (strcmp(v, "down") == 0)
        set_finding(f, "metrics", R_FAIL, "trend %s (stagnation flag set): %s",
                    v, seq);
    else
        set_finding(f, "metrics", R_PASS, "trend %s (stagnation flag %s): %s",
                    v, flag ? "set" : "none", seq);
    free(vals);
    free(list);
}

/* ---------- check 6/7: code-safety (exoqms-code) and asset-logic
 * (exoqms-svg) — the field modules. Both are batch static analyzers with
 * the same contract: findings as a JSON array with a "severity" field
 * ("major"/"minor"), exit 0 no findings, 1 findings, 2 usage/IO error.
 * The pass rule is severity-based: 0 MAJOR findings passes; minor
 * findings are reported as evidence but are non-fatal (documented in
 * exoqms/standard.md 5.3). */

/* count findings + major/minor severities in a JSON findings array */
static void json_severity_count(const char *out, int *n, int *maj, int *min)
{
    *n = *maj = *min = 0;
    if (!out)
        return;
    for (const char *p = out; (p = strstr(p, "\"severity\":")); p += 11) {
        const char *v = p + 11;
        if (strncmp(v, "\"major\"", 7) == 0) {
            (*n)++;
            (*maj)++;
        } else if (strncmp(v, "\"minor\"", 7) == 0) {
            (*n)++;
            (*min)++;
        }
    }
}

/* the manifest source dirs (column 2 of docs/stack.tsv) resolved against
 * the repo root, space-separated; NULL when none are usable. This is the
 * default scan target of the code-safety check: the stack audits its own
 * C source. */
static char *manifest_src_dirs(cfg_t *cfg)
{
    char manifest[2048];
    snprintf(manifest, sizeof manifest, "%s/docs/stack.tsv", cfg->repo);
    FILE *f = fopen(manifest, "r");
    if (!f)
        return NULL;
    char dirs[16][2048];
    size_t nd = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        trim_crlf(line);
        if (!line[0] || line[0] == '#')
            continue;
        char *col[8];
        int nc = tab_split(line, col, 8);
        if (nc < 2 || !col[1][0])
            continue;
        char resolved[2048];
        struct stat st;
        if (col[1][0] == '/')
            snprintf(resolved, sizeof resolved, "%s", col[1]);
        else
            snprintf(resolved, sizeof resolved, "%s/%s", cfg->repo, col[1]);
        if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        /* the repo root covers every subdir: skip it if others exist,
           and dedupe identical entries */
        int dup = 0;
        for (size_t i = 0; i < nd; i++)
            if (strcmp(dirs[i], resolved) == 0)
                dup = 1;
        if (!dup && nd < 16)
            snprintf(dirs[nd++], sizeof dirs[0], "%s", resolved);
    }
    fclose(f);
    if (nd == 0)
        return NULL;
    buf_t b = {0};
    for (size_t i = 0; i < nd; i++) {
        /* the repo root (or its `/.` spelling) covers every subdir:
           skip it when subdirs are listed */
        int is_root = strcmp(dirs[i], cfg->repo) == 0;
        size_t rl = strlen(cfg->repo);
        if (!is_root && strlen(dirs[i]) == rl + 2 &&
            strncmp(dirs[i], cfg->repo, rl) == 0 &&
            strcmp(dirs[i] + rl, "/.") == 0)
            is_root = 1;
        if (nd > 1 && is_root)
            continue;
        buf_printf(&b, "%s ", dirs[i]);
    }
    if (!b.len) {
        buf_printf(&b, "%s ", cfg->repo);
    }
    char *out = b.p;
    b.p = NULL;
    buf_free(&b);
    return out;
}

/* append up to 200 chars of child output to the evidence line */
static void finding_snippet(finding_t *f, const char *out, size_t olen)
{
    if (!out || !out[0])
        return;
    char *o = xstrndup(out, olen < 200 ? olen : 200);
    trim_crlf(o);
    if (strlen(f->evidence) + strlen(o) + 2 < EVID_MAX) {
        strncat(f->evidence, " ",
                EVID_MAX - strlen(f->evidence) - 1);
        strncat(f->evidence, o, EVID_MAX - strlen(f->evidence) - 1);
    }
    free(o);
}

static void check_code_safety(check_ctx_t *ctx, finding_t *f)
{
    if (!ctx->cfg->code_path[0]) {
        set_finding(f, "code-safety", R_SKIP,
                    "no code binary configured (--code)");
        return;
    }
    char *default_dirs = NULL;
    const char *target = ctx->target;
    if (!target || !target[0]) {
        /* the whole repo: fixtures are excluded by the tools, documented
           exceptions live in <repo>/.exoqms-allow */
        default_dirs = xstrdup(ctx->cfg->repo);
        if (!default_dirs) {
            set_finding(f, "code-safety", R_SKIP,
                        "no target given (audit ?target=)");
            return;
        }
        target = default_dirs;
    }
    char *dirs = xstrdup(target);
    char *argv[16];
    int n = 0;
    argv[n++] = ctx->cfg->code_path;
    char *save = NULL;
    for (char *p = strtok_r(dirs, " ", &save); p && n < 14;
         p = strtok_r(NULL, " ", &save))
        argv[n++] = p;
    /* documented exceptions: <repo>/.exoqms-allow (file:line:check lines) */
    char allowpath[2048];
    snprintf(allowpath, sizeof allowpath, "%s/.exoqms-allow",
             ctx->cfg->repo);
    FILE *al = fopen(allowpath, "r");
    if (al) {
        fclose(al);
        argv[n++] = "--allow";
        argv[n++] = allowpath;
    }
    argv[n++] = "--json";
    argv[n] = NULL;
    char *out = NULL;
    size_t olen = 0;
    char err[256];
    int rc = run_child(argv, ctx->cfg->repo, CHECK_TIMEOUT_S, &out, &olen,
                       err, sizeof err);
    int nf = 0, maj = 0, min = 0;
    json_severity_count(out, &nf, &maj, &min);
    if (rc == -2) {
        set_finding(f, "code-safety", R_FAIL,
                    "timed out after %ds scanning %s", CHECK_TIMEOUT_S,
                    target);
    } else if (rc == -1) {
        set_finding(f, "code-safety", R_FAIL, "%s", err);
    } else if (maj > 0) {
        set_finding(f, "code-safety", R_FAIL,
                    "%d major, %d minor finding(s) on %s", maj, min, target);
        finding_snippet(f, out, olen);
    } else if (nf == 0 && rc != 0) {
        set_finding(f, "code-safety", R_FAIL,
                    "no severity-parsable findings but exit %d on %s: %s",
                    rc, target, out && out[0] ? out : "(no output)");
    } else {
        set_finding(f, "code-safety", R_PASS,
                    "%d finding(s), 0 major on %s (minor non-fatal)", nf,
                    target);
        if (nf > 0)
            finding_snippet(f, out, olen);
    }
    free(out);
    free(dirs);
    free(default_dirs);
}

static void check_asset_logic(check_ctx_t *ctx, finding_t *f)
{
    if (!ctx->cfg->svg_path[0]) {
        set_finding(f, "asset-logic", R_SKIP,
                    "no svg binary configured (--svg)");
        return;
    }
    const char *target = ctx->target;
    if (!target || !target[0])
        target = ctx->cfg->repo;
    char *argv[6] = {NULL, (char *)target, "--shape", "auto", "--json",
                     NULL};
    argv[0] = ctx->cfg->svg_path;
    char *out = NULL;
    size_t olen = 0;
    char err[256];
    int rc = run_child(argv, ctx->cfg->repo, CHECK_TIMEOUT_S, &out, &olen,
                       err, sizeof err);
    int nf = 0, maj = 0, min = 0;
    json_severity_count(out, &nf, &maj, &min);
    if (rc == -2) {
        set_finding(f, "asset-logic", R_FAIL,
                    "timed out after %ds on %s", CHECK_TIMEOUT_S, target);
    } else if (rc == -1) {
        set_finding(f, "asset-logic", R_FAIL, "%s", err);
    } else if (maj > 0) {
        set_finding(f, "asset-logic", R_FAIL,
                    "%d major, %d minor finding(s) on %s", maj, min, target);
        finding_snippet(f, out, olen);
    } else if (nf == 0 && rc != 0) {
        set_finding(f, "asset-logic", R_FAIL,
                    "no severity-parsable findings but exit %d on %s: %s",
                    rc, target, out && out[0] ? out : "(no output)");
    } else {
        set_finding(f, "asset-logic", R_PASS,
                    "%d finding(s), 0 major on %s", nf, target);
        if (nf > 0)
            finding_snippet(f, out, olen);
    }
    free(out);
}

/* ---------- dispatch ---------- */

int check_run(const char *id, check_ctx_t *ctx, finding_t *f)
{
    memset(f, 0, sizeof *f);
    if (!strcmp(id, "component-tests") || !strcmp(id, "build-warnings"))
        check_component_tests(ctx, f);
    else if (!strcmp(id, "doc-compliance"))
        check_doc_compliance(ctx, f);
    else if (!strcmp(id, "dogfood"))
        check_dogfood(ctx, f);
    else if (!strcmp(id, "ui-audit"))
        check_ui_audit(ctx, f);
    else if (!strcmp(id, "metrics"))
        check_metrics(ctx, f);
    else if (!strcmp(id, "code-safety"))
        check_code_safety(ctx, f);
    else if (!strcmp(id, "asset-logic"))
        check_asset_logic(ctx, f);
    else
        set_finding(f, id, R_SKIP, "unknown check id");
    if (!f->id[0])
        snprintf(f->id, sizeof f->id, "%s", id);
    return 0;
}
