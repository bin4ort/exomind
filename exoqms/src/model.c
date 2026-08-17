/* exoqms durable model: quality objectives (9001 §6.2), non-conformities
 * (9001 §8.7/§10.2) and audit programs (ISO 19011). All records live in
 * exomind under exoqms:* keys and are reloaded on startup. */
#include "exoqms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void qms_init(qms_t *q)
{
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->mu, NULL);
}

void qms_free(qms_t *q)
{
    for (size_t i = 0; i < q->n_objs; i++) {
        free(q->objs[i].desc);
        free(q->objs[i].metric);
        free(q->objs[i].target);
        free(q->objs[i].period);
    }
    for (size_t i = 0; i < q->n_ncs; i++) {
        free(q->ncs[i].title);
        free(q->ncs[i].desc);
        free(q->ncs[i].source);
        free(q->ncs[i].caction);
        free(q->ncs[i].evidence);
        free(q->ncs[i].closed_by);
    }
    for (size_t i = 0; i < q->n_audits; i++) {
        free(q->audits[i].name);
        free(q->audits[i].criteria);
        free(q->audits[i].findings);
    }
    free(q->objs);
    free(q->ncs);
    free(q->audits);
    pthread_mutex_destroy(&q->mu);
    memset(q, 0, sizeof *q);
}

static int make_id(char *id, size_t cap)
{
    snprintf(id, cap, "%lld:%08x", (long long)now_epoch(), rand32());
    return 0;
}

/* split an escaped record line back into raw fields; returns field count.
 * The unescaped buffer is owned by the caller (free it after use). */
static int parse_record(const char *raw, char **f, int maxf, char **owned)
{
    char *u = unesc_line(raw);
    int n = tab_split(u, f, maxf);
    *owned = u;
    return n;
}

/* audit records are id<TAB>name<TAB>criteria<TAB>scheduled<TAB>status
 * <TAB>score<TAB>findings, where the findings tail itself carries
 * embedded tabs and newlines: split off the six header fields and hand
 * back the intact tail rather than running it through tab_split (which
 * would truncate the tail at its first tab). Returns 1 on a valid
 * record, 0 otherwise. The unescaped buffer is owned by the caller. */
static int parse_audit_record(const char *raw, char **hdr, int maxhdr,
                              char **tail, char **owned)
{
    char *u = unesc_line(raw);
    char *p = u;
    int n = 0;
    for (; n < maxhdr; n++) {
        hdr[n] = p;
        char *t = strchr(p, '\t');
        if (!t) {
            n++;
            break;
        }
        *t = 0;
        p = t + 1;
    }
    *tail = p;
    *owned = u;
    return n;
}

static void nc_value(const nc_t *nc, buf_t *b)
{
    buf_printf(b, "%s\t%s\t%s\t%s\t%s\t%lld\t%s\t%s\t%s\t%lld\t%s",
               nc->id,
               nc->title ? nc->title : "",
               nc->desc ? nc->desc : "",
               nc->sev,
               nc->source ? nc->source : "",
               (long long)nc->detected_at,
               nc->status,
               nc->caction ? nc->caction : "",
               nc->evidence ? nc->evidence : "",
               (long long)nc->closed_at,
               nc->closed_by ? nc->closed_by : "");
}

