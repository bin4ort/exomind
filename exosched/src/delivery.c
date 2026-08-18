/* exosched delivery receipts: per-fire delivery outcome (did any WS client
 * ACK the push) + cumulative statistics. Exomind is the only source of
 * truth, exactly like the timers: per-fire outcome also lands in the fired
 * note and the 24h key delivery:<id>:<epoch>, and the cumulative counters
 * live durably under `exosched:delivery` (ttl 0 = no expiry). */
#include "exosched.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t fired;
    int64_t acked;   /* fires with >= 1 client ACK */
    int64_t unacked; /* fires with 0 ACKs (incl. 0 clients connected) */
    int64_t last_acked_ts;
    int64_t last_unacked_ts;
} stats_t;

static stats_t g_stats;
static int g_loaded = 0; /* counters were successfully read from exomind */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static char *stats_value(const stats_t *s)
{
    buf_t b = {0};
    buf_printf(&b, "fired: %lld\nacked: %lld\nunacked: %lld\n",
               (long long)s->fired, (long long)s->acked,
               (long long)s->unacked);
    buf_printf(&b, "last_acked_ts: %lld\nlast_unacked_ts: %lld\n",
               (long long)s->last_acked_ts, (long long)s->last_unacked_ts);
    return b.p;
}

/* reads one "name: <num>" field from the persisted value */
static int64_t kv_num(const char *v, const char *key)
{
    const char *p = strstr(v, key);
    if (!p)
        return 0;
    p += strlen(key);
    while (*p == ' ')
        p++;
    return (int64_t)strtoll(p, NULL, 10);
}

static void stats_parse(const char *v, stats_t *s)
{
    memset(s, 0, sizeof *s);
    s->fired = kv_num(v, "fired:");
    s->acked = kv_num(v, "acked:");
    s->unacked = kv_num(v, "unacked:");
    s->last_acked_ts = kv_num(v, "last_acked_ts:");
    s->last_unacked_ts = kv_num(v, "last_unacked_ts:");
}

/* reads the durable counters key; caller holds g_mu */
static int stats_read_locked(exo_t *e, stats_t *s)
{
    char err[256];
    char target[512];
    snprintf(target, sizeof target, "/get?key=%s", DELIVERY_KEY);
    char *resp = NULL;
    size_t rlen = 0;
    int status = 0;
    if (exo_request(e, "GET", target, NULL, 0, 0, &resp, &rlen, &status,
                    err, sizeof err) != 0) {
        fprintf(stderr, "exosched: delivery stats read failed: %s\n", err);
        return -1;
    }
    if (status == 200)
        stats_parse(resp, s);
    else if (status == 404)
        memset(s, 0, sizeof *s);
    else {
        fprintf(stderr,
                "exosched: delivery stats read failed (status %d)\n",
                status);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

int delivery_load(exo_t *e)
{
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    if (!g_loaded)
        rc = stats_read_locked(e, &g_stats);
    if (rc == 0)
        g_loaded = 1;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

/* advances the cumulative counters by one fire and persists them to
 * exomind. The in-memory counters only move after the persist succeeded
 * (fire bookkeeping otherwise retries the whole write, so a fire is never
 * double-counted). If the startup load had failed (exomind was down) the
 * durable counters are read first, so they are not clobbered. */
int delivery_record(exo_t *e, int acked, int total, char *err, size_t errsz)
{
    (void)total; /* per-fire total is the note/keys' business */
    pthread_mutex_lock(&g_mu);
    if (!g_loaded && stats_read_locked(e, &g_stats) != 0) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_loaded = 1;
    stats_t next = g_stats;
    next.fired++;
    if (acked > 0) {
        next.acked++;
        next.last_acked_ts = now_epoch();
    } else {
        next.unacked++;
        next.last_unacked_ts = now_epoch();
    }
    char *v = stats_value(&next);
    int rc = exo_persist(e, DELIVERY_KEY, v, 0, err, errsz);
    free(v);
    if (rc == 0)
        g_stats = next;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

/* /delivery body: cumulative stats, one "key: value" per line */
void delivery_format(buf_t *out)
{
    pthread_mutex_lock(&g_mu);
    stats_t s = g_stats;
    pthread_mutex_unlock(&g_mu);
    char *v = stats_value(&s);
    buf_puts(out, v);
    free(v);
}

/* /delivery?detail=1: per-timer receipts listed from the delivery:* keys */
void delivery_detail(exo_t *e, buf_t *out)
{
    char err[256];
    char **keys = NULL;
    size_t n = 0;
    if (exo_list(e, "delivery:", &keys, &n, err, sizeof err) != 0) {
        fprintf(stderr, "exosched: delivery detail list failed: %s\n", err);
        return;
    }
    if (n == 0)
        return;
    char **vals = NULL;
    if (exo_batch_get(e, keys, n, &vals, err, sizeof err) != 0) {
        fprintf(stderr, "exosched: delivery detail batch failed: %s\n", err);
        for (size_t i = 0; i < n; i++)
            free(keys[i]);
        free(keys);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        /* key = delivery:<id>:<epoch>; the timer id itself contains ':',
         * so the epoch is everything after the last one */
        const char *base = keys[i] + strlen("delivery:");
        const char *last = strrchr(base, ':');
        size_t idlen = last ? (size_t)(last - base) : strlen(base);
        const char *ep = last ? last + 1 : "";
        char *v = unesc_line(vals[i]);
        buf_printf(out, "timer %.*s %s %s\n", (int)idlen, base, ep, v);
        free(v);
    }
    for (size_t i = 0; i < n; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
}