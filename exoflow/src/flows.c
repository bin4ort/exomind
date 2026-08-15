/* exoflow flow model + registry: dependency graphs, claims, deadlines.
 * Durable state lives in exomind under `exoflow:flow:<id>`; the in-memory
 * registry is a cache whose consistency is enforced by one mutex. */
#include "exoflow.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static flow_t *g_flows = NULL;
static size_t g_nflows = 0;
static uint64_t g_gen = 0; /* bumped on add/delete; stale-reload detector */

void flows_init(void)
{
    /* registry starts empty; flows_reload() fills it */
}

void flows_lock(void)
{
    pthread_mutex_lock(&g_mu);
}

void flows_unlock(void)
{
    pthread_mutex_unlock(&g_mu);
}

/* caller must hold the registry lock */
flow_t *flow_find(const char *id)
{
    for (flow_t *f = g_flows; f; f = f->next)
        if (strcmp(f->id, id) == 0)
            return f;
    return NULL;
}

/* head of the registry list; caller must hold the registry lock */
flow_t *flows_first(void)
{
    return g_flows;
}

size_t flow_count(void)
{
    size_t n;
    flows_lock();
    n = g_nflows;
    flows_unlock();
    return n;
}

const char *flow_status(const flow_t *f)
{
    int all_done = 1, any_cancelled = 0, any_active = 0, any_failed = 0;
    for (size_t i = 0; i < f->nsteps; i++) {
        const step_t *s = &f->steps[i];
        if (strcmp(s->state, "done") != 0)
            all_done = 0;
        if (strcmp(s->state, "cancelled") == 0)
            any_cancelled = 1;
        if (strcmp(s->state, "pending") == 0 || strcmp(s->state, "claimed") == 0 ||
            strcmp(s->state, "overdue") == 0)
            any_active = 1;
        if (strcmp(s->state, "failed") == 0)
            any_failed = 1;
    }
    if (all_done)
        return "done";
    if (any_cancelled)
        return "cancelled";
    if (!any_active && any_failed)
        return "failed";
    return "active";
}

/* step ids and worker names are restricted so they stay safe inside TSV
 * lines, URLs, and the comma-joined deps column */
static int safe_token(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n == 0 || n >= max)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* flow ids are generated as `<epoch>:<hex>` and travel inside URLs and TSV
 * columns; only whitespace and the column separator are really unsafe */
static int flow_id_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= FLOW_ID_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ' || c == ',')
            return 0;
    }
    return 1;
}

static void step_free(step_t *s)
{
    free(s->desc);
    for (size_t i = 0; i < s->ndeps; i++)
        free(s->deps[i]);
    free(s->deps);
}

static void flow_free(flow_t *f)
{
    for (size_t i = 0; i < f->nsteps; i++)
        step_free(&f->steps[i]);
    free(f->steps);
    free(f->name);
    free(f);
}

/* ---------------- serialization ----------------
 * key `exoflow:flow:<id>` holds a TSV document (format version 2):
 *   line 1: exoflow<TAB>2<TAB><escaped name>
 *   line 2+: step<TAB><sid><TAB><escaped desc><TAB><deps csv><TAB><state><TAB><owner><TAB><deadline>
 *   optional last line (loops only):
 *     loop<TAB><interval s><TAB><max><TAB><until><TAB><iter><TAB><budget><TAB><next_run><TAB><parent><TAB><stopped>
 * v1 documents (header `exoflow<TAB>1<TAB>...`) load as non-looping. */
char *flow_serialize(const flow_t *f)
{
    char *en = esc_line(f->name, strlen(f->name));
    buf_t b = {0};
    buf_printf(&b, "exoflow\t%s\t%s\n", FLOW_TSV_VERSION, en);
    free(en);
    for (size_t i = 0; i < f->nsteps; i++) {
        const step_t *s = &f->steps[i];
        char *ed = esc_line(s->desc, strlen(s->desc));
        buf_printf(&b, "step\t%s\t%s\t", s->id, ed);
        free(ed);
        for (size_t j = 0; j < s->ndeps; j++) {
            if (j)
                buf_puts(&b, ",");
            buf_puts(&b, s->deps[j]);
        }
        buf_printf(&b, "\t%s\t%s\t%lld\n", s->state, s->owner,
                   (long long)s->deadline);
    }
    if (f->loop_active)
        buf_printf(&b, "loop\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%s\t%d\n",
                   (long long)f->loop_interval, (long long)f->loop_max,
                   (long long)f->loop_until, (long long)f->loop_iter,
                   (long long)f->loop_budget, (long long)f->loop_next,
                   f->loop_parent, f->loop_stopped ? 1 : 0);
    return b.p;
}

/* parses a serialized flow; returns 0 and a malloc'd flow, or -1. the
 * returned flow is fully validated (deps exist, no cycles). format
 * version 1 (legacy) loads as a plain non-looping flow; version 2 may
 * carry one trailing `loop` line. */
