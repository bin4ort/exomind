/* exosched timers: registry (sorted by effective deadline) + exactly one
 * timer thread. Internal deadlines use CLOCK_MONOTONIC; wall clock is used
 * only for parsing and for the notes written back into exomind.
 *
 * Durability protocol (exomind is the only source of truth):
 *  - one-shot value:  "<fire_epoch>\t<msg>"
 *  - recurring value: "<fire_epoch>\t<repeat_s>\t<until_epoch>\t<msg>"
 * msg is esc_line()'d on persist, unescaped on reload.
 *  - on fire: ws push, then write the note and rewrite/delete the key.
 *  - if exomind is down at fire time the timer stays in the registry with
 *    retry_mono set; the note/reschedule/del is retried until it succeeds,
 *    so a fire is never silently lost.
 */
#include "exosched.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
static timer_rec_t *g_head = NULL;
static volatile int g_stop = 0;
static exo_t *g_exo = NULL;
/* bumped under g_mu whenever the registry is changed by a request (add /
 * cancel); a reload snapshot taken before a bump is stale and must be
 * re-run instead of being applied on top of the newer state */
static int64_t g_reload_gen = 0;

void timers_init(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &attr);
    pthread_condattr_destroy(&attr);
}

void timers_shutdown(void)
{
    pthread_mutex_lock(&g_mu);
    g_stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

timer_rec_t *timer_find(const char *id)
{
    timer_rec_t *found = NULL;
    pthread_mutex_lock(&g_mu);
    for (timer_rec_t *t = g_head; t; t = t->next)
        if (strcmp(t->id, id) == 0) {
            found = t;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    return found;
}

size_t timer_count(void)
{
    size_t n = 0;
    pthread_mutex_lock(&g_mu);
    for (timer_rec_t *t = g_head; t; t = t->next)
        n++;
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* deep-copies the registry for lock-free iteration by callers */
timer_rec_t *timers_snapshot(size_t *n)
{
    pthread_mutex_lock(&g_mu);
    size_t cnt = 0;
    for (timer_rec_t *t = g_head; t; t = t->next)
        cnt++;
    timer_rec_t *snap = cnt ? xcalloc(cnt, sizeof *snap) : NULL;
    size_t i = 0;
    for (timer_rec_t *t = g_head; t; t = t->next) {
        snprintf(snap[i].id, sizeof snap[i].id, "%s", t->id);
        snap[i].wall_fire = t->wall_fire;
        snap[i].mono_fire = t->mono_fire;
        snap[i].repeat = t->repeat;
        snap[i].until = t->until;
        snap[i].retry_mono = t->retry_mono;
        snap[i].msg = xstrdup(t->msg);
        i++;
    }
    pthread_mutex_unlock(&g_mu);
    *n = cnt;
    return snap;
}

void timers_snapshot_free(timer_rec_t *snap, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(snap[i].msg);
    free(snap);
}

/* effective deadline: pending exomind retries trump the (already elapsed)
 * fire deadline */
static int64_t eff_due(const timer_rec_t *t)
{
    return t->retry_mono ? t->retry_mono : t->mono_fire;
}

/* inserts under the held lock, sorted by effective deadline */
static void insert_locked(timer_rec_t *t)
{
    timer_rec_t **pp = &g_head;
    while (*pp && eff_due(*pp) <= eff_due(t))
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    pthread_cond_signal(&g_cv);
}

/* inserts a timer; mono_fire computed from the wall-clock delta */
int timer_add(const char *id, int64_t wall_fire, int64_t repeat,
              int64_t until, const char *msg)
{
    timer_rec_t *t = xmalloc(sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", id);
    t->wall_fire = wall_fire;
    t->repeat = repeat;
    t->until = until;
    t->retry_mono = 0;
    t->msg = xstrdup(msg);
    int64_t delta_ns = (wall_fire - now_epoch()) * 1000000000L;
    t->mono_fire = mono_ns() + delta_ns;

    pthread_mutex_lock(&g_mu);
    timer_rec_t *dup;
    for (dup = g_head; dup; dup = dup->next)
        if (strcmp(dup->id, id) == 0)
            break;
    if (dup) {
        pthread_mutex_unlock(&g_mu);
        free(t->msg);
        free(t);
        return -1;
    }
    insert_locked(t);
    g_reload_gen++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int timer_cancel(const char *id)
{
    int found = 0;
    pthread_mutex_lock(&g_mu);
    timer_rec_t **pp = &g_head;
    while (*pp) {
        if (strcmp((*pp)->id, id) == 0) {
            timer_rec_t *t = *pp;
            *pp = t->next;
            free(t->msg);
            free(t);
            found = 1;
            break;
        }
        pp = &(*pp)->next;
    }
    if (found) {
        g_reload_gen++;
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}

/* ---- exomind bookkeeping for a fired timer ------------------------------ */

char *timer_value(int64_t fire, int64_t repeat, int64_t until,
                  const char *msg)
{
    char *esc = esc_line(msg, strlen(msg));
    char *v = xmalloc(strlen(esc) + 96);
    if (repeat > 0)
        snprintf(v, strlen(esc) + 96, "%lld\t%lld\t%lld\t%s",
                 (long long)fire, (long long)repeat, (long long)until, esc);
    else
        snprintf(v, strlen(esc) + 96, "%lld\t%s", (long long)fire, esc);
    free(esc);
    return v;
}

long timer_ttl(int64_t fire)
{
    long ttl = (long)(fire - now_epoch()) + 300;
    if (ttl < 60)
        ttl = 60;
    if (ttl > 315360000)
        ttl = 315360000;
    return ttl;
}/* handles the fire bookkeeping for one timer. The ws push already happened.
 * returns 0 if the timer is done (one-shot fired, or last recurring fire),
 * 1 if it must be kept (recurring, bookkeeping done), 2 if exomind was
 * down and the bookkeeping must be retried (keep, retry later).
 * On success the timer's wall_fire/mono_fire are advanced in place. */
static int fire_timer(exo_t *e, timer_rec_t *t)
{
    char err[256];
    int last = t->repeat > 0 && t->until > 0 &&
               t->wall_fire + t->repeat > t->until;

    buf_t note = {0};
    if (last)
        buf_printf(&note, "fired timer %s: %s at %lld (last fire, until %lld)",
                   t->id, t->msg, (long long)t->wall_fire,
                   (long long)t->until);
    else if (t->repeat > 0)
        buf_printf(&note, "fired timer %s: %s at %lld (repeat %llds)",
                   t->id, t->msg, (long long)t->wall_fire,
                   (long long)t->repeat);
    else
        buf_printf(&note, "fired timer %s: %s at %lld", t->id, t->msg,
                   (long long)t->wall_fire);
    if (exo_note(e, note.p, err, sizeof err) != 0) {
        fprintf(stderr, "exosched: note failed for %s: %s (retrying)\n",
                t->id, err);
        buf_free(&note);
        return 2;
    }
    buf_free(&note);

    char key[512];
    snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", t->id);
    if (!last && t->repeat > 0) {
        /* recurring: rewrite the key with the next fire time */
        int64_t nxt = t->wall_fire + t->repeat;
        char *val = timer_value(nxt, t->repeat, t->until, t->msg);
        if (exo_persist(e, key, val, timer_ttl(nxt), err, sizeof err) != 0) {
            fprintf(stderr,
                    "exosched: reschedule failed for %s: %s (retrying)\n",
                    t->id, err);
            free(val);
            return 2;
        }
        free(val);
        t->wall_fire = nxt;
        int64_t delta_ns = (nxt - now_epoch()) * 1000000000L;
        t->mono_fire = mono_ns() + delta_ns;
        t->retry_mono = 0;
        return 1; /* keep the timer */
    }
    int existed = 0;
    if (exo_del(e, key, &existed, err, sizeof err) != 0) {
        fprintf(stderr, "exosched: del %s failed: %s (retrying)\n", key, err);
        return 2;
    }
    return 0; /* done: caller frees */
}

/* the one and only timer thread: sleeps until the next effective deadline
 * (100ms granularity), woken by timer_add/timer_cancel via the condvar */
void *timer_loop(void *arg)
{
    g_exo = arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (g_stop)
            break;
        int64_t now = mono_ns();
        timer_rec_t *t = g_head;
        if (t && eff_due(t) <= now) {
            g_head = t->next;
            t->next = NULL;
            pthread_mutex_unlock(&g_mu);
            if (!t->retry_mono)
                ws_broadcast(t->id, t->wall_fire, t->msg);
            int keep = fire_timer(g_exo, t);
            pthread_mutex_lock(&g_mu);
            if (keep == 2) {
                /* exomind was down: keep the timer, retry the note /
                 * reschedule / delete later; the fire was not lost */
                t->retry_mono = mono_ns() + RETRY_DELAY_NS;
                insert_locked(t);
            } else if (keep == 1) {
                /* recurring, rescheduled: keep and sleep until next fire */
                t->retry_mono = 0;
                insert_locked(t);
            } else {
                free(t->msg);
                free(t);
            }
            continue;
        }
        int64_t wait_ns = 100 * 1000000L; /* 100ms granularity */
        if (t) {
            int64_t to_next = eff_due(t) - now;
            if (to_next < wait_ns)
                wait_ns = to_next < 0 ? 0 : to_next;
        }
        struct timespec ts;
        ts.tv_sec = now / 1000000000L + wait_ns / 1000000000L;
        ts.tv_nsec = now % 1000000000L + wait_ns % 1000000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec++;
        }
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* writes a "missed timer ..." note and drops the durable key */
static void drop_missed(exo_t *e, const char *id, const char *msg,
                        int64_t fire, const char *reason)
{
    char err[256];
    buf_t note = {0};
    if (reason)
        buf_printf(&note, "missed timer %s: %s (was at %lld, now %lld; %s)",
                   id, msg, (long long)fire, (long long)now_epoch(), reason);
    else
        buf_printf(&note,
                   "missed timer %s: %s (was at %lld, now %lld)", id, msg,
                   (long long)fire, (long long)now_epoch());
    if (exo_note(e, note.p, err, sizeof err) != 0)
        fprintf(stderr, "exosched: note failed: %s\n", err);
    buf_free(&note);
    char key[512];
    snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", id);
    int existed = 0;
    if (exo_del(e, key, &existed, err, sizeof err) != 0)
        fprintf(stderr, "exosched: del %s failed: %s (ttl will expire it)\n",
                key, err);
}

/* sentinel reason for a plain overdue one-shot (note keeps its original
 * form, without the "; <reason>" suffix) */
#define REASON_OVERDUE_ONE_SHOT ((const char *)1)

/* parses one timer value into an add or a drop; never touches the registry.
 * returns 0 on success (add: *out_* set; drop: *out_reason set), -1 on a
 * bad value (skipped, already logged). */
static int reload_parse(const char *key, const char *value, char *id,
                        size_t idcap, int64_t *out_fire, int64_t *out_repeat,
                        int64_t *out_until, char **out_msg,
                        const char **out_reason)
{
    int64_t fire = 0, repeat = 0, until = 0;
    const char *p = value;
    char *end = NULL;
    fire = strtoll(p, &end, 10);
    if (end == p || *end != '\t' || fire <= 0) {
        fprintf(stderr, "exosched: reload: bad timer value for %s\n", key);
        return -1;
    }
    p = end + 1;
    char *end2 = NULL;
    int64_t r2 = strtoll(p, &end2, 10);
    char *msg = NULL;
    if (end2 > p && *end2 == '\t' && r2 > 0) {
        /* recurring: <fire>\t<repeat>\t<until>\t<msg> */
        repeat = r2;
        char *end3 = NULL;
        until = strtoll(end2 + 1, &end3, 10);
        if (end3 == end2 + 1 || *end3 != '\t' || until < 0) {
            fprintf(stderr, "exosched: reload: bad recurring value for %s\n",
                    key);
            return -1;
        }
        msg = unesc_line(end3 + 1);
    } else {
        /* one-shot: <fire>\t<msg> */
        msg = unesc_line(p);
    }
    snprintf(id, idcap, "%s", key + strlen(EXO_KEY_PREFIX));
    *out_fire = fire;
    *out_repeat = repeat;
    *out_until = until;
    *out_msg = msg;
    *out_reason = NULL;
    return 0;
}

/* inserts a reloaded timer; caller holds g_mu. returns 0 added, 1 already
 * present (skip), -1 registry changed since the snapshot was taken (the
 * whole reload must be aborted and re-run against fresh state). */
static int reload_add_locked(const char *id, int64_t wall_fire, int64_t repeat,
                             int64_t until, const char *msg, int64_t gen)
{
    if (gen != g_reload_gen)
        return -1;
    for (timer_rec_t *t = g_head; t; t = t->next)
        if (strcmp(t->id, id) == 0)
            return 1;
    timer_rec_t *t = xmalloc(sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", id);
    t->wall_fire = wall_fire;
    t->repeat = repeat;
    t->until = until;
    t->retry_mono = 0;
    t->msg = xstrdup(msg);
    int64_t delta_ns = (wall_fire - now_epoch()) * 1000000000L;
    t->mono_fire = mono_ns() + delta_ns;
    insert_locked(t);
    return 0;
}

/* reloads timers from exomind at startup: /list then one /batch of /gets.
 * returns 0 on success (even with zero timers), -1 if exomind is down or
 * the snapshot went stale (registry changed mid-reload; caller retries). */
int timers_reload(exo_t *e)
{
    char err[256];
    pthread_mutex_lock(&g_mu);
    int64_t gen = g_reload_gen;
    pthread_mutex_unlock(&g_mu);
    char **keys = NULL;
    size_t n = 0;
    if (exo_list(e, EXO_KEY_PREFIX, &keys, &n, err, sizeof err) != 0) {
        fprintf(stderr, "exosched: reload failed: %s\n", err);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "exosched: no timers to reload\n");
        return 0;
    }
    char **vals = NULL;
    if (exo_batch_get(e, keys, n, &vals, err, sizeof err) != 0) {
        fprintf(stderr, "exosched: reload batch failed: %s\n", err);
        for (size_t i = 0; i < n; i++)
            free(keys[i]);
        free(keys);
        return -1;
    }

    /* phase 1: parse the snapshot into pending adds / drops (pure CPU) */
    char **ids = xcalloc(n, sizeof *ids);
    int64_t *fires = xcalloc(n, sizeof *fires);
    int64_t *repeats = xcalloc(n, sizeof *repeats);
    int64_t *untils = xcalloc(n, sizeof *untils);
    char **msgs = xcalloc(n, sizeof *msgs);
    const char **reasons = xcalloc(n, sizeof *reasons);
    size_t np = 0;
    for (size_t i = 0; i < n; i++) {
        char *id = xmalloc(TIMER_ID_MAX);
        int64_t fire, repeat, until;
        char *msg;
        const char *reason = NULL;
        if (reload_parse(keys[i], vals[i], id, TIMER_ID_MAX, &fire, &repeat,
                         &until, &msg, &reason) != 0) {
            free(id);
            continue;
        }
        if (repeat > 0) {
            int64_t orig_fire = fire;
            if (until > 0 && fire >= until) {
                reason = "recurring: until already reached";
            } else {
                while (fire <= now_epoch() &&
                       (until == 0 || fire + repeat <= until))
                    fire += repeat;
                if (fire <= now_epoch())
                    reason = "recurring: until reached while down";
            }
            if (fire != orig_fire)
                fprintf(stderr,
                        "exosched: reloaded recurring timer %s (caught up to "
                        "%lld, repeat %llds)\n",
                        id, (long long)fire, (long long)repeat);
        } else if (fire <= now_epoch()) {
            reason = REASON_OVERDUE_ONE_SHOT;
        }
        ids[np] = id;
        fires[np] = fire;
        repeats[np] = repeat;
        untils[np] = until;
        msgs[np] = msg;
        reasons[np] = reason;
        np++;
    }

    /* phase 2: drops (missed-timer notes + key cleanup): HTTP, no lock */
    for (size_t i = 0; i < np; i++) {
        if (reasons[i])
            drop_missed(e, ids[i], msgs[i], fires[i],
                        reasons[i] == REASON_OVERDUE_ONE_SHOT ? NULL
                                                             : reasons[i]);
    }

    /* phase 3: adds, under the lock, re-validating the snapshot per key so
     * a timer cancelled (or added) while we were reading cannot be
     * resurrected by this stale snapshot */
    pthread_mutex_lock(&g_mu);
    int stale = 0;
    for (size_t i = 0; i < np; i++) {
        if (reasons[i])
            continue;
        int rc = reload_add_locked(ids[i], fires[i], repeats[i], untils[i],
                                   msgs[i], gen);
        if (rc == -1) {
            stale = 1;
            break;
        }
        if (rc == 1)
            fprintf(stderr, "exosched: reload: duplicate id %s\n", ids[i]);
        else
            fprintf(stderr, "exosched: reloaded timer %s (fires in %llds%s)\n",
                    ids[i], (long long)(fires[i] - now_epoch()),
                    repeats[i] > 0 ? ", recurring" : "");
    }
    pthread_mutex_unlock(&g_mu);

    for (size_t i = 0; i < n; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
    for (size_t i = 0; i < np; i++) {
        free(ids[i]);
        free(msgs[i]);
    }
    free(ids);
    free(fires);
    free(repeats);
    free(untils);
    free(msgs);
    free(reasons);

    if (stale) {
        fprintf(stderr,
                "exosched: reload aborted: registry changed mid-reload; "
                "retrying against fresh state\n");
        return -1;
    }
    fprintf(stderr, "exosched: reload complete (%zu timers found)\n", n);
    return 0;
}
