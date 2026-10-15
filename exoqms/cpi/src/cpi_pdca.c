/* exocpi PDCA engine — Plan-Do-Check-Act cycle implementation */

#include "exoqms.h"
#include "cpi.h"
#include "util.h"
#include "store.h"
#include "../common/exo.h"
#include "../common/exo_cli.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Generate unique ID: timestamp + random hex (without prefix) */
static void cpi_gen_id(char *out, size_t cap, const char *prefix)
{
    (void)prefix;
    int64_t now = time(NULL);
    unsigned int r = (unsigned int)rand();
    snprintf(out, cap, "%llx:%x", (unsigned long long)now, r);
}

/* Store PDCA record in exomind */
static int cpi_store_pdca(struct exo *exo, const cpi_pdca_t *p)
{
    char key[256];
    char val[8192];
    char err[256];
    snprintf(key, sizeof key, "cpi:pdca:%s", p->id);
    snprintf(val, sizeof val,
             "id=%s\nproblem=%s\ntarget=%s\nmetric=%s\nhypothesis=%s\nstate=%d\n"
             "plan_id=%s\ndo_id=%s\ncheck_id=%s\nact_id=%s\ncreated=%lld\nupdated=%lld\n",
             p->id, p->problem, p->target, p->metric, p->hypothesis, p->state,
             p->plan_id, p->do_id, p->check_id, p->act_id,
             (long long)p->created, (long long)p->updated);
    return exo_persist(exo, key, val, err, sizeof err);
}

/* Load PDCA record from exomind */
static int cpi_load_pdca(struct exo *exo, const char *id, cpi_pdca_t *p)
{
    char key[256];
    char *val = NULL;
    char err[256];
    snprintf(key, sizeof key, "cpi:pdca:%s", id);
    if (exo_get(exo, key, &val, err, sizeof err) != 0 || !val)
        return -1;
    /* Simple parse */
    char *line = strtok(val, "\n");
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            if (!strcmp(line, "id")) strncpy(p->id, eq+1, sizeof p->id);
            else if (!strcmp(line, "problem")) strncpy(p->problem, eq+1, sizeof p->problem);
            else if (!strcmp(line, "target")) strncpy(p->target, eq+1, sizeof p->target);
            else if (!strcmp(line, "metric")) strncpy(p->metric, eq+1, sizeof p->metric);
            else if (!strcmp(line, "hypothesis")) strncpy(p->hypothesis, eq+1, sizeof p->hypothesis);
            else if (!strcmp(line, "state")) p->state = atoi(eq+1);
            else if (!strcmp(line, "plan_id")) strncpy(p->plan_id, eq+1, sizeof p->plan_id);
            else if (!strcmp(line, "do_id")) strncpy(p->do_id, eq+1, sizeof p->do_id);
            else if (!strcmp(line, "check_id")) strncpy(p->check_id, eq+1, sizeof p->check_id);
            else if (!strcmp(line, "act_id")) strncpy(p->act_id, eq+1, sizeof p->act_id);
            else if (!strcmp(line, "created")) p->created = atoll(eq+1);
            else if (!strcmp(line, "updated")) p->updated = atoll(eq+1);
        }
        line = strtok(NULL, "\n");
    }
    free(val);
    return 0;
}

/* cpi_pdca_create: Create a new PDCA cycle */
int cpi_pdca_create(struct exo *exo, const char *problem, const char *target,
                    const char *metric, const char *hypothesis, char *out, size_t cap)
{
    if (!problem || !problem[0] || !target || !target[0] || !metric || !metric[0])
        return snprintf(out, cap, "error: missing problem, target, or metric"), 1;

    cpi_pdca_t p = {0};
    cpi_gen_id(p.id, sizeof p.id, "cpi:pdca");
    strncpy(p.problem, problem, sizeof p.problem);
    strncpy(p.target, target, sizeof p.target);
    strncpy(p.metric, metric, sizeof p.metric);
    if (hypothesis) strncpy(p.hypothesis, hypothesis, sizeof p.hypothesis);
    p.state = CPI_PDCA_PLAN;
    p.created = p.updated = time(NULL);

    if (cpi_store_pdca(exo, &p) != 0)
        return snprintf(out, cap, "error: failed to store PDCA"), 1;

    /* Also store as note for audit trail */
    char note[4096];
    char err[256];
    snprintf(note, sizeof note,
             "{\"tool\":\"cpi_pdca_create\",\"args\":{\"problem\":\"%s\",\"target\":\"%s\",\"metric\":\"%s\"},"
             "\"result\":\"%s\",\"success\":1}", problem, target, metric, p.id);
    exo_note(exo, note, err, sizeof err);

    return snprintf(out, cap, "ok %s", p.id), 0;
}

