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
    int have_manifest = manifest_parse(manifest, &comps, &n) == 0;
    if (!have_manifest) {
        /* universal mode: no stack manifest — whole-project test commands
         * come from .exoqms.json ("test") and run from the repo root */
        if (ctx->cfg->pcfg.n_test == 0) {
            set_finding(f, "component-tests", R_SKIP,
                        "stack manifest not found: %s (no .exoqms.json "
                        "test commands either)", manifest);
            return;
        }
        size_t ran = 0, passed = 0;
        buf_t ev = {0};
        for (size_t i = 0; i < ctx->cfg->pcfg.n_test; i++) {
            char *argv[4] = {"sh", "-c", ctx->cfg->pcfg.test_cmds[i], NULL};
            char *out = NULL;
            size_t olen = 0;
            char err[256];
            int rc = run_child(argv, ctx->cfg->repo, CHECK_TIMEOUT_S, &out,
                               &olen, err, sizeof err);
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
                buf_printf(&ev, "test[%zu]: pass (%s)\n", i,
                           summary && summary[0] ? summary : "exit 0");
            } else if (rc == -2) {
                buf_printf(&ev, "test[%zu]: FAIL timed out (killed)\n", i);
            } else if (rc == 1) {
                buf_printf(&ev, "test[%zu]: FAIL exit %s\n", i,
                           summary && summary[0] ? summary : "nonzero");
            } else {
                buf_printf(&ev, "test[%zu]: FAIL %s\n", i, err);
            }
            free(out);
        }
        if (ran == passed)
            set_finding(f, "component-tests", R_PASS, "%zu/%zu project test "
                        "commands passed", passed, ran);
        else
            set_finding(f, "component-tests", R_FAIL, "%zu/%zu project test "
                        "commands passed", passed, ran);
        if (ev.len + strlen(f->evidence) + 2 < EVID_MAX) {
            strncat(f->evidence, "\n", EVID_MAX - strlen(f->evidence) - 1);
            strncat(f->evidence, ev.p ? ev.p : "",
                    EVID_MAX - strlen(f->evidence) - 1);
        }
        buf_free(&ev);
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
    if (access(manifest, F_OK) != 0 && ctx->cfg->pcfg.n_docs > 0) {
        /* universal mode: no stack manifest — the .exoqms.json "docs"
         * list names the files every project must ship */
        size_t missing = 0;
        buf_t ev = {0};
        for (size_t i = 0; i < ctx->cfg->pcfg.n_docs; i++) {
            char doc[2048];
            snprintf(doc, sizeof doc, "%s/%s", ctx->cfg->repo,
                     ctx->cfg->pcfg.docs[i]);
            if (access(doc, F_OK) != 0) {
                missing++;
                buf_printf(&ev, "missing %s\n", ctx->cfg->pcfg.docs[i]);
            } else {
                buf_printf(&ev, "present %s\n", ctx->cfg->pcfg.docs[i]);
            }
        }
        if (missing == 0)
            set_finding(f, "doc-compliance", R_PASS,
                        "%zu/%zu required docs present (.exoqms.json)",
                        ctx->cfg->pcfg.n_docs - missing,
                        ctx->cfg->pcfg.n_docs);
        else
            set_finding(f, "doc-compliance", R_FAIL,
                        "%zu/%zu required docs present (.exoqms.json)",
                        ctx->cfg->pcfg.n_docs - missing,
                        ctx->cfg->pcfg.n_docs);
        if (ev.len + strlen(f->evidence) + 2 < EVID_MAX) {
            strncat(f->evidence, "\n", EVID_MAX - strlen(f->evidence) - 1);
            strncat(f->evidence, ev.p ? ev.p : "",
                    EVID_MAX - strlen(f->evidence) - 1);
        }
        buf_free(&ev);
        return;
    }
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
    if (!ctx->cfg->pcfg.rules_codesafety) {
        set_finding(f, "code-safety", R_SKIP,
                    "disabled by .exoqms.json (rules.code-safety=false)");
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
    char *argv[40];
    int n = 0;
    argv[n++] = ctx->cfg->code_path;
    char *save = NULL;
    for (char *p = strtok_r(dirs, " ", &save); p && n < 36;
         p = strtok_r(NULL, " ", &save))
        argv[n++] = p;
    /* language-adaptive: --lang auto (or the first .exoqms.json
     * languages[] entry when the project pins one) */
    const char *lang = "auto";
    if (ctx->cfg->pcfg.n_languages > 0 && ctx->cfg->pcfg.languages[0][0])
        lang = ctx->cfg->pcfg.languages[0];
    argv[n++] = "--lang";
    argv[n++] = (char *)lang;
    /* project ignore globs from .exoqms.json */
    for (size_t i = 0; i < ctx->cfg->pcfg.n_ignore && n < 38; i++) {
        argv[n++] = "--ignore";
        argv[n++] = ctx->cfg->pcfg.ignore[i];
    }
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

/* ---------- checks 8/9/10: universal rules engine (debt, hygiene,
 * secrets) — exoqms-code --rules. Contract with the rule engine: rule
 * file names are the check ids (`debt-*`, `hygiene-*`, `secrets-*`);
 * the daemon runs exoqms-code <repo> --rules <dir> --ignore <globs>
 * --json ONCE per audit (memoized in the ctx) and partitions the
 * findings by check-id prefix. */

/* where the rule files live: --rules config, else <dirname(code)>/rules,
 * else <repo>/exoqms/code/rules */
static const char *rules_dir(cfg_t *cfg, char *buf, size_t cap)
{
    struct stat st;
    if (cfg->rules_path[0]) {
        if (stat(cfg->rules_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(buf, cap, "%s", cfg->rules_path);
            return buf;
        }
        return NULL;
    }
    if (cfg->code_path[0]) {
        snprintf(buf, cap, "%s", cfg->code_path);
        char *slash = strrchr(buf, '/');
        if (slash) {
            /* <dir>/<binary> -> <dir>/rules, then ../rules (source tree) */
            snprintf(slash + 1, cap - (size_t)(slash + 1 - buf), "rules");
            if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
                char *s2 = strrchr(buf, '/'); /* the "/rules" part */
                if (s2) {
                    *s2 = 0; /* drop "rules": back to <dir> */
                    char *s3 = strrchr(buf, '/'); /* the dir's basename */
                    if (s3)
                        snprintf(s3 + 1, cap - (size_t)(s3 + 1 - buf),
                                 "rules");
                }
            }
        } else {
            snprintf(buf, cap, "rules");
        }
        if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
            return buf;
    }
    snprintf(buf, cap, "%s/exoqms/code/rules", cfg->repo);
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
        return buf;
    return NULL;
}

/* run the shared rules scan once per audit; output memoized in ctx.
 * returns 0 ok (child exit 0 or 1, output in *out), -1 cannot run
 * (reason in err), -2 timed out, 2+ unexpected child exit */
static int rules_scan(check_ctx_t *ctx, char **out, size_t *outlen,
                      char *err, size_t errsz)
{
    if (ctx->rules_cached) {
        *out = ctx->rules_json;
        *outlen = ctx->rules_len;
        return 0;
    }
    if (!ctx->cfg->code_path[0]) {
        snprintf(err, errsz, "no code binary configured (--code)");
        return -1;
    }
    char dirbuf[2048];
    const char *rdir = rules_dir(ctx->cfg, dirbuf, sizeof dirbuf);
    if (!rdir) {
        snprintf(err, errsz, "no rules dir (--rules, next to --code, or "
                 "<repo>/exoqms/code/rules)");
        return -1;
    }
    char *argv[40];
    int n = 0;
    argv[n++] = ctx->cfg->code_path;
    argv[n++] = (char *)ctx->cfg->repo;
    argv[n++] = "--rules";
    argv[n++] = (char *)rdir;
    for (size_t i = 0; i < ctx->cfg->pcfg.n_ignore && n < 36; i++) {
        argv[n++] = "--ignore";
        argv[n++] = ctx->cfg->pcfg.ignore[i];
    }
    argv[n++] = "--json";
    argv[n] = NULL;
    int rc = run_child(argv, ctx->cfg->repo, CHECK_TIMEOUT_S, out, outlen,
                       err, errsz);
    ctx->rules_cached = 1;
    ctx->rules_json = *out;
    ctx->rules_len = *outlen;
    if (rc == -2) {
        snprintf(err, errsz, "rules scan timed out after %ds and was "
                 "killed", CHECK_TIMEOUT_S);
        return -2;
    }
    if (rc == -1)
        return -1;
    return rc;
}

/* count findings whose check id starts with `prefix`; append one
 * `file:line` evidence line per finding. When mask=1 the matched line
 * is never echoed: it is replaced with *** (secrets requirement).
 * On success *res stays R_PASS and 0 is returned; otherwise *res is
 * R_SKIP or R_FAIL with the reason in err. */
/* allow: optional list of path substrings; findings whose file matches
 * are excluded (secrets allowlist) */
static int universal_count(check_ctx_t *ctx, const char *prefix, int mask,
                           char *const *allow, size_t nallow,
                           int *count, buf_t *ev, res_t *res, char *err,
                           size_t errsz)
{
    char *out = NULL;
    size_t olen = 0;
    int rc = rules_scan(ctx, &out, &olen, err, errsz);
    if (rc == -2) {
        *res = R_FAIL;
        return -1;
    }
    if (rc == -1) {
        *res = R_SKIP;
        return -1;
    }
    if (rc > 1) {
        *res = R_FAIL;
        snprintf(err, errsz, "rules engine exited %d: %s", rc,
                 out && out[0] ? out : "(no output)");
        return -1;
    }
    *count = 0;
    size_t pos = 0;
    const char *elem;
    size_t elen;
    size_t plen = strlen(prefix);
    while (json_array_each(out, olen, &pos, &elem, &elen)) {
        char *ck = json_field(elem, elen, "check");
        if (!ck)
            continue;
        if (strncmp(ck, prefix, plen) != 0) {
            free(ck);
            continue;
        }
        if (allow && nallow > 0) {
            char *fl = json_field(elem, elen, "file");
            int skip = 0;
            if (fl) {
                for (size_t a = 0; a < nallow; a++)
                    if (strstr(fl, allow[a])) {
                        skip = 1;
                        break;
                    }
                free(fl);
            }
            if (skip) {
                free(ck);
                continue;
            }
        }
        (*count)++;
        if (ev) {
            char *fl = json_field(elem, elen, "file");
            char *ln = json_field(elem, elen, "line");
            if (mask)
                buf_printf(ev, "%s:%s ***\n", fl ? fl : "?",
                           ln ? ln : "?");
            else
                buf_printf(ev, "%s:%s %s\n", fl ? fl : "?", ln ? ln : "?",
                           ck);
            free(fl);
            free(ln);
        }
        free(ck);
    }
    *res = R_PASS;
    return 0;
}

static void finding_append_lines(finding_t *f, const buf_t *ev)
{
    if (!ev || !ev->len)
        return;
    if (ev->len + strlen(f->evidence) + 2 < EVID_MAX) {
        strncat(f->evidence, "\n", EVID_MAX - strlen(f->evidence) - 1);
        strncat(f->evidence, ev->p, EVID_MAX - strlen(f->evidence) - 1);
    }
}

static void check_debt(check_ctx_t *ctx, finding_t *f)
{
    if (!ctx->cfg->code_path[0]) {
        set_finding(f, "debt", R_SKIP, "no code binary configured (--code)");
        return;
    }
    if (!ctx->cfg->pcfg.rules_debt) {
        set_finding(f, "debt", R_SKIP,
                    "disabled by .exoqms.json (rules.debt=false)");
        return;
    }
    int count = 0;
    buf_t ev = {0};
    char err[256] = {0};
    res_t res = R_PASS;
    if (universal_count(ctx, "debt-", 0, NULL, 0, &count, &ev, &res, err,
                        sizeof err) != 0) {
        set_finding(f, "debt", res, "%s", err);
        buf_free(&ev);
        return;
    }
    int thr = ctx->cfg->pcfg.debt_threshold;
    if (count <= thr)
        set_finding(f, "debt", R_PASS,
                    "%d debt finding(s), threshold %d", count, thr);
    else
        set_finding(f, "debt", R_FAIL,
                    "%d debt finding(s) > threshold %d", count, thr);
    finding_append_lines(f, &ev);
    buf_free(&ev);
}

static void check_hygiene(check_ctx_t *ctx, finding_t *f)
{
    if (!ctx->cfg->code_path[0]) {
        set_finding(f, "hygiene", R_SKIP,
                    "no code binary configured (--code)");
        return;
    }
    if (!ctx->cfg->pcfg.rules_hygiene) {
        set_finding(f, "hygiene", R_SKIP,
                    "disabled by .exoqms.json (rules.hygiene=false)");
        return;
    }
    int count = 0;
    buf_t ev = {0};
    char err[256] = {0};
    res_t res = R_PASS;
    if (universal_count(ctx, "hygiene-", 0, NULL, 0, &count, &ev, &res,
                        err, sizeof err) != 0) {
        set_finding(f, "hygiene", res, "%s", err);
        buf_free(&ev);
        return;
    }
    if (count == 0)
        set_finding(f, "hygiene", R_PASS, "0 hygiene finding(s)");
    else
        set_finding(f, "hygiene", R_FAIL, "%d hygiene finding(s)",
                    count);
    finding_append_lines(f, &ev);
    buf_free(&ev);
}

static void check_secrets(check_ctx_t *ctx, finding_t *f)
{
    if (!ctx->cfg->code_path[0]) {
        set_finding(f, "secrets", R_SKIP,
                    "no code binary configured (--code)");
        return;
    }
    if (!ctx->cfg->pcfg.rules_secrets) {
        set_finding(f, "secrets", R_SKIP,
                    "disabled by .exoqms.json (rules.secrets=false)");
        return;
    }
    int count = 0;
    buf_t ev = {0};
    char err[256] = {0};
    res_t res = R_PASS;
    if (universal_count(ctx, "secrets-", 1, ctx->cfg->pcfg.secrets_allow,
                        ctx->cfg->pcfg.n_secrets_allow, &count, &ev, &res,
                        err, sizeof err) != 0) {
        set_finding(f, "secrets", res, "%s", err);
        buf_free(&ev);
        return;
    }
    if (count == 0)
        set_finding(f, "secrets", R_PASS, "0 secrets finding(s)");
    else
        set_finding(f, "secrets", R_FAIL,
                    "%d secrets finding(s) (matched lines masked)", count);
    finding_append_lines(f, &ev);
    buf_free(&ev);
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


/* ---------- check: agent-health (scheduler-driven freeze detection) -----
 * For every agent in --agents: if an exosched reminder mentioning the
 * agent has FIRED (a "fired timer" note), and the agent's latest
 * activity note is older than that fire, the agent is likely frozen:
 * reminder fired, no response, no deliverable. The orchestrator
 * redeploys the agent or takes the task over. */
typedef struct {
    char agent[128];
    int64_t fired;
} fired_ent_t;

static void check_agent_health(check_ctx_t *ctx, finding_t *f)
{
    const char *agents = ctx->agents && ctx->agents[0] ? ctx->agents
                                                        : ctx->cfg->agents;
    char err[256];
    buf_t ev = {0};
    char *copy = xstrdup(agents);
    char *save = NULL;
    int frozen = 0, total = 0;

    /* one scan of the fired-timer notes */
    fired_ent_t fired[64];
    size_t nfired = 0;
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    if (exo_request(ctx->exo, "GET", "/notes?limit=500&q=fired", NULL, 0, 0,
                    &resp, &rlen, &status, err, sizeof err) != 0) {
        set_finding(f, "agent-health", R_FAIL,
                    "exomind unreachable: %s", err);
        free(resp);
        free(copy);
        buf_free(&ev);
        return;
    }
    if (resp) {
        char *line = strdup(resp);
        if (!line) {
            free(resp);
            resp = NULL;
            goto out_notes;
        }
        char *lsave = NULL;
        for (char *l = strtok_r(line, "\n", &lsave); l;
             l = strtok_r(NULL, "\n", &lsave)) {
            if (!strstr(l, "fired timer"))
                continue;
            /* find the agent id inside the message: "agent:<id>" -
               only accept the token when it starts a word */
            const char *ag = l;
            int found = 0;
            while ((ag = strstr(ag, "agent:")) != NULL) {
                if (ag == l || ag[-1] == ' ' || ag[-1] == '\t') {
                    found = 1;
                    break;
                }
                ag += 6;
            }
            if (!found)
                continue;
            char id[128];
            size_t w = 0;
            for (const char *p = ag + 6; *p && *p != ':' && *p != ' ' &&
                 *p != '\t' && w + 1 < sizeof id; p++)
                id[w++] = *p;
            id[w] = 0;
            if (!id[0])
                continue;
            int64_t ep = 0;
            sscanf(l, "note:%lld:", (long long *)&ep);
            if (nfired < 64) {
                snprintf(fired[nfired].agent, sizeof fired[nfired].agent,
                         "%s", id);
                fired[nfired].fired = ep;
                nfired++;
            }
        }
out_notes:
        free(line);
    }
    free(resp);

    for (char *a = strtok_r(copy, ",", &save); a; a = strtok_r(NULL, ",", &save)) {
        while (*a == ' ')
            a++;
        if (!a[0])
            continue;
        total++;
        int64_t fire = 0;
        for (size_t i = 0; i < nfired; i++)
            if (strcmp(fired[i].agent, a) == 0 && fired[i].fired > fire)
                fire = fired[i].fired;

        /* latest activity: newest AGENT note mentioning the agent
           (exclude the fired-timer notes exosched wrote itself) */
        char q[1024];
        snprintf(q, sizeof q, "/notes?limit=100&q=agent%%3A%s", a);
        int64_t act = 0;
        if (exo_request(ctx->exo, "GET", q, NULL, 0, 0, &resp, &rlen,
                        &status, err, sizeof err) == 0 && resp) {
            char *line = strdup(resp);
            if (!line) {
                free(resp);
                resp = NULL;
                break;
            }
            char *lsave = NULL;
            for (char *l = strtok_r(line, "\n", &lsave); l;
                 l = strtok_r(NULL, "\n", &lsave)) {
                if (strstr(l, "fired timer"))
                    continue;
                int64_t ep = 0;
                sscanf(l, "note:%lld:", (long long *)&ep);
                if (ep > act)
                    act = ep;
            }
            free(line);
            free(resp);
            resp = NULL;
        }
        /* deliverable signal: the agent marked itself done */
        char dk[512];
        snprintf(dk, sizeof dk, "agent:%s:done", a);
        char *dv = NULL;
        int done_marker = 0;
        if (exo_get(ctx->exo, dk, &dv, err, sizeof err) == 0 && dv)
            done_marker = 1;
        free(dv);
        if (fire > 0 && fire > act && !done_marker) {
            frozen++;
            buf_printf(&ev,
                       "agent %s: reminder fired %lld, last activity %lld "
                       "(--likely frozen: no response, no deliverable)\n",
                       a, (long long)fire, (long long)act);
        }
    }
    free(copy);
    if (total == 0) {
        set_finding(f, "agent-health", R_SKIP, "no agents configured");
    } else if (frozen > 0) {
        set_finding(f, "agent-health", R_FAIL, "%d/%d agents frozen:\n%s",
                    frozen, total, ev.p ? ev.p : "");
    } else {
        set_finding(f, "agent-health", R_PASS, "%d/%d agents responsive "
                    "(reminders answered or none fired)", total, total);
    }
    buf_free(&ev);
}

/* ---------- check: docs-coverage (every manifest component must carry
 * a README and a test command - the merge gate) ---------- */
static void check_memory_awareness(check_ctx_t *ctx, finding_t *f)
{
    /* the memory mandate: when exomind carries a `mandate` key, every
     * configured agent must have acknowledged it by writing
     * `agent:<id>:ready` — otherwise it is working without memory. */
    const char *agents = ctx->agents && ctx->agents[0] ? ctx->agents
                                                        : ctx->cfg->agents;
    if (!agents || !agents[0]) {
        set_finding(f, "memory-awareness", R_SKIP, "no agents configured");
        return;
    }
    char err[256];
    char *m = NULL;
    if (exo_get(ctx->exo, "mandate", &m, err, sizeof err) != 0) {
        set_finding(f, "memory-awareness", R_SKIP, "exomind unreachable");
        return;
    }
    if (!m) {
        set_finding(f, "memory-awareness", R_SKIP,
                    "no mandate set (exomind --mandate ...)");
        return;
    }
    free(m);
    char *copy = xstrdup(agents);
    char *save = NULL;
    buf_t ev = {0};
    int ready = 0, total = 0;
    for (char *a = strtok_r(copy, ",", &save); a;
         a = strtok_r(NULL, ",", &save)) {
        while (*a == ' ')
            a++;
        if (!a[0])
            continue;
        total++;
        char dk[512];
        snprintf(dk, sizeof dk, "agent:%s:ready", a);
        char *dv = NULL;
        if (exo_get(ctx->exo, dk, &dv, err, sizeof err) == 0 && dv) {
            ready++;
            buf_printf(&ev, "agent:%s:ready ok\n", a);
            free(dv);
        } else {
            buf_printf(&ev, "agent:%s MISSING agent:%s:ready "
                            "(has not acknowledged the mandate)\n", a, a);
        }
    }
    free(copy);
    if (total > 0 && ready == total) {
        set_finding(f, "memory-awareness", R_PASS,
                    "%d/%d agents acknowledged the mandate", ready, total);
    } else {
        set_finding(f, "memory-awareness", R_FAIL,
                    "%d/%d agents acknowledged the mandate", ready, total);
        char *e2 = ev.p ? ev.p : "";
        char *esc = esc_line(e2, strlen(e2));
        strncat(f->evidence, esc, EVID_MAX - strlen(f->evidence) - 1);
        free(esc);
    }
    buf_free(&ev);
}

static void check_kit_fidelity(check_ctx_t *ctx, finding_t *f)
{
    /* the behavioral development kit (exokit): if the repo carries a
     * kit/, the audit gates on it — every contract entry needs examples
     * and every example must pass against the current implementation. */
    if (!ctx->cfg->kit_path[0]) {
        set_finding(f, "kit-fidelity", R_SKIP,
                    "no kit binary configured (--kit)");
        return;
    }
    /* a kit is only expected when the repo has one */
    char kitdir[2048];
    snprintf(kitdir, sizeof kitdir, "%s/kit", ctx->cfg->repo);
    struct stat st;
    if (stat(kitdir, &st) != 0) {
        set_finding(f, "kit-fidelity", R_SKIP,
                    "no kit/ directory in the repo (nothing to verify)");
        return;
    }
    char *argv[16];
    int n = 0;
    argv[n++] = ctx->cfg->kit_path;
    argv[n++] = "audit";
    argv[n++] = "--kit";
    argv[n++] = kitdir;
    argv[n] = NULL;
    char *out = NULL;
    size_t olen = 0;
    char err[256];
    int rc = run_child(argv, ctx->cfg->repo, CHECK_TIMEOUT_S, &out, &olen,
                       err, sizeof err);
    int nf = 0, maj = 0, min = 0;
    json_severity_count(out, &nf, &maj, &min);
    if (rc == -2) {
        set_finding(f, "kit-fidelity", R_FAIL,
                    "timed out after %ds auditing the kit", CHECK_TIMEOUT_S);
    } else if (rc == -1) {
        set_finding(f, "kit-fidelity", R_FAIL, "%s", err);
    } else if (maj > 0) {
        set_finding(f, "kit-fidelity", R_FAIL,
                    "%d major finding(s) in the kit ledger", maj);
        finding_snippet(f, out, olen);
    } else if (rc != 0) {
        set_finding(f, "kit-fidelity", R_FAIL,
                    "kit audit exit %d on %s: %s", rc, kitdir,
                    out && out[0] ? out : "(no output)");
    } else {
        set_finding(f, "kit-fidelity", R_PASS,
                    "kit ledger green: every contract entry exemplified and "
                    "verified");
    }
    free(out);
}

static void check_docs_coverage(check_ctx_t *ctx, finding_t *f)
{
    char manifest[2048];
    snprintf(manifest, sizeof manifest, "%s/docs/stack.tsv", ctx->cfg->repo);
    FILE *fp = fopen(manifest, "r");
    if (!fp) {
        set_finding(f, "docs-coverage", R_SKIP,
                    "no docs/stack.tsv manifest (project mode?)");
        return;
    }
    buf_t ev = {0};
    int missing = 0, total = 0;
    char line[4096];
    while (fgets(line, sizeof line, fp)) {
        trim_crlf(line);
        if (!line[0] || line[0] == '#')
            continue;
        char *col[8];
        int nc = tab_split(line, col, 8);
        if (nc < 2 || !col[0][0] || !col[1][0])
            continue;
        total++;
        char dir[2048];
        if (col[1][0] == '/')
            snprintf(dir, sizeof dir, "%s", col[1]);
        else
            snprintf(dir, sizeof dir, "%s/%s", ctx->cfg->repo, col[1]);
        struct stat st;
        char readme[4096];
        snprintf(readme, sizeof readme, "%s/README.md", dir);
        if (stat(readme, &st) != 0 || !S_ISREG(st.st_mode)) {
            missing++;
            buf_printf(&ev, "%s: no README.md\n", col[0]);
        }
        /* 5th column = test command; `-` is the documented exception
           (suite exceeds the 5s audit budget) */
        if (nc < 5 || !col[4][0] || (col[4][0] == '-' && !col[4][1])) {
            missing++;
            buf_printf(&ev, "%s: no test command (5th column)\n", col[0]);
        }
    }
    fclose(fp);
    if (total == 0) {
        set_finding(f, "docs-coverage", R_SKIP, "empty manifest");
    } else if (missing > 0) {
        set_finding(f, "docs-coverage", R_FAIL, "%d/%d components missing "
                    "README or test command:\n%s", missing, total,
                    ev.p ? ev.p : "");
    } else {
        set_finding(f, "docs-coverage", R_PASS, "%d/%d components carry "
                    "README + test command", total, total);
    }
    buf_free(&ev);
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
    else if (!strcmp(id, "debt"))
        check_debt(ctx, f);
    else if (!strcmp(id, "hygiene"))
        check_hygiene(ctx, f);
    else if (!strcmp(id, "secrets"))
        check_secrets(ctx, f);
    else if (!strcmp(id, "asset-logic"))
        check_asset_logic(ctx, f);
    else if (!strcmp(id, "agent-health"))
        check_agent_health(ctx, f);
    else if (!strcmp(id, "docs-coverage"))
        check_docs_coverage(ctx, f);
    else if (!strcmp(id, "kit-fidelity"))
        check_kit_fidelity(ctx, f);
    else if (!strcmp(id, "memory-awareness"))
        check_memory_awareness(ctx, f);
    else
        set_finding(f, id, R_SKIP, "unknown check id");
    if (!f->id[0])
        snprintf(f->id, sizeof f->id, "%s", id);
    return 0;
}

void ctx_cleanup(check_ctx_t *ctx)
{
    if (ctx->rules_json) {
        free(ctx->rules_json);
        ctx->rules_json = NULL;
    }
    ctx->rules_cached = 0;
    ctx->rules_len = 0;
}