static int flow_parse(const char *doc, flow_t **out, char *err, size_t errsz)
{
    flow_t *f = xcalloc(1, sizeof *f);
    char *copy = xstrdup(doc);
    size_t nlines = 0;
    char **lines = NULL;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n'))
            line[--l] = 0;
        lines = xrealloc(lines, (nlines + 1) * sizeof(char *));
        lines[nlines++] = line;
    }
    if (nlines < 2) {
        snprintf(err, errsz, "corrupt flow document");
        goto bad;
    }
    char *p1 = strchr(lines[0], '\t');
    if (!p1 || strncmp(lines[0], "exoflow\t", 8) != 0) {
        snprintf(err, errsz, "bad flow header");
        goto bad;
    }
    char *ver = p1 + 1;
    char *t2 = strchr(ver, '\t');
    if (!t2) {
        snprintf(err, errsz, "bad flow header");
        goto bad;
    }
    *t2 = 0;
    if (strcmp(ver, "1") != 0 && strcmp(ver, "2") != 0) {
        snprintf(err, errsz, "unsupported flow format version");
        goto bad;
    }
    f->name = unesc_line(t2 + 1);

    for (size_t i = 1; i < nlines; i++) {
        char *ln = lines[i];
        if (strncmp(ln, "step\t", 5) == 0) {
            char *cols[8];
            size_t nc = 0;
            char *c = ln + 5;
            for (;;) {
                if (nc >= 8) {
                    snprintf(err, errsz, "bad step line");
                    goto bad;
                }
                cols[nc++] = c;
                char *t = strchr(c, '\t');
                if (!t)
                    break;
                *t = 0;
                c = t + 1;
            }
            /* stored form: id desc deps state owner deadline (6 columns);
             * tolerate a bare 3/4-column line (untouched state -> pending) */
            if (nc != 6 && nc != 3 && nc != 4) {
                snprintf(err, errsz, "bad step line");
                goto bad;
            }
            if (!safe_token(cols[0], STEP_ID_MAX)) {
                snprintf(err, errsz, "bad step id");
                goto bad;
            }
            f->steps = xrealloc(f->steps, (f->nsteps + 1) * sizeof(step_t));
            step_t *s = &f->steps[f->nsteps++];
            memset(s, 0, sizeof *s);
            snprintf(s->id, sizeof s->id, "%s", cols[0]);
            s->desc = unesc_line(cols[1]);
            snprintf(s->state, sizeof s->state, "pending");
            char *d = cols[2];
            while (*d) {
                char *comma = strchr(d, ',');
                if (comma)
                    *comma = 0;
                if (!safe_token(d, STEP_ID_MAX)) {
                    snprintf(err, errsz, "bad dep id");
                    goto bad;
                }
                s->deps = xrealloc(s->deps, (s->ndeps + 1) * sizeof(char *));
                s->deps[s->ndeps++] = xstrdup(d);
                if (!comma)
                    break;
                d = comma + 1;
            }
            if (nc == 4) {
                long long dl = atoll(cols[3]);
                if (dl < 0) {
                    snprintf(err, errsz, "bad deadline");
                    goto bad;
                }
                s->deadline = dl;
            } else if (nc == 6) {
                if (strcmp(cols[3], "pending") != 0 &&
                    strcmp(cols[3], "claimed") != 0 &&
                    strcmp(cols[3], "done") != 0 &&
                    strcmp(cols[3], "failed") != 0 &&
                    strcmp(cols[3], "overdue") != 0 &&
                    strcmp(cols[3], "cancelled") != 0) {
                    snprintf(err, errsz, "bad step state");
                    goto bad;
                }
                snprintf(s->state, sizeof s->state, "%s", cols[3]);
                if (strcmp(cols[4], "") != 0 && !safe_token(cols[4], OWNER_MAX)) {
                    snprintf(err, errsz, "bad step owner");
                    goto bad;
                }
                snprintf(s->owner, sizeof s->owner, "%s", cols[4]);
                long long dl = atoll(cols[5]);
                if (dl < 0) {
                    snprintf(err, errsz, "bad deadline");
                    goto bad;
                }
                s->deadline = dl;
            }
        } else if (strncmp(ln, "loop\t", 5) == 0) {
            if (i != nlines - 1) {
                snprintf(err, errsz, "loop line must be last");
                goto bad;
            }
            char *cols[8];
            size_t nc = 0;
            char *c = ln + 5;
            for (;;) {
                if (nc >= 8) {
                    snprintf(err, errsz, "bad loop line");
                    goto bad;
                }
                cols[nc++] = c;
                char *t = strchr(c, '\t');
                if (!t)
                    break;
                *t = 0;
                c = t + 1;
            }
            if (nc != 8) {
                snprintf(err, errsz, "bad loop line");
                goto bad;
            }
            long long iv = atoll(cols[0]), mx = atoll(cols[1]),
                          un = atoll(cols[2]), it = atoll(cols[3]),
                          bd = atoll(cols[4]), nx = atoll(cols[5]);
            if (iv < 1 || mx < 0 || un < 0 || it < 1 || bd < 1 || nx < 0 ||
                (strcmp(cols[6], "") != 0 && !flow_id_ok(cols[6])) ||
                (strcmp(cols[7], "0") != 0 && strcmp(cols[7], "1") != 0)) {
                snprintf(err, errsz, "bad loop line");
                goto bad;
            }
            f->loop_active = 1;
            f->loop_interval = iv;
            f->loop_max = mx;
            f->loop_until = un;
            f->loop_iter = it;
            f->loop_budget = bd;
            f->loop_next = nx;
            snprintf(f->loop_parent, sizeof f->loop_parent, "%s", cols[6]);
            f->loop_stopped = strcmp(cols[7], "1") == 0;
        } else {
            snprintf(err, errsz, "bad step line");
            goto bad;
        }
    }
    free(copy);
    free(lines);
    *out = f;
    return 0;
bad:
    free(copy);
    free(lines);
    flow_free(f);
    return -1;
}

/* ---------------- validation (create path) ---------------- */

static int step_find(const flow_t *f, const char *sid)
{
    for (size_t i = 0; i < f->nsteps; i++)
        if (strcmp(f->steps[i].id, sid) == 0)
            return (int)i;
    return -1;
}