int qms_reload(qms_t *q, exo_t *e, char **agents, int *notes24h,
               char *err, size_t errsz)
{
    qms_t fresh;
    qms_init(&fresh);

    char **keys = NULL;
    size_t n = 0;

    if (exo_list(e, OBJ_KEY_PREFIX, &keys, &n, err, errsz) != 0) {
        qms_free(&fresh);
        return -1;
    }
    fresh.objs = xcalloc(n ? n : 1, sizeof(obj_t));
    for (size_t i = 0; i < n; i++) {
        char *v = NULL;
        if (exo_get(e, keys[i], &v, err, errsz) != 0 || !v) {
            free(v);
            continue;
        }
        char *f[8];
        char *owned = NULL;
        int nf = parse_record(v, f, 8, &owned);
        free(v);
        if (nf < 5) {
            free(owned);
            continue;
        }
        obj_t *o = &fresh.objs[fresh.n_objs++];
        snprintf(o->id, sizeof o->id, "%s", f[0]);
        o->desc = xstrdup(f[1]);
        o->metric = xstrdup(f[2]);
        o->target = xstrdup(f[3]);
        o->period = xstrdup(nf > 4 && f[4][0] ? f[4] : "iter");
        o->created = nf > 5 ? strtoll(f[5], NULL, 10) : now_epoch();
        free(owned);
    }
    for (size_t i = 0; i < n; i++)
        free(keys[i]);
    free(keys);

    if (exo_list(e, NC_KEY_PREFIX, &keys, &n, err, errsz) != 0) {
        qms_free(&fresh);
        return -1;
    }
    fresh.ncs = xcalloc(n ? n : 1, sizeof(nc_t));
    for (size_t i = 0; i < n; i++) {
        char *v = NULL;
        if (exo_get(e, keys[i], &v, err, errsz) != 0 || !v) {
            free(v);
            continue;
        }
        char *f[12];
        char *owned = NULL;
        int nf = parse_record(v, f, 12, &owned);
        free(v);
        if (nf < 6) {
            free(owned);
            continue;
        }
        nc_t *c = &fresh.ncs[fresh.n_ncs++];
        snprintf(c->id, sizeof c->id, "%s", f[0]);
        c->title = xstrdup(f[1]);
        c->desc = xstrdup(f[2]);
        snprintf(c->sev, sizeof c->sev, "%s", f[3]);
        c->source = xstrdup(f[4]);
        c->detected_at = strtoll(f[5], NULL, 10);
        snprintf(c->status, sizeof c->status, "%s", nf > 6 && f[6][0] ? f[6] : "open");
        c->caction = xstrdup(nf > 7 ? f[7] : "");
        c->evidence = xstrdup(nf > 8 ? f[8] : "");
        c->closed_at = nf > 9 ? strtoll(f[9], NULL, 10) : 0;
        c->closed_by = xstrdup(nf > 10 ? f[10] : "");
        free(owned);
    }
    for (size_t i = 0; i < n; i++)
        free(keys[i]);
    free(keys);

    if (exo_list(e, AUDIT_KEY_PREFIX, &keys, &n, err, errsz) != 0) {
        qms_free(&fresh);
        return -1;
    }
    fresh.audits = xcalloc(n ? n : 1, sizeof(audit_t));
    for (size_t i = 0; i < n; i++) {
        char *v = NULL;
        if (exo_get(e, keys[i], &v, err, errsz) != 0 || !v) {
            free(v);
            continue;
        }
        char *f[7];
        char *owned = NULL;
        int nf = parse_audit_record(v, f, 6, &f[6], &owned);
        free(v);
        if (nf < 6) {
            free(owned);
            continue;
        }
        audit_t *a = &fresh.audits[fresh.n_audits++];
        snprintf(a->id, sizeof a->id, "%s", f[0]);
        a->name = xstrdup(f[1]);
        a->criteria = xstrdup(f[2]);
        a->scheduled = strtoll(f[3], NULL, 10);
        snprintf(a->status, sizeof a->status, "%s", f[4]);
        a->score = atoi(f[5]);
        a->findings = xstrdup(f[6]);
        free(owned);
    }
    for (size_t i = 0; i < n; i++)
        free(keys[i]);
    free(keys);

    /* persistent config */
    char *v = NULL;
    if (exo_get(e, AGENTS_KEY, &v, err, errsz) == 0 && v) {
        free(*agents);
        *agents = xstrdup(v);
    }
    free(v);
    v = NULL;
    if (exo_get(e, NOTES24_KEY, &v, err, errsz) == 0 && v) {
        int x = atoi(v);
        if (x > 0)
            *notes24h = x;
    }
    free(v);

    pthread_mutex_lock(&q->mu);
    qms_t old = *q;
    *q = fresh;
    pthread_mutex_unlock(&q->mu);
    qms_free(&old);
    return 0;
}

int obj_create(qms_t *q, exo_t *e, const char *desc, const char *metric,
               const char *target, const char *period, char *outid,
               size_t outsz, char *err, size_t errsz)
{
    if (!desc[0] || !metric[0] || !target[0]) {
        snprintf(err, errsz, "objective needs title, metric_key and target");
        return -1;
    }
    char id[ID_MAX];
    make_id(id, sizeof id);
    char key[512];
    snprintf(key, sizeof key, OBJ_KEY_PREFIX "%s", id);
    buf_t v = {0};
    buf_printf(&v, "%s\t%s\t%s\t%s\t%s\t%lld", id, desc, metric, target,
               period, (long long)now_epoch());
    if (exo_persist(e, key, v.p, err, errsz) != 0) {
        buf_free(&v);
        return -1;
    }
    buf_free(&v);
    pthread_mutex_lock(&q->mu);
    q->objs = xrealloc(q->objs, (q->n_objs + 1) * sizeof(obj_t));
    obj_t *o = &q->objs[q->n_objs++];
    snprintf(o->id, sizeof o->id, "%s", id);
    o->desc = xstrdup(desc);
    o->metric = xstrdup(metric);
    o->target = xstrdup(target);
    o->period = xstrdup(period);
    o->created = now_epoch();
    pthread_mutex_unlock(&q->mu);
    if (outid)
        snprintf(outid, outsz, "%s", id);
    return 0;
}