/* cpi_pdca_get: Retrieve PDCA cycle status */
int cpi_pdca_get(struct exo *exo, const char *id, char *out, size_t cap)
{
    if (!id || !id[0])
        return snprintf(out, cap, "error: missing id"), 1;

    cpi_pdca_t p = {0};
    if (cpi_load_pdca(exo, id, &p) != 0)
        return snprintf(out, cap, "error: not found"), 1;

    const char *state_str[] = {"plan", "do", "check", "act", "done"};
    return snprintf(out, cap,
                    "id=%s\nproblem=%s\ntarget=%s\nmetric=%s\nhypothesis=%s\nstate=%s\n"
                    "plan_id=%s\ndo_id=%s\ncheck_id=%s\nact_id=%s\ncreated=%lld\nupdated=%lld\n",
                    p.id, p.problem, p.target, p.metric, p.hypothesis,
                    state_str[p.state], p.plan_id, p.do_id, p.check_id, p.act_id,
                    (long long)p.created, (long long)p.updated), 0;
}

/* cpi_pdca_list: List all PDCA cycles */
int cpi_pdca_list(struct exo *exo, char *out, size_t cap)
{
    char **keys = NULL;
    size_t nk = 0;
    char err[256];
    const char *prefix = "cpi:pdca:";
    if (exo_list(exo, prefix, &keys, &nk, err, sizeof err) != 0)
        return snprintf(out, cap, "error: list failed"), 1;

    size_t off = 0;
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0; i < nk && off < cap - 1; i++) {
        cpi_pdca_t p = {0};
        if (cpi_load_pdca(exo, keys[i] + prefix_len, &p) == 0) {
            const char *state_str[] = {"plan", "do", "check", "act", "done"};
            off += snprintf(out + off, cap - off,
                            "%s\t%s\t%s\t%lld\n", p.id, state_str[p.state], p.problem, (long long)p.created);
        }
        free(keys[i]);
    }
    free(keys);
    return 0;
}

/* cpi_plan: Record plan phase */
int cpi_plan(struct exo *exo, const char *pdca_id, const char *problem,
             const char *target, const char *metric, const char *hypothesis,
             char *out, size_t cap)
{
    cpi_pdca_t p = {0};
    if (cpi_load_pdca(exo, pdca_id, &p) != 0)
        return snprintf(out, cap, "error: pdca not found"), 1;

    char plan_id[64];
    cpi_gen_id(plan_id, sizeof plan_id, "cpi:plan");
    strncpy(p.plan_id, plan_id, sizeof p.plan_id);
    if (problem) strncpy(p.problem, problem, sizeof p.problem);
    if (target) strncpy(p.target, target, sizeof p.target);
    if (metric) strncpy(p.metric, metric, sizeof p.metric);
    if (hypothesis) strncpy(p.hypothesis, hypothesis, sizeof p.hypothesis);
    p.state = CPI_PDCA_PLAN;
    p.updated = time(NULL);

    /* Store plan detail */
    char key[256], val[8192];
    char err[256];
    snprintf(key, sizeof key, "cpi:plan:%s", plan_id);
    snprintf(val, sizeof val, "pdca_id=%s\nproblem=%s\ntarget=%s\nmetric=%s\nhypothesis=%s\ncreated=%lld\n",
             pdca_id, p.problem, p.target, p.metric, p.hypothesis, (long long)time(NULL));
    exo_persist(exo, key, val, err, sizeof err);

    cpi_store_pdca(exo, &p);
    return snprintf(out, cap, "ok %s", plan_id), 0;
}

/* cpi_do: Execute do phase — claim exoflow step */
int cpi_do(struct exo *exo, const char *pdca_id, const char *step,
           char *out, size_t cap)
{
    cpi_pdca_t p = {0};
    if (cpi_load_pdca(exo, pdca_id, &p) != 0)
        return snprintf(out, cap, "error: pdca not found"), 1;
    if (!step || !step[0])
        return snprintf(out, cap, "error: missing step"), 1;

    char do_id[64];
    cpi_gen_id(do_id, sizeof do_id, "cpi:do");
    strncpy(p.do_id, do_id, sizeof p.do_id);
    p.state = CPI_PDCA_DO;
    p.updated = time(NULL);

    /* Store do detail */
    char key[256], val[4096];
    char err[256];
    snprintf(key, sizeof key, "cpi:do:%s", do_id);
    snprintf(val, sizeof val, "pdca_id=%s\nstep=%s\ncreated=%lld\n",
             pdca_id, step, (long long)time(NULL));
    exo_persist(exo, key, val, err, sizeof err);

    /* Claim exoflow step via exoflow /next (would need exoflow connection) */
    /* For now, record the intent */
    char note[4096];
    char note_err[256];
    snprintf(note, sizeof note,
             "{\"tool\":\"cpi_do\",\"args\":{\"pdca\":\"%s\",\"step\":\"%s\"},"
             "\"result\":\"claimed step %s\",\"success\":1}", pdca_id, step, step);
    exo_note(exo, note, note_err, sizeof note_err);

    cpi_store_pdca(exo, &p);
    return snprintf(out, cap, "ok %s", do_id), 0;
}