/* 3-color DFS cycle detection over the dep graph */
static int detect_cycle(const flow_t *f)
{
    char *color = xcalloc(f->nsteps ? f->nsteps : 1, 1);
    int cyclic = 0;
    for (size_t i = 0; i < f->nsteps && !cyclic; i++) {
        if (color[i])
            continue;
        /* iterative DFS with an explicit stack */
        size_t *stack = xcalloc(f->nsteps, sizeof(size_t));
        size_t sp = 0;
        stack[sp++] = i;
        color[i] = 1;
        while (sp && !cyclic) {
            size_t cur = stack[sp - 1];
            int advanced = 0;
            for (size_t k = 0; k < f->steps[cur].ndeps; k++) {
                int d = step_find(f, f->steps[cur].deps[k]);
                if (d < 0)
                    continue; /* validated earlier */
                if (color[d] == 1) {
                    cyclic = 1;
                    break;
                }
                if (color[d] == 0) {
                    color[d] = 1;
                    stack[sp++] = (size_t)d;
                    advanced = 1;
                    break;
                }
            }
            if (!advanced && !cyclic) {
                color[cur] = 2;
                sp--;
            }
        }
        free(stack);
    }
    free(color);
    return cyclic;
}

static int make_flow_id_locked(char *id, size_t cap)
{
    for (int try = 0; try < 8; try++) {
        snprintf(id, cap, "%lld:%08x", (long long)now_epoch(),
                 (unsigned)rand32());
        if (!flow_find(id))
            return 0;
    }
    return -1;
}

static int make_flow_id(char *id, size_t cap)
{
    for (int try = 0; try < 8; try++) {
        snprintf(id, cap, "%lld:%08x", (long long)now_epoch(),
                 (unsigned)rand32());
        flows_lock();
        int exists = flow_find(id) != NULL;
        flows_unlock();
        if (!exists)
            return 0;
    }
    return -1;
}

/* ---------------- loop spec ---------------- */

static int64_t parse_units(const char *s)
{
    if (!isdigit((unsigned char)s[0]) || !s[1])
        return 0;
    int64_t n = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            if (!p[1]) {
                if (*p == 's')
                    return n;
                if (*p == 'm')
                    return n * 60;
                if (*p == 'h')
                    return n * 3600;
            }
            return 0;
        }
        n = n * 10 + (*p - '0');
        if (n < 0)
            return 0;
    }
    return 0;
}

/* parses the loop line of a POST /flow body:
 * `loop<TAB>every <n><s|m|h><TAB>[max <n>] [until <epoch>]`
 * returns 0 on success, -1 on a malformed spec. */
static int parse_loop_spec(const char *line, int64_t *interval, int64_t *max,
                           int64_t *until)
{
    *max = 0;
    *until = 0;
    char *copy = xstrdup(line);
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    if (!tok || strcmp(tok, "loop") != 0) {
        free(copy);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &save);
    if (!tok || strcmp(tok, "every") != 0) {
        free(copy);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &save);
    int64_t iv = tok ? parse_units(tok) : 0;
    if (iv < 1) {
        free(copy);
        return -1;
    }
    while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
        char *arg = strtok_r(NULL, " \t", &save);
        if (!arg) {
            free(copy);
            return -1;
        }
        int64_t v = atoll(arg);
        if (strcmp(tok, "max") == 0) {
            if (v < 1) {
                free(copy);
                return -1;
            }
            *max = v;
        } else if (strcmp(tok, "until") == 0) {
            if (v < 0) {
                free(copy);
                return -1;
            }
            *until = v;
        } else {
            free(copy);
            return -1;
        }
    }
    free(copy);
    *interval = iv;
    return 0;
}

int flow_is_loop(const flow_t *f)
{
    return f->loop_active;
}

/* ---------------- persistence helpers ---------------- */

static int persist_flow(cli_t *e, const flow_t *f, char *err, size_t errsz)
{
    char key[512];
    snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", f->id);
    char *doc = flow_serialize(f);
    int rc = exo_persist(e, key, doc, 0, err, errsz);
    free(doc);
    return rc;
}

/* audit note: `flow <f> step <s> -> <state> by <owner>` (+ optional suffix);
 * best effort: a failed note is logged but never fails the operation */
static void audit_note(cli_t *e, const flow_t *f, const step_t *s,
                       const char *state, const char *by, const char *suffix)
{
    buf_t b = {0};
    buf_printf(&b, "flow %s step %s -> %s by %s", f->id, s->id, state, by);
    if (suffix && suffix[0])
        buf_printf(&b, ": %s", suffix);
    char err[256];
    if (exo_note(e, b.p, err, sizeof err) != 0)
        fprintf(stderr, "exoflow: audit note failed (%s): %s\n", err, b.p);
    buf_free(&b);
}

/* sweeps a flow for expired deadlines: any pending/claimed step whose
 * deadline has passed becomes overdue (persisted + audited). caller must
 * hold the registry lock. */
int flow_sweep(cli_t *e, flow_t *f, char *err, size_t errsz)
{
    int64_t now = now_epoch();
    int changed = 0;
    for (size_t i = 0; i < f->nsteps; i++) {
        step_t *s = &f->steps[i];
        if (s->deadline > 0 && s->deadline < now &&
            (strcmp(s->state, "pending") == 0 ||
             strcmp(s->state, "claimed") == 0)) {
            snprintf(s->state, sizeof s->state, "overdue");
            changed = 1;
            audit_note(e, f, s, "overdue",
                       s->owner[0] ? s->owner : "exoflow", NULL);
        }
    }
    if (changed && persist_flow(e, f, err, errsz) != 0)
        return -1;
    return 0;
}

static void set_state(step_t *s, const char *state, const char *owner)
{
    snprintf(s->state, sizeof s->state, "%s", state);
    char tmp[OWNER_MAX];
    snprintf(tmp, sizeof tmp, "%s", owner ? owner : "");
    snprintf(s->owner, sizeof s->owner, "%s", tmp);
}