obj_t *obj_find(qms_t *q, const char *id)
{
    for (size_t i = 0; i < q->n_objs; i++)
        if (!strcmp(q->objs[i].id, id))
            return &q->objs[i];
    return NULL;
}

int nc_create(qms_t *q, exo_t *e, const char *title, const char *sev,
              const char *desc, const char *source, char *outid,
              size_t outsz, char *err, size_t errsz)
{
    if (!title[0] || !desc[0]) {
        snprintf(err, errsz, "nc needs title and description");
        return -1;
    }
    if (strcmp(sev, "major") != 0 && strcmp(sev, "minor") != 0) {
        snprintf(err, errsz, "severity must be major or minor");
        return -1;
    }
    char id[ID_MAX];
    make_id(id, sizeof id);
    char key[512];
    snprintf(key, sizeof key, NC_KEY_PREFIX "%s", id);
    nc_t tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.id, sizeof tmp.id, "%s", id);
    tmp.title = (char *)title;
    tmp.desc = (char *)desc;
    snprintf(tmp.sev, sizeof tmp.sev, "%s", sev);
    tmp.source = (char *)source;
    tmp.detected_at = now_epoch();
    snprintf(tmp.status, sizeof tmp.status, "open");
    buf_t v = {0};
    nc_value(&tmp, &v);
    if (exo_persist(e, key, v.p, err, errsz) != 0) {
        buf_free(&v);
        return -1;
    }
    buf_free(&v);
    pthread_mutex_lock(&q->mu);
    q->ncs = xrealloc(q->ncs, (q->n_ncs + 1) * sizeof(nc_t));
    nc_t *c = &q->ncs[q->n_ncs++];
    memset(c, 0, sizeof *c);
    snprintf(c->id, sizeof c->id, "%s", id);
    c->title = xstrdup(title);
    c->desc = xstrdup(desc);
    snprintf(c->sev, sizeof c->sev, "%s", sev);
    c->source = xstrdup(source);
    c->detected_at = now_epoch();
    snprintf(c->status, sizeof c->status, "open");
    c->caction = xstrdup("");
    c->evidence = xstrdup("");
    c->closed_by = xstrdup("");
    pthread_mutex_unlock(&q->mu);
    if (outid)
        snprintf(outid, outsz, "%s", id);
    return 0;
}

nc_t *nc_find(qms_t *q, const char *id)
{
    for (size_t i = 0; i < q->n_ncs; i++)
        if (!strcmp(q->ncs[i].id, id))
            return &q->ncs[i];
    return NULL;
}

/* NC lifecycle: open -> analysis -> corrective -> verify -> closed.
 * `close` is the escape hatch: allowed from ANY status when the body
 * carries corrective_action and evidence (simple rule, documented). */