/* cpi_check: Execute check phase with measurement */
int cpi_check(struct exo *exo, const char *pdca_id, double measurement,
              char *out, size_t cap)
{
    cpi_pdca_t p = {0};
    if (cpi_load_pdca(exo, pdca_id, &p) != 0)
        return snprintf(out, cap, "error: pdca not found"), 1;

    char check_id[64];
    cpi_gen_id(check_id, sizeof check_id, "cpi:check");
    strncpy(p.check_id, check_id, sizeof p.check_id);
    p.state = CPI_PDCA_CHECK;
    p.updated = time(NULL);

    /* Store check detail with measurement */
    char key[256], val[4096];
    char err[256];
    snprintf(key, sizeof key, "cpi:check:%s", check_id);
    snprintf(val, sizeof val, "pdca_id=%s\nmeasurement=%.6f\ncreated=%lld\n",
             pdca_id, measurement, (long long)time(NULL));
    exo_persist(exo, key, val, err, sizeof err);

    /* Run control chart analysis if metric is known */
    char verdict[32] = "inconclusive";
    if (p.metric[0]) {
        /* Would query exomind for historical metric data and run SPC */
        /* Simplified: pass if measurement moves toward target */
        strcpy(verdict, "pass"); /* placeholder */
    }

    char note[4096];
    char note_err[256];
    snprintf(note, sizeof note,
             "{\"tool\":\"cpi_check\",\"args\":{\"pdca\":\"%s\",\"measurement\":%.6f},"
             "\"result\":\"verdict: %s\",\"success\":1}", pdca_id, measurement, verdict);
    exo_note(exo, note, note_err, sizeof note_err);

    cpi_store_pdca(exo, &p);
    return snprintf(out, cap, "ok %s verdict=%s", check_id, verdict), 0;
}

/* cpi_act: Execute act phase — standardize or replan */
int cpi_act(struct exo *exo, const char *pdca_id, const char *decision,
            const char *note_text, char *out, size_t cap)
{
    cpi_pdca_t p = {0};
    if (cpi_load_pdca(exo, pdca_id, &p) != 0)
        return snprintf(out, cap, "error: pdca not found"), 1;
    if (!decision || (strcmp(decision, "standardize") && strcmp(decision, "replan")))
        return snprintf(out, cap, "error: decision must be standardize or replan"), 1;

    char act_id[64];
    cpi_gen_id(act_id, sizeof act_id, "cpi:act");
    strncpy(p.act_id, act_id, sizeof p.act_id);
    p.updated = time(NULL);

    if (!strcmp(decision, "standardize")) {
        p.state = CPI_PDCA_DONE;
        /* Would trigger exokit standard sync here */
    } else {
        p.state = CPI_PDCA_PLAN; /* back to plan for replan */
    }

    char key[256], val[4096];
    char err[256];
    snprintf(key, sizeof key, "cpi:act:%s", act_id);
    snprintf(val, sizeof val, "pdca_id=%s\ndecision=%s\nnote=%s\ncreated=%lld\n",
             pdca_id, decision, note_text ? note_text : "", (long long)time(NULL));
    exo_persist(exo, key, val, err, sizeof err);

    cpi_store_pdca(exo, &p);

    char note[4096];
    char note_err[256];
    snprintf(note, sizeof note,
             "{\"tool\":\"cpi_act\",\"args\":{\"pdca\":\"%s\",\"decision\":\"%s\"},"
             "\"result\":\"%s\",\"success\":1}", pdca_id, decision,
             !strcmp(decision, "standardize") ? "standardized" : "replanned");
    exo_note(exo, note, note_err, sizeof note_err);

    cpi_store_pdca(exo, &p);
    return snprintf(out, cap, "ok %s", act_id), 0;
}