/* ---------------- operations ---------------- */

/* parses a flow body, persists it, registers deadline reminders, and adds
 * it to the registry. replies via fid/nsteps_out. */
int flow_create(cli_t *e, cli_t *x, const char *body, size_t blen,
                char *fid, size_t fidcap, size_t *nsteps_out,
                char *err, size_t errsz)
{
    char *copy = xstrndup(body, blen);
    size_t nlines = 0;
    char **lines = NULL;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == '\n'))
            line[--l] = 0;
        lines = xrealloc(lines, (nlines + 1) * sizeof(char *));
        lines[nlines++] = line;
    }
    if (nlines == 0) {
        snprintf(err, errsz, "empty body");
        free(copy);
        free(lines);
        return -1;
    }
    char *rawname = lines[0];
    while (*rawname == ' ' || *rawname == '\t')
        rawname++;
    if (!rawname[0]) {
        snprintf(err, errsz, "empty flow name");
        free(copy);
        free(lines);
        return -1;
    }
    if (nlines < 2) {
        snprintf(err, errsz, "no steps");
        free(copy);
        free(lines);
        return -1;
    }
    if (nlines == 2 && strncmp(lines[1], "loop\t", 5) == 0) {
        snprintf(err, errsz, "no steps");
        free(copy);
        free(lines);
        return -1;
    }

    /* an optional LAST line `loop<TAB>every <n><s|m|h>...` turns the flow
     * into a loop. a line starting with `loop\t` whose every-part does not
     * parse is rejected (`error: bad loop spec`); any other last line is a
     * plain step, so old bodies keep working byte for byte. */
    int64_t loop_iv = 0, loop_max = 0, loop_until = 0;
    size_t nstep_lines = nlines - 1;
    if (strncmp(lines[nlines - 1], "loop\t", 5) == 0) {
        if (parse_loop_spec(lines[nlines - 1], &loop_iv, &loop_max,
                            &loop_until) != 0) {
            snprintf(err, errsz, "bad loop spec");
            free(copy);
            free(lines);
            return -1;
        }
        nstep_lines--;
    }

    flow_t *f = xcalloc(1, sizeof *f);
    f->name = unesc_line(rawname);
    f->nsteps = nstep_lines;
    f->steps = xcalloc(f->nsteps, sizeof(step_t));
    if (loop_iv > 0) {
        f->loop_active = 1;
        f->loop_interval = loop_iv;
        f->loop_max = loop_max;
        f->loop_until = loop_until;
        f->loop_iter = 1;
        f->loop_budget = 1;
        f->loop_next = now_epoch() + loop_iv;
    }

    for (size_t i = 1; i <= nstep_lines; i++) {
        step_t *s = &f->steps[i - 1];
        memset(s, 0, sizeof *s);
        snprintf(s->state, sizeof s->state, "pending");
        char *ln = lines[i];
        char *cols[8];
        size_t nc = 0;
        char *c = ln;
        for (;;) {
            if (nc >= 8) {
                snprintf(err, errsz, "bad step line");
                goto bad;
            }
            cols[nc++] = c;
            char *t = strchr(c, '\t');
            if (!t)
                break;
            *t = 0;
            c = t + 1;
        }
        if (nc < 3 || nc > 4) {
            snprintf(err, errsz, "bad step line");
            goto bad;
        }
        if (!safe_token(cols[0], STEP_ID_MAX)) {
            snprintf(err, errsz, "bad step id");
            goto bad;
        }
        if (step_find(f, cols[0]) >= 0) {
            snprintf(err, errsz, "duplicate step %s", cols[0]);
            goto bad;
        }
        snprintf(s->id, sizeof s->id, "%s", cols[0]);
        s->desc = unesc_line(cols[1]);
        char *d = cols[2];
        while (*d) {
            char *comma = strchr(d, ',');
            if (comma)
                *comma = 0;
            if (!safe_token(d, STEP_ID_MAX)) {
                snprintf(err, errsz, "bad dep id");
                goto bad;
            }
            s->deps = xrealloc(s->deps, (s->ndeps + 1) * sizeof(char *));
            s->deps[s->ndeps++] = xstrdup(d);
            if (!comma)
                break;
            d = comma + 1;
        }
        if (nc == 4) {
            long long dl = atoll(cols[3]);
            if (dl < 0) {
                snprintf(err, errsz, "bad deadline");
                goto bad;
            }
            s->deadline = dl;
        }
    }
    /* dep references are resolved after the whole graph is parsed, so a
     * forward reference inside a cycle is still caught by the cycle check */
    for (size_t i = 0; i < f->nsteps; i++) {
        for (size_t j = 0; j < f->steps[i].ndeps; j++)
            if (step_find(f, f->steps[i].deps[j]) < 0) {
                snprintf(err, errsz, "unknown dep %s", f->steps[i].deps[j]);
                goto bad;
            }
    }
    if (detect_cycle(f)) {
        snprintf(err, errsz, "cyclic deps");
        goto bad;
    }

    if (make_flow_id(f->id, sizeof f->id) != 0) {
        snprintf(err, errsz, "cannot allocate flow id");
        goto bad;
    }
    if (persist_flow(e, f, err, errsz) != 0) {
        char uerr[256];
        snprintf(uerr, sizeof uerr, "%s", err);
        snprintf(err, errsz, "exomind unavailable: %s", uerr);
        goto bad;
    }
    flows_lock();
    flow_t *old = flow_find(f->id);
    if (old) {
        flows_unlock();
        snprintf(err, errsz, "flow id collision");
        char derr[256];
        (void)exo_del(e, f->id, NULL, derr, sizeof derr);
        goto bad;
    }
    f->next = g_flows;
    g_flows = f;
    g_nflows++;
    g_gen++;
    flows_unlock();

    /* deadline timers: register on exosched so agents see the fired note in
     * exomind's feed. best effort: overdue-ness is also enforced lazily by
     * the local sweep, so a down exosched never breaks the flow. */
    for (size_t i = 0; i < f->nsteps; i++) {
        step_t *s = &f->steps[i];
        if (s->deadline > 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "exoflow %s %s", f->id, s->id);
            char xerr[256];
            if (xs_remind(x, s->deadline, msg, xerr, sizeof xerr) != 0)
                fprintf(stderr,
                        "exoflow: cannot register deadline timer for %s/%s: "
                        "%s\n",
                        f->id, s->id, xerr);
        }
    }
    /* loop timer: same best-effort pattern. the lazy check is authoritative;
     * the reminder only surfaces the next due iteration in the note feed. */
    if (f->loop_active) {
        char msg[160];
        snprintf(msg, sizeof msg, "exoflow:loop:%s", f->id);
        char xerr[256];
        if (xs_remind(x, f->loop_next, msg, xerr, sizeof xerr) != 0)
            fprintf(stderr, "exoflow: cannot register loop timer for %s: %s\n",
                    f->id, xerr);
    }
    snprintf(fid, fidcap, "%s", f->id);
    *nsteps_out = f->nsteps;
    free(copy);
    free(lines);
    return 0;