int nc_transition(qms_t *q, exo_t *e, const char *id, const char *action,
                  const char *body, char *outstatus, size_t outsz,
                  char *err, size_t errsz)
{
    char nst[STATUS_MAX] = {0};
    char *nca = NULL, *nev = NULL, *nby = NULL;
    int64_t nclosed = 0;
    const char *cur = NULL;
    /* snapshot of the record as it must look AFTER the transition; every
     * field is strdup'd under the lock so no pointer can dangle */
    char sid[ID_MAX], stitle[1024], sdesc[MAX_FIELD], ssev[SEV_MAX],
         ssource[1024], sstatus[STATUS_MAX];
    int64_t sdetected = 0, sclosed = 0;
    char *sca = NULL, *sev2 = NULL, *sby = NULL;
    memset(&sid, 0, sizeof sid);
    memset(&stitle, 0, sizeof stitle);
    memset(&sdesc, 0, sizeof sdesc);
    memset(&ssev, 0, sizeof ssev);
    memset(&ssource, 0, sizeof ssource);
    memset(&sstatus, 0, sizeof sstatus);

    pthread_mutex_lock(&q->mu);
    nc_t *c = nc_find(q, id);
    if (!c) {
        pthread_mutex_unlock(&q->mu);
        snprintf(err, errsz, "no such nc");
        return -1;
    }
    cur = c->status;
    if (!strcmp(action, "analyse")) {
        if (strcmp(cur, "open") != 0) {
            pthread_mutex_unlock(&q->mu);
            snprintf(err, errsz, "invalid transition: analyse from %s (expected open)", cur);
            return -1;
        }
        snprintf(nst, sizeof nst, "analysis");
    } else if (!strcmp(action, "correct")) {
        if (strcmp(cur, "analysis") != 0) {
            pthread_mutex_unlock(&q->mu);
            snprintf(err, errsz, "invalid transition: correct from %s (expected analysis)", cur);
            return -1;
        }
        snprintf(nst, sizeof nst, "corrective");
    } else if (!strcmp(action, "verify")) {
        if (strcmp(cur, "corrective") != 0) {
            pthread_mutex_unlock(&q->mu);
            snprintf(err, errsz, "invalid transition: verify from %s (expected corrective)", cur);
            return -1;
        }
        snprintf(nst, sizeof nst, "verify");
    } else if (!strcmp(action, "close")) {
        /* needs corrective_action + evidence unless already at verify */
        char *bf[8];
        int nf = tab_split((char *)body, bf, 8);
        char *ca = nf > 0 ? bf[0] : (char *)"";
        char *ev = nf > 1 ? bf[1] : (char *)"";
        if (strcmp(cur, "verify") != 0 && (!ca[0] || !ev[0])) {
            pthread_mutex_unlock(&q->mu);
            snprintf(err, errsz,
                     "close requires status verify or a body of "
                     "corrective_action<TAB>evidence");
            return -1;
        }
        nca = xstrdup(ca[0] ? ca : c->caction);
        nev = xstrdup(ev[0] ? ev : c->evidence);
        nby = xstrdup(nf > 3 && bf[3][0] ? bf[3] : "api");
        nclosed = now_epoch();
        snprintf(nst, sizeof nst, "closed");
    } else {
        pthread_mutex_unlock(&q->mu);
        snprintf(err, errsz, "unknown action (analyse|correct|verify|close)");
        return -1;
    }
    /* snapshot everything under the lock */
    snprintf(sid, sizeof sid, "%s", c->id);
    snprintf(stitle, sizeof stitle, "%s", c->title);
    snprintf(sdesc, sizeof sdesc, "%s", c->desc);
    snprintf(ssev, sizeof ssev, "%s", c->sev);
    snprintf(ssource, sizeof ssource, "%s", c->source);
    sdetected = c->detected_at;
    snprintf(sstatus, sizeof sstatus, "%s", nst);
    sca = xstrdup(nca ? nca : c->caction);
    sev2 = xstrdup(nev ? nev : c->evidence);
    sby = xstrdup(nby ? nby : c->closed_by);
    sclosed = nclosed ? nclosed : c->closed_at;
    pthread_mutex_unlock(&q->mu);
    free(nca);
    free(nev);
    free(nby);

    /* persist the new state before committing it to memory */
    nc_t copy;
    memset(&copy, 0, sizeof copy);
    snprintf(copy.id, sizeof copy.id, "%s", sid);
    copy.title = stitle;
    copy.desc = sdesc;
    snprintf(copy.sev, sizeof copy.sev, "%s", ssev);
    copy.source = ssource;
    copy.detected_at = sdetected;
    snprintf(copy.status, sizeof copy.status, "%s", sstatus);
    copy.caction = sca;
    copy.evidence = sev2;
    copy.closed_at = sclosed;
    copy.closed_by = sby;
    char key[512];
    snprintf(key, sizeof key, NC_KEY_PREFIX "%s", id);
    buf_t v = {0};
    nc_value(&copy, &v);
    int rc = exo_persist(e, key, v.p, err, errsz);
    buf_free(&v);
    if (rc != 0) {
        free(sca);
        free(sev2);
        free(sby);
        return -1;
    }

    pthread_mutex_lock(&q->mu);
    c = nc_find(q, id);
    if (c) {
        snprintf(c->status, sizeof c->status, "%s", nst);
        if (copy.caction != c->caction) {
            free(c->caction);
            c->caction = xstrdup(copy.caction);
        }
        if (copy.evidence != c->evidence) {
            free(c->evidence);
            c->evidence = xstrdup(copy.evidence);
        }
        if (copy.closed_by != c->closed_by) {
            free(c->closed_by);
            c->closed_by = xstrdup(copy.closed_by);
        }
        c->closed_at = copy.closed_at;
    }
    pthread_mutex_unlock(&q->mu);
    free(sca);
    free(sev2);
    free(sby);

    buf_t note_text = {0};
    buf_printf(&note_text, "nc %s transition: %s -> %s", id, cur, nst);
    if (body && body[0])
        buf_printf(&note_text, " (%s)", body);
    if (exo_note(e, note_text.p, err, sizeof err) != 0)
        fprintf(stderr, "exoqms: transition note failed for %s: %s\n", id,
                err);
    buf_free(&note_text);
    if (outstatus)
        snprintf(outstatus, outsz, "%s", nst);
    return 0;
}

int audit_save(qms_t *q, exo_t *e, const char *id, const char *name,
               const char *criteria, const char *findings, int score,
               char *err, size_t errsz)
{
    char key[512];
    snprintf(key, sizeof key, AUDIT_KEY_PREFIX "%s", id);
    buf_t v = {0};
    buf_printf(&v, "%s\t%s\t%s\t%lld\tdone\t%d\t%s", id, name, criteria,
               (long long)now_epoch(), score, findings);
    char *esc = esc_line(v.p, v.len);
    buf_free(&v);
    int rc = exo_persist(e, key, esc, err, errsz);
    free(esc);
    if (rc != 0)
        return -1;
    pthread_mutex_lock(&q->mu);
    q->audits = xrealloc(q->audits, (q->n_audits + 1) * sizeof(audit_t));
    audit_t *a = &q->audits[q->n_audits++];
    memset(a, 0, sizeof *a);
    snprintf(a->id, sizeof a->id, "%s", id);
    a->name = xstrdup(name);
    a->criteria = xstrdup(criteria);
    a->scheduled = now_epoch();
    snprintf(a->status, sizeof a->status, "done");
    a->score = score;
    a->findings = xstrdup(findings);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

audit_t *audit_find(qms_t *q, const char *id)
{
    for (size_t i = 0; i < q->n_audits; i++)
        if (!strcmp(q->audits[i].id, id))
            return &q->audits[i];
    return NULL;
}

/* detection registry: upsert `issue:<check>` in exomind.
 * Value (TSV): status<TAB>fails<TAB>consecutive<TAB>first_seen
 * <TAB>last_seen<TAB>reopens<TAB>evidence
 * A failing detection opens/increments; a passing one closes (history
 * kept: fails and reopens counters persist). Returns 0 on success. */
int issue_register(exo_t *e, const char *check, int is_fail,
                   const char *evidence, char *err, size_t errsz)
{
    char key[512];
    snprintf(key, sizeof key, "issue:%s", check);
    char *old = NULL;
    char status[32] = "open";
    long fails = 0, consec = 0, reopens = 0;
    int64_t first = now_epoch(), last = now_epoch();
    char ev[240] = "";
    if (exo_get(e, key, &old, err, errsz) == 0 && old) {
        char *copy = xstrdup(old);
        char *save = NULL;
        char *f = strtok_r(copy, "\t", &save);
        if (f)
            snprintf(status, sizeof status, "%s", f);
        f = strtok_r(NULL, "\t", &save);
        if (f)
            fails = atol(f);
        f = strtok_r(NULL, "\t", &save);
        if (f)
            consec = atol(f);
        f = strtok_r(NULL, "\t", &save);
        if (f)
            first = atoll(f);
        f = strtok_r(NULL, "\t", &save);
        if (f)
            last = atoll(f);
        f = strtok_r(NULL, "\t", &save);
        if (f)
            reopens = atol(f);
        f = strtok_r(NULL, "\t", &save);
        if (f)
            snprintf(ev, sizeof ev, "%s", f);
        free(copy);
        free(old);
    }
    last = now_epoch();
    /* a passing check with no history does not create a record */
    if (!is_fail && !old)
        return 0;
    if (is_fail) {
        fails++;
        if (strcmp(status, "closed") == 0) {
            reopens++;
            consec = 1;
        } else {
            consec++;
        }
        snprintf(status, sizeof status, "open");
    } else {
        if (strcmp(status, "open") == 0)
            snprintf(status, sizeof status, "closed");
        consec = 0;
    }
    char *es = esc_line(evidence, strlen(evidence));
    snprintf(ev, sizeof ev, "%s", es[0] ? es : "(no evidence)");
    free(es);
    char val[2048];
    snprintf(val, sizeof val, "%s\t%ld\t%ld\t%lld\t%lld\t%ld\t%s",
             status, fails, consec, (long long)first, (long long)last,
             reopens, ev);
    if (exo_persist(e, key, val, err, errsz) != 0)
        return -1;
    return 0;
}
