/* exosched timers: registry (sorted by deadline) + exactly one timer thread.
 * Internal deadlines use CLOCK_MONOTONIC; wall clock is used only for
 * parsing and for the notes written back into exomind. */
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
    timer_rec_t *t;
    for (t = g_head; t; t = t->next)
        if (strcmp(t->id, id) == 0)
            return t;
    return NULL;
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

/* inserts a timer; mono_fire computed from wall clock delta */
int timer_add(const char *id, int64_t wall_fire, const char *msg)
{
    timer_rec_t *t = xmalloc(sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", id);
    t->wall_fire = wall_fire;
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
    timer_rec_t **pp = &g_head;
    while (*pp && (*pp)->mono_fire <= t->mono_fire)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    pthread_cond_signal(&g_cv);
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
    if (found)
        pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return found;
}

/* writes the fired/missed timer into the exomind note feed and drops the
 * durable key; the TTL is the safety net if we are down at fire time */
static void fire_cleanup(exo_t *e, const timer_rec_t *t, const char *kind)
{
    char err[256];
    buf_t note = {0};
    if (!strcmp(kind, "missed"))
        buf_printf(&note,
                   "missed timer %s: %s (was at %lld, now %lld)", t->id,
                   t->msg, (long long)t->wall_fire, (long long)now_epoch());
    else
        buf_printf(&note, "fired timer %s: %s at %lld", t->id, t->msg,
                   (long long)t->wall_fire);
    if (exo_note(e, note.p, err, sizeof err) != 0)
        fprintf(stderr, "exosched: note failed: %s\n", err);
    buf_free(&note);
    char key[512];
    snprintf(key, sizeof key, EXO_KEY_PREFIX "%s", t->id);
    int existed = 0;
    if (exo_del(e, key, &existed, err, sizeof err) != 0)
        fprintf(stderr, "exosched: del %s failed: %s (ttl will expire it)\n",
                key, err);
}

/* the one and only timer thread: sleeps until the next deadline (100ms
 * granularity), woken by timer_add/timer_cancel via the condvar */
void *timer_loop(void *arg)
{
    g_exo = arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (g_stop)
            break;
        int64_t now = mono_ns();
        timer_rec_t *t = g_head;
        if (t && t->mono_fire <= now) {
            g_head = t->next;
            t->next = NULL;
            pthread_mutex_unlock(&g_mu);
            ws_broadcast(t->id, t->wall_fire, t->msg);
            fire_cleanup(g_exo, t, "fired");
            free(t->msg);
            free(t);
            pthread_mutex_lock(&g_mu);
            continue;
        }
        int64_t wait_ns = 100 * 1000000L; /* 100ms granularity */
        if (t) {
            int64_t to_next = t->mono_fire - now;
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

static void reload_one(exo_t *e, const char *key, const char *value)
{
    const char *sep = strchr(value, '\t');
    if (!sep) {
        fprintf(stderr, "exosched: reload: bad timer value for %s\n", key);
        return;
    }
    char wall[32];
    size_t wlen = (size_t)(sep - value);
    if (wlen >= sizeof wall)
        wlen = sizeof wall - 1;
    memcpy(wall, value, wlen);
    wall[wlen] = 0;
    int64_t fire = (int64_t)strtoll(wall, NULL, 10);
    if (fire <= 0) {
        fprintf(stderr, "exosched: reload: bad epoch in %s\n", key);
        return;
    }
    const char *id = key + strlen(EXO_KEY_PREFIX);
    const char *msg = sep + 1;
    if (fire > now_epoch()) {
        if (timer_add(id, fire, msg) != 0)
            fprintf(stderr, "exosched: reload: duplicate id %s\n", id);
        else
            fprintf(stderr, "exosched: reloaded timer %s (fires in %llds)\n",
                    id, (long long)(fire - now_epoch()));
    } else {
        timer_rec_t fake;
        snprintf(fake.id, sizeof fake.id, "%s", id);
        fake.wall_fire = fire;
        fake.msg = (char *)msg;
        fire_cleanup(e, &fake, "missed");
    }
}

/* reloads timers from exomind at startup: /list then one /batch of /gets.
 * returns 0 on success (even with zero timers), -1 if exomind is down */
int timers_reload(exo_t *e)
{
    char err[256];
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
    for (size_t i = 0; i < n; i++) {
        reload_one(e, keys[i], vals[i]);
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
    fprintf(stderr, "exosched: reload complete (%zu timers found)\n", n);
    return 0;
}