bad:
    for (size_t i = 0; i < f->nsteps; i++)
        step_free(&f->steps[i]);
    free(f->steps);
    free(f->name);
    free(f);
    free(copy);
    free(lines);
    return -1;
}

static int loop_stop_all(cli_t *e, flow_t *f, const char *reason,
                         char *err, size_t errsz);

int flow_delete(cli_t *e, const char *id, int *existed, char *err, size_t errsz)
{
    flows_lock();
    flow_t *prev = NULL, *f = g_flows;
    while (f && strcmp(f->id, id) != 0) {
        prev = f;
        f = f->next;
    }
    if (!f) {
        flows_unlock();
        *existed = 0;
        return 0;
    }
    /* deleting any record of a loop halts future iterations first, so the
     * surviving records can never lazily spawn again */
    if (f->loop_active) {
        if (loop_stop_all(e, f, "flow deleted", err, errsz) != 0) {
            flows_unlock();
            return -1;
        }
    }
    if (prev)
        prev->next = f->next;
    else
        g_flows = f->next;
    g_nflows--;
    g_gen++;
    flows_unlock();
    flow_free(f);
    char key[512];
    snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", id);
    int del_existed = 0;
    if (exo_del(e, key, &del_existed, err, errsz) != 0)
        fprintf(stderr, "exoflow: delete key failed for %s: %s\n", id, err);
    *existed = 1;
    return 0;
}

/* cancels every non-terminal step; caller must hold the registry lock */
int flow_cancel(cli_t *e, const char *id, char *err, size_t errsz)
{
    flow_t *f = flow_find(id);
    if (!f) {
        snprintf(err, errsz, "no such flow");
        return -1;
    }
    char *prev_state = xcalloc(f->nsteps, STATE_MAX);
    char *prev_owner = xcalloc(f->nsteps, OWNER_MAX);
    int changed = 0;
    for (size_t i = 0; i < f->nsteps; i++) {
        step_t *s = &f->steps[i];
        snprintf(prev_state + i * STATE_MAX, STATE_MAX, "%s", s->state);
        snprintf(prev_owner + i * OWNER_MAX, OWNER_MAX, "%s", s->owner);
        if (strcmp(s->state, "pending") == 0 || strcmp(s->state, "claimed") == 0 ||
            strcmp(s->state, "overdue") == 0) {
            audit_note(e, f, s, "cancelled", "api", NULL);
            set_state(s, "cancelled", "");
            changed = 1;
        }
    }
    if (changed && persist_flow(e, f, err, errsz) != 0) {
        for (size_t i = 0; i < f->nsteps; i++) {
            snprintf(f->steps[i].state, STATE_MAX, "%s",
                     prev_state + i * STATE_MAX);
            snprintf(f->steps[i].owner, OWNER_MAX, "%s",
                     prev_owner + i * OWNER_MAX);
        }
        char uerr[256];
        snprintf(uerr, sizeof uerr, "%s", err);
        snprintf(err, errsz, "exomind unavailable: %s", uerr);
        free(prev_state);
        free(prev_owner);
        return -1;
    }
    free(prev_state);
    free(prev_owner);
    return 0;
}

/* ---------------- loop scheduler (lazy) ----------------
 * A loop is a chain of flow records, all persisted in exomind. The first
 * record carries the loop spec and no parent; every later iteration links
 * `loop_parent` back to it. Scheduling is LAZY, exactly like the deadline
 * sweep: no background thread, no timer-driven spawn. Every read of
 * /flows, /flow?id=, /loops, /next and every startup reload runs loop_tick
 * per record, which spawns the next iteration only when the newest one is
 * terminal AND `next_run <= now` AND the limits (max / until) allow it.
 * The exosched reminder `exoflow:loop:<id>` is feed candy only. */

/* id of the loop's first iteration */
static const char *loop_root_id(const flow_t *f)
{
    return f->loop_parent[0] ? f->loop_parent : f->id;
}

/* the driver record of f's loop: the one with the highest iter label.
 * Only the driver may schedule the next iteration, so older records with
 * stale next_run copies can never double-spawn. caller must hold the
 * registry lock. */
static flow_t *loop_driver(const flow_t *f)
{
    const char *rid = loop_root_id(f);
    flow_t *best = NULL;
    for (flow_t *r = g_flows; r; r = r->next) {
        if (!r->loop_active)
            continue;
        if (strcmp(r->id, rid) == 0 || strcmp(r->loop_parent, rid) == 0)
            if (!best || r->loop_iter > best->loop_iter)
                best = r;
    }
    return best;
}

static int loop_is_terminal(const flow_t *f)
{
    const char *st = flow_status(f);
    return strcmp(st, "done") == 0 || strcmp(st, "failed") == 0 ||
           strcmp(st, "cancelled") == 0;
}

/* best-effort loop audit note; never fails the caller */
static void loop_note(cli_t *e, const char *text)
{
    char err[256];
    if (exo_note(e, text, err, sizeof err) != 0)
        fprintf(stderr, "exoflow: loop note failed (%s): %s\n", err, text);
}

/* deep-copies src's steps into a fresh all-pending flow named <name> */
static flow_t *loop_clone_steps(const flow_t *src, const char *name)
{
    flow_t *nf = xcalloc(1, sizeof *nf);
    nf->name = xstrdup(name);
    nf->nsteps = src->nsteps;
    nf->steps = xcalloc(src->nsteps, sizeof(step_t));
    for (size_t i = 0; i < src->nsteps; i++) {
        const step_t *ds = &src->steps[i];
        step_t *ns = &nf->steps[i];
        memset(ns, 0, sizeof *ns);
        snprintf(ns->id, sizeof ns->id, "%s", ds->id);
        ns->desc = xstrdup(ds->desc);
        ns->ndeps = ds->ndeps;
        ns->deps = xcalloc(ds->ndeps, sizeof(char *));
        for (size_t j = 0; j < ds->ndeps; j++)
            ns->deps[j] = xstrdup(ds->deps[j]);
        snprintf(ns->state, sizeof ns->state, "pending");
        ns->deadline = ds->deadline;
    }
    return nf;
}

/* marks every record of f's loop stopped (no further spawns) and writes
 * one audit note. caller must hold the registry lock. */
static int loop_stop_all(cli_t *e, flow_t *f, const char *reason,
                         char *err, size_t errsz)
{
    const char *rid = loop_root_id(f);
    int changed = 0;
    for (flow_t *r = g_flows; r; r = r->next) {
        if (!r->loop_active)
            continue;
        if (strcmp(r->id, rid) == 0 || strcmp(r->loop_parent, rid) == 0) {
            if (!r->loop_stopped || r->loop_next != 0) {
                r->loop_stopped = 1;
                r->loop_next = 0;
                char serr[256];
                if (persist_flow(e, r, serr, sizeof serr) != 0) {
                    snprintf(err, errsz, "exomind unavailable: %s", serr);
                    return -1;
                }
                changed = 1;
            }
        }
    }
    if (changed) {
        buf_t b = {0};
        buf_printf(&b, "flow loop %s stopped (%s)", rid, reason);
        loop_note(e, b.p);
        buf_free(&b);
    }
    return 0;
}

/* lazy loop check for one flow. called with the registry lock held, from
 * every read path and the startup reload. spawns the next iteration when
 * the driver record is terminal and due; finalizes (next_run = 0 + audit
 * note) when max or until is exhausted. returns -1 only on persistence
 * failure (nothing was changed). */
int loop_tick(cli_t *e, cli_t *x, flow_t *f, char *err, size_t errsz)
{
    if (!f->loop_active || !loop_is_terminal(f) || f->loop_stopped)
        return 0;
    if (loop_driver(f) != f)
        return 0; /* an older record: the newest iteration drives */
    if (f->loop_next <= 0)
        return 0;
    int64_t now = now_epoch();
    if (f->loop_next > now)
        return 0;
    const char *rid = loop_root_id(f);
    int cancelled = strcmp(flow_status(f), "cancelled") == 0;
    /* an explicitly cancelled iteration does not consume max budget: the
     * replacement keeps the parent's budget and the total counted runs is
     * unchanged */
    int64_t budget_after = f->loop_budget + (cancelled ? 0 : 1);

    if (f->loop_max > 0 && budget_after > f->loop_max) {
        int64_t prev = f->loop_next;
        f->loop_next = 0;
        if (persist_flow(e, f, err, errsz) != 0) {
            f->loop_next = prev;
            return -1;
        }
        buf_t b = {0};
        buf_printf(&b, "flow loop %s finished (max reached)", rid);
        loop_note(e, b.p);
        buf_free(&b);
        return 0;
    }
    if (f->loop_until > 0 && now >= f->loop_until) {
        int64_t prev = f->loop_next;
        f->loop_next = 0;
        if (persist_flow(e, f, err, errsz) != 0) {
            f->loop_next = prev;
            return -1;
        }
        buf_t b = {0};
        buf_printf(&b, "flow loop %s finished (until reached)", rid);
        loop_note(e, b.p);
        buf_free(&b);
        return 0;
    }

    /* spawn the next iteration */
    int64_t new_next = f->loop_next + f->loop_interval;
    int64_t prev = f->loop_next;
    f->loop_next = new_next;
    if (persist_flow(e, f, err, errsz) != 0) {
        f->loop_next = prev;
        return -1;
    }
    char nname[64];
    snprintf(nname, sizeof nname, "iter %lld", (long long)(f->loop_iter + 1));
    flow_t *nf = loop_clone_steps(f, nname);
    if (make_flow_id_locked(nf->id, sizeof nf->id) != 0) {
        flow_free(nf);
        f->loop_next = prev;
        snprintf(err, errsz, "cannot allocate flow id");
        return -1;
    }
    nf->loop_active = 1;
    nf->loop_interval = f->loop_interval;
    nf->loop_max = f->loop_max;
    nf->loop_until = f->loop_until;
    nf->loop_iter = f->loop_iter + 1;
    nf->loop_budget = budget_after;
    nf->loop_next = new_next;
    snprintf(nf->loop_parent, sizeof nf->loop_parent, "%s", rid);
    if (persist_flow(e, nf, err, errsz) != 0) {
        /* roll back so the next lazy check retries without duplicating */
        f->loop_next = prev;
        char perr[256];
        if (persist_flow(e, f, perr, sizeof perr) != 0)
            fprintf(stderr, "exoflow: loop rollback failed for %s: %s\n",
                    f->id, perr);
        flow_free(nf);
        return -1;
    }
    /* propagate the rescheduled next_run to the loop's other records so
     * /loops stays coherent (best effort: the driver's value is the only
     * authoritative one) */
    for (flow_t *r = g_flows; r; r = r->next) {
        if (r == f || r == nf || !r->loop_active)
            continue;
        if (strcmp(r->id, rid) == 0 || strcmp(r->loop_parent, rid) == 0) {
            r->loop_next = new_next;
            char perr[256];
            if (persist_flow(e, r, perr, sizeof perr) != 0)
                fprintf(stderr,
                        "exoflow: loop reschedule persist failed for %s: %s\n",
                        r->id, perr);
        }
    }
    nf->next = g_flows;
    g_flows = nf;
    g_nflows++;
    g_gen++;
    buf_t b = {0};
    buf_printf(&b, "flow loop %s -> iter %lld at %lld", rid,
               (long long)nf->loop_iter, (long long)now);
    loop_note(e, b.p);
    buf_free(&b);
    if (x) {
        /* feed candy only; a due-but-past timer is pointless and exosched
         * rejects it, so skip the refresh when the new run is overdue */
        if (new_next > now) {
            char msg[160];
            snprintf(msg, sizeof msg, "exoflow:loop:%s", rid);
            char xerr[256];
            if (xs_remind(x, new_next, msg, xerr, sizeof xerr) != 0)
                fprintf(stderr,
                        "exoflow: loop timer refresh failed for %s: %s\n",
                        rid, xerr);
        }
    }
    return 0;
}

/* `stop-loop`: halts every future iteration of the loop; the current
 * records keep their state. replies 0 on success. caller must hold the
 * registry lock. */
int flow_stop_loop(cli_t *e, const char *id, char *err, size_t errsz)
{
    flow_t *f = flow_find(id);
    if (!f) {
        snprintf(err, errsz, "no such flow");
        return -1;
    }
    if (!f->loop_active) {
        snprintf(err, errsz, "not a loop");
        return -1;
    }
    return loop_stop_all(e, f, "stop-loop", err, errsz);
}

/* handles `done [note]`, `failed [note]`, `unclaim`; caller must hold the
 * registry lock */
int step_do(cli_t *e, flow_t *f, const char *sid, const char *action,
            const char *note, char *err, size_t errsz)
{
    step_t *s = NULL;
    for (size_t i = 0; i < f->nsteps; i++)
        if (strcmp(f->steps[i].id, sid) == 0) {
            s = &f->steps[i];
            break;
        }
    if (!s) {
        snprintf(err, errsz, "no such step");
        return -1;
    }
    const char *by = s->owner[0] ? s->owner : "api";
    const char *to = NULL;

    if (strcmp(action, "done") == 0) {
        if (strcmp(s->state, "done") == 0) {
            snprintf(err, errsz, "already done");
            return -1;
        }
        if (strcmp(s->state, "cancelled") == 0) {
            snprintf(err, errsz, "step is cancelled");
            return -1;
        }
        for (size_t i = 0; i < s->ndeps; i++) {
            step_t *d = NULL;
            for (size_t j = 0; j < f->nsteps; j++)
                if (strcmp(f->steps[j].id, s->deps[i]) == 0) {
                    d = &f->steps[j];
                    break;
                }
            if (!d || strcmp(d->state, "done") != 0) {
                snprintf(err, errsz, "deps pending");
                return -1;
            }
        }
        to = "done";
    } else if (strcmp(action, "failed") == 0) {
        if (strcmp(s->state, "done") == 0) {
            snprintf(err, errsz, "already done");
            return -1;
        }
        if (strcmp(s->state, "cancelled") == 0) {
            snprintf(err, errsz, "step is cancelled");
            return -1;
        }
        to = "failed";
    } else if (strcmp(action, "unclaim") == 0) {
        if (strcmp(s->state, "claimed") != 0) {
            snprintf(err, errsz, "not claimed");
            return -1;
        }
        to = "pending";
    } else {
        snprintf(err, errsz, "bad action");
        return -1;
    }

    char prev_state[STATE_MAX];
    char prev_owner[OWNER_MAX];
    snprintf(prev_state, sizeof prev_state, "%s", s->state);
    snprintf(prev_owner, sizeof prev_owner, "%s", s->owner);

    if (strcmp(to, "pending") == 0)
        set_state(s, to, "");
    else if (strcmp(to, "failed") == 0)
        set_state(s, to, s->owner);
    else
        set_state(s, to, s->owner);
    if (persist_flow(e, f, err, errsz) != 0) {
        snprintf(s->state, sizeof s->state, "%s", prev_state);
        snprintf(s->owner, sizeof s->owner, "%s", prev_owner);
        char uerr[256];
        snprintf(uerr, sizeof uerr, "%s", err);
        snprintf(err, errsz, "exomind unavailable: %s", uerr);
        return -1;
    }
    audit_note(e, f, s, to, by, note);
    return 0;
}

/* claims the first pending step with all deps done for <worker>; caller
 * must hold the registry lock. replies into sid ("none" if nothing). */
int flow_next(cli_t *e, flow_t *f, const char *worker, char *sid,
              size_t sidcap, char *err, size_t errsz)
{
    if (!safe_token(worker, OWNER_MAX)) {
        snprintf(err, errsz, "bad worker");
        return -1;
    }
    if (flow_sweep(e, f, err, errsz) != 0)
        return -1;
    for (size_t i = 0; i < f->nsteps; i++) {
        step_t *s = &f->steps[i];
        if (strcmp(s->state, "pending") != 0)
            continue;
        int ready = 1;
        for (size_t j = 0; j < s->ndeps; j++) {
            step_t *d = NULL;
            for (size_t k = 0; k < f->nsteps; k++)
                if (strcmp(f->steps[k].id, s->deps[j]) == 0) {
                    d = &f->steps[k];
                    break;
                }
            if (!d || strcmp(d->state, "done") != 0) {
                ready = 0;
                break;
            }
        }
        if (!ready)
            continue;
        set_state(s, "claimed", worker);
        if (persist_flow(e, f, err, errsz) != 0) {
            char uerr[256];
        snprintf(uerr, sizeof uerr, "%s", err);
        snprintf(err, errsz, "exomind unavailable: %s", uerr);
            set_state(s, "pending", "");
            return -1;
        }
        audit_note(e, f, s, "claimed", worker, NULL);
        snprintf(sid, sidcap, "%s", s->id);
        return 0;
    }
    snprintf(sid, sidcap, "none");
    return 0;
}

/* ---------------- reload ---------------- */

static int reload_parse_flow(const char *key, const char *val, flow_t **out)
{
    /* key: exoflow:flow:<id> */
    const char *id = key + strlen(EXO_KEY_PREFIX);
    size_t il = strlen(id);
    if (il == 0 || il >= FLOW_ID_MAX) {
        fprintf(stderr, "exoflow: reload: skipping bad key %s\n", key);
        return -1;
    }
    for (size_t i = 0; i < il; i++)
        if (id[i] == '\t' || id[i] == '\n' || id[i] == ' ') {
            fprintf(stderr, "exoflow: reload: skipping bad key %s\n", key);
            return -1;
        }
    char err[256];
    if (flow_parse(val, out, err, sizeof err) != 0) {
        fprintf(stderr, "exoflow: reload: skipping %s (%s)\n", key, err);
        return -1;
    }
    if (strlen(id) >= sizeof(*out)->id) {
        flow_free(*out);
        return -1;
    }
    snprintf((*out)->id, sizeof(*out)->id, "%s", id);
    return 0;
}

/* reloads all flows from exomind: /list then one /batch of /gets.
 * returns 0 on success (even with zero flows), -1 if exomind is down or
 * the registry changed mid-reload (caller retries). */
int flows_reload(cli_t *e, cli_t *x)
{
    char err[256];
    flows_lock();
    uint64_t gen = g_gen;
    flows_unlock();
    char **keys = NULL;
    size_t n = 0;
    if (exo_list(e, EXO_KEY_PREFIX, &keys, &n, err, sizeof err) != 0) {
        fprintf(stderr, "exoflow: reload failed: %s\n", err);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "exoflow: no flows to reload\n");
        return 0;
    }
    char **vals = NULL;
    if (exo_batch_get(e, keys, n, &vals, err, sizeof err) != 0) {
        fprintf(stderr, "exoflow: reload batch failed: %s\n", err);
        for (size_t i = 0; i < n; i++)
            free(keys[i]);
        free(keys);
        return -1;
    }
    flow_t **parsed = xcalloc(n, sizeof(flow_t *));
    size_t np = 0;
    for (size_t i = 0; i < n; i++) {
        flow_t *f = NULL;
        if (reload_parse_flow(keys[i], vals[i], &f) == 0)
            parsed[np++] = f;
    }
    flows_lock();
    int stale = 0;
    if (g_gen != gen) {
        stale = 1;
    } else {
        for (size_t i = 0; i < np; i++) {
            if (flow_find(parsed[i]->id)) {
                fprintf(stderr, "exoflow: reload: duplicate id %s\n",
                        parsed[i]->id);
                flow_free(parsed[i]);
                parsed[i] = NULL;
                continue;
            }
            parsed[i]->next = g_flows;
            g_flows = parsed[i];
            g_nflows++;
            fprintf(stderr, "exoflow: reloaded flow %s (%s, %zu steps)\n",
                    parsed[i]->id, parsed[i]->name, parsed[i]->nsteps);
            /* same lazy deadline sweep as on every read: a deadline that
             * expired while the daemon was down becomes overdue now */
            char serr[256];
            if (flow_sweep(e, parsed[i], serr, sizeof serr) != 0)
                fprintf(stderr, "exoflow: reload sweep failed for %s: %s\n",
                        parsed[i]->id, serr);
            parsed[i] = NULL; /* now owned by the registry */
        }
        /* lazy loop check, after every record is in the registry so the
         * driver lookup sees the whole loop: a loop whose iteration
         * finished while the daemon was down resumes from persisted state */
        for (flow_t *r = g_flows; r; r = r->next) {
            if (!r->loop_active)
                continue;
            char lerr[256];
            if (loop_tick(e, x, r, lerr, sizeof lerr) != 0)
                fprintf(stderr, "exoflow: reload loop check failed for %s: "
                        "%s\n", r->id, lerr);
        }
    }
    flows_unlock();
    for (size_t i = 0; i < n; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
    for (size_t i = 0; i < np; i++)
        if (parsed[i])
            flow_free(parsed[i]);
    free(parsed);
    if (stale) {
        fprintf(stderr,
                "exoflow: reload aborted: registry changed mid-reload; "
                "retrying\n");
        return -1;
    }
    fprintf(stderr, "exoflow: reload complete (%zu flows found)\n", n);
    return 0;
}
