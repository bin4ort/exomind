/*
 * exomind store: append-only log with an in-memory hash index.
 *
 * On-disk record layout (little-endian fields):
 *   magic[4] "EXM1" | flags[1] | ts_ms[8] | klen[4] | vlen[4]
 *   | ttl_ms[8] | crc32[4] | key[klen] | val[vlen]
 *
 * crc32 covers key+val. Tombstone records (flags & 1) have vlen == 0.
 * Crash recovery: a torn tail record is detected and truncated back to the
 * last good offset. Compaction rewrites live records into a fresh file once
 * dead bytes exceed a threshold.
 */
#include "store.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC "EXM1"
#define HDR_SIZE 33
#define FLAG_TOMB 0x01u
#define MAX_KEY 4096
#define MAX_VAL (8u * 1024u * 1024u)
#define MAX_QUERY 10000
#define COMPACT_MIN_BYTES (64ull * 1024 * 1024)
#define COMPACT_DEAD_PCT 33

typedef struct entry {
    struct entry *next;
    char *key;
    uint32_t klen;
    uint64_t offset; /* record start in the log file */
    uint32_t vsize;
    int64_t ts;        /* write time, ms epoch */
    int64_t expires_at;/* ms epoch, 0 = never */
} entry_t;

struct store {
    char *path;
    int fd;
    uint64_t size;
    entry_t **buckets;
    size_t nbuckets;
    size_t count;
    uint64_t dead_bytes;
    int64_t opened_at;
    pthread_mutex_t mu;
    uint64_t n_reads, n_writes, n_deletes, n_misses;
};

/* ---------------- primitives ---------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint64_t fnv1a(const char *k, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)k[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int expired(const entry_t *e)
{
    return e->expires_at != 0 && now_ms() > e->expires_at;
}

static entry_t **find_slot(store_t *s, const char *key, uint32_t klen,
                           uint64_t h)
{
    size_t idx = (size_t)(h & (s->nbuckets - 1));
    entry_t **p = &s->buckets[idx];
    while (*p) {
        if ((*p)->klen == klen && memcmp((*p)->key, key, klen) == 0)
            return p;
        p = &(*p)->next;
    }
    return p;
}

static void grow(store_t *s)
{
    size_t nn = s->nbuckets * 2;
    entry_t **nb = xcalloc(nn, sizeof(entry_t *));
    for (size_t b = 0; b < s->nbuckets; b++) {
        entry_t *e = s->buckets[b];
        while (e) {
            entry_t *next = e->next;
            size_t idx = (size_t)(fnv1a(e->key, e->klen) & (nn - 1));
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    free(s->buckets);
    s->buckets = nb;
    s->nbuckets = nn;
}

/*
 * Insert or update an entry in the index. Returns the byte count of the
 * superseded record (0 for a brand-new key).
 */
static uint64_t upsert(store_t *s, const char *key, uint32_t klen,
                       uint64_t offset, uint32_t vlen, int64_t ts,
                       int64_t ttl_ms)
{
    if (s->count + 1 > s->nbuckets * 3 / 4)
        grow(s);
    entry_t **slot = find_slot(s, key, klen, fnv1a(key, klen));
    uint64_t dead = 0;
    if (*slot) {
        dead = HDR_SIZE + (*slot)->klen + (*slot)->vsize;
        free((*slot)->key);
        (*slot)->key = xstrndup(key, klen);
        (*slot)->klen = klen;
        (*slot)->offset = offset;
        (*slot)->vsize = vlen;
        (*slot)->ts = ts;
        (*slot)->expires_at = ttl_ms ? ts + ttl_ms : 0;
    } else {
        entry_t *e = xcalloc(1, sizeof(*e));
        e->key = xstrndup(key, klen);
        e->klen = klen;
        e->offset = offset;
        e->vsize = vlen;
        e->ts = ts;
        e->expires_at = ttl_ms ? ts + ttl_ms : 0;
        *slot = e;
        s->count++;
    }
    return dead;
}

/* append one record at *pos (which is advanced); fd is not fsynced here */
static int write_rec_at(store_t *s, int fd, uint64_t *pos, const char *key,
                        uint32_t klen, const char *val, uint32_t vlen,
                        int64_t ts, int64_t ttl_ms, uint8_t flags)
{
    (void)s;
    uint8_t hdr[HDR_SIZE];
    memcpy(hdr, MAGIC, 4);
    hdr[4] = flags;
    put_u64(hdr + 5, (uint64_t)ts);
    put_u32(hdr + 13, klen);
    put_u32(hdr + 17, vlen);
    put_u64(hdr + 21, (uint64_t)ttl_ms);
    uint32_t crc = crc32_init();
    crc32_update(&crc, key, klen);
    crc32_update(&crc, val, vlen);
    put_u32(hdr + 29, crc32_final(crc));

    uint64_t base = *pos;
    if (pwrite(fd, hdr, HDR_SIZE, (off_t)base) != HDR_SIZE)
        return -1;
    if (klen && pwrite(fd, key, klen, (off_t)(base + HDR_SIZE)) != (ssize_t)klen)
        return -1;
    if (vlen &&
        pwrite(fd, val, vlen, (off_t)(base + HDR_SIZE + klen)) != (ssize_t)vlen)
        return -1;
    *pos = base + HDR_SIZE + klen + vlen;
    return 0;
}

static char *read_val(store_t *s, const entry_t *e)
{
    char *v = xmalloc(e->vsize + 1);
    if (pread(s->fd, v, e->vsize,
              (off_t)(e->offset + HDR_SIZE + e->klen)) != (ssize_t)e->vsize) {
        free(v);
        return NULL;
    }
    v[e->vsize] = 0;
    return v;
}

/* ---------------- load & compaction ---------------- */

static void store_load(store_t *s)
{
    struct stat st;
    if (fstat(s->fd, &st) != 0)
        return;
    s->size = (uint64_t)st.st_size;
    uint64_t pos = 0, good = 0;
    uint8_t hdr[HDR_SIZE];
    while (pos + HDR_SIZE <= s->size) {
        if (pread(s->fd, hdr, HDR_SIZE, (off_t)pos) != HDR_SIZE)
            break;
        if (memcmp(hdr, MAGIC, 4) != 0)
            break;
        uint32_t klen = get_u32(hdr + 13), vlen = get_u32(hdr + 17);
        if (klen > MAX_KEY || vlen > MAX_VAL)
            break;
        uint64_t reclen = HDR_SIZE + (uint64_t)klen + vlen;
        if (pos + reclen > s->size)
            break;
        char *kbuf = xmalloc(klen + 1);
        char *vbuf = xmalloc(vlen + 1);
        if (pread(s->fd, kbuf, klen, (off_t)(pos + HDR_SIZE)) != (ssize_t)klen ||
            (vlen && pread(s->fd, vbuf, vlen,
                           (off_t)(pos + HDR_SIZE + klen)) != (ssize_t)vlen)) {
            free(kbuf);
            free(vbuf);
            break;
        }
        kbuf[klen] = 0;
        vbuf[vlen] = 0;
        uint32_t crc = crc32_init();
        crc32_update(&crc, kbuf, klen);
        crc32_update(&crc, vbuf, vlen);
        if (crc32_final(crc) != get_u32(hdr + 29)) {
            fprintf(stderr,
                    "exomind: crc mismatch at offset %llu, truncating log\n",
                    (unsigned long long)pos);
            free(kbuf);
            free(vbuf);
            break;
        }
        uint8_t flags = hdr[4];
        int64_t ts = (int64_t)get_u64(hdr + 5);
        int64_t ttl = (int64_t)get_u64(hdr + 21);
        if (flags & FLAG_TOMB) {
            entry_t **slot = find_slot(s, kbuf, klen, fnv1a(kbuf, klen));
            if (*slot) {
                s->dead_bytes += HDR_SIZE + (*slot)->klen + (*slot)->vsize;
                entry_t *e = *slot;
                *slot = e->next;
                free(e->key);
                free(e);
                s->count--;
            }
        } else {
            s->dead_bytes +=
                upsert(s, kbuf, klen, pos, vlen, ts, ttl);
        }
        free(kbuf);
        free(vbuf);
        good = pos + reclen;
        pos = good;
    }
    if (good != s->size) {
        if (ftruncate(s->fd, (off_t)good) != 0)
            fprintf(stderr, "exomind: truncate failed: %s\n", strerror(errno));
        s->size = good;
    }
}

static void compact(store_t *s)
{
    size_t plen = strlen(s->path);
    char *tmp = xmalloc(plen + 5);
    memcpy(tmp, s->path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    int tfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) {
        fprintf(stderr, "exomind: compact open failed: %s\n", strerror(errno));
        free(tmp);
        return;
    }
    uint64_t pos = 0;
    int64_t now = now_ms();
    for (size_t b = 0; b < s->nbuckets; b++) {
        for (entry_t *e = s->buckets[b]; e; e = e->next) {
            if (e->expires_at && now > e->expires_at)
                continue;
            char *v = read_val(s, e);
            if (!v)
                continue;
            int64_t ttl = e->expires_at ? e->expires_at - e->ts : 0;
            uint64_t start = pos;
            if (write_rec_at(s, tfd, &pos, e->key, e->klen, v, e->vsize,
                             e->ts, ttl, 0) == 0)
                e->offset = start;
            free(v);
        }
    }
    if (fdatasync(tfd) != 0)
        fprintf(stderr, "exomind: compact sync failed: %s\n", strerror(errno));
    close(tfd);
    if (rename(tmp, s->path) != 0) {
        fprintf(stderr, "exomind: compact rename failed: %s\n",
                strerror(errno));
        unlink(tmp);
        free(tmp);
        return;
    }
    close(s->fd);
    s->fd = open(s->path, O_RDWR);
    if (s->fd < 0) {
        fprintf(stderr, "exomind: reopen after compact failed: %s\n",
                strerror(errno));
        exit(1);
    }
    s->size = pos;
    s->dead_bytes = 0;
    free(tmp);
}

static void maybe_compact(store_t *s)
{
    if (s->size < COMPACT_MIN_BYTES)
        return;
    if (s->dead_bytes * 100 < s->size * COMPACT_DEAD_PCT)
        return;
    compact(s);
}

/* ---------------- public API ---------------- */

store_t *store_open(const char *path)
{
    store_t *s = xcalloc(1, sizeof(*s));
    s->path = xstrdup(path);
    s->opened_at = now_ms();
    pthread_mutex_init(&s->mu, NULL);
    s->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (s->fd < 0) {
        fprintf(stderr, "exomind: cannot open %s: %s\n", path,
                strerror(errno));
        exit(1);
    }
    s->nbuckets = 64;
    s->buckets = xcalloc(64, sizeof(entry_t *));
    store_load(s);
    fprintf(stderr, "exomind: loaded %zu entries (%llu bytes) from %s\n",
            s->count, (unsigned long long)s->size, path);
    return s;
}

void store_close(store_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    fdatasync(s->fd);
    close(s->fd);
    for (size_t b = 0; b < s->nbuckets; b++) {
        entry_t *e = s->buckets[b];
        while (e) {
            entry_t *next = e->next;
            free(e->key);
            free(e);
            e = next;
        }
    }
    free(s->buckets);
    free(s->path);
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

int store_set(store_t *s, const char *key, size_t klen, const char *val,
              size_t vlen, int64_t ttl_sec, int append)
{
    if (klen == 0 || klen > MAX_KEY || vlen > MAX_VAL)
        return -1;
    if (ttl_sec < 0)
        ttl_sec = 0;
    pthread_mutex_lock(&s->mu);
    int64_t ts = now_ms();

    char *merged = NULL;
    size_t merged_len = 0;
    if (append) {
        entry_t **slot = find_slot(s, key, (uint32_t)klen, fnv1a(key, klen));
        if (*slot && !expired(*slot)) {
            entry_t *e = *slot;
            char *old = xmalloc(e->vsize + 1);
            if (pread(s->fd, old, e->vsize,
                      (off_t)(e->offset + HDR_SIZE + e->klen)) ==
                (ssize_t)e->vsize) {
                size_t oldlen = e->vsize;
                int sep = (oldlen > 0 && old[oldlen - 1] != '\n') ? 1 : 0;
                merged = xmalloc(oldlen + (size_t)sep + vlen);
                if (oldlen)
                    memcpy(merged, old, oldlen);
                if (sep)
                    merged[oldlen] = '\n';
                memcpy(merged + oldlen + sep, val, vlen);
                merged_len = oldlen + (size_t)sep + vlen;
            }
            free(old);
        }
    }

    const char *wv = merged ? merged : val;
    size_t wlen = merged ? merged_len : vlen;
    uint64_t base = s->size;
    int rc = -1;
    if (write_rec_at(s, s->fd, &s->size, key, (uint32_t)klen, wv,
                     (uint32_t)wlen, ts, ttl_sec * 1000, 0) == 0) {
        s->dead_bytes +=
            upsert(s, key, (uint32_t)klen, base, (uint32_t)wlen, ts,
                   ttl_sec * 1000);
        s->n_writes++;
        rc = 0;
        maybe_compact(s);
    }
    free(merged);
    pthread_mutex_unlock(&s->mu);
    return rc;
}

int store_sync(store_t *s)
{
    pthread_mutex_lock(&s->mu);
    int rc = fdatasync(s->fd);
    pthread_mutex_unlock(&s->mu);
    return rc;
}

char *store_get(store_t *s, const char *key, size_t klen, size_t *vlen,
                int64_t *ts)
{
    pthread_mutex_lock(&s->mu);
    entry_t **slot = find_slot(s, key, (uint32_t)klen, fnv1a(key, klen));
    char *v = NULL;
    if (*slot && !expired(*slot)) {
        entry_t *e = *slot;
        v = read_val(s, e);
        if (v) {
            *vlen = e->vsize;
            if (ts)
                *ts = e->ts;
        }
        s->n_reads++;
    } else {
        s->n_misses++;
    }
    pthread_mutex_unlock(&s->mu);
    return v;
}

int store_del(store_t *s, const char *key, size_t klen)
{
    if (klen == 0 || klen > MAX_KEY)
        return -1;
    pthread_mutex_lock(&s->mu);
    entry_t **slot = find_slot(s, key, (uint32_t)klen, fnv1a(key, klen));
    int existed = 0;
    if (*slot) {
        entry_t *e = *slot;
        existed = !expired(e);
        s->dead_bytes += HDR_SIZE + e->klen + e->vsize;
        *slot = e->next;
        free(e->key);
        free(e);
        s->count--;
    }
    int64_t ts = now_ms();
    int rc = write_rec_at(s, s->fd, &s->size, key, (uint32_t)klen, "", 0, ts, 0,
                          FLAG_TOMB);
    s->n_deletes++;
    maybe_compact(s);
    pthread_mutex_unlock(&s->mu);
    return rc == 0 ? existed : -1;
}

/* ---------------- queries ---------------- */

static void push_kv(kv_t **arr, size_t *cnt, size_t *cap, const char *key,
                    size_t klen, const char *val, size_t vlen, int has_val,
                    int64_t ts, int64_t score)
{
    if (*cnt == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *arr = xrealloc(*arr, *cap * sizeof(kv_t));
    }
    kv_t *k = &(*arr)[*cnt];
    k->key = xstrndup(key, klen);
    k->klen = klen;
    k->val = NULL;
    k->vlen = 0;
    k->has_val = has_val;
    k->ts = ts;
    k->score = score;
    if (has_val) {
        k->val = xmalloc(vlen + 1);
        memcpy(k->val, val, vlen);
        k->val[vlen] = 0;
        k->vlen = vlen;
    }
    (*cnt)++;
}

static int key_score(const char *key, uint32_t klen, const char *sub,
                     size_t slen)
{
    if (!slen)
        return 0;
    if (klen == slen && strncasecmp(key, sub, slen) == 0)
        return 100;
    if (klen >= slen && strncasecmp(key, sub, slen) == 0)
        return 80;
    if (stristr(key, klen, sub, slen))
        return 60;
    return 0;
}

static int cmp_list_asc(const void *a, const void *b)
{
    const kv_t *x = a, *y = b;
    size_t m = x->klen < y->klen ? x->klen : y->klen;
    int c = memcmp(x->key, y->key, m);
    if (c)
        return c;
    return x->klen < y->klen ? -1 : x->klen > y->klen ? 1 : 0;
}

static int cmp_list_desc(const void *a, const void *b)
{
    return -cmp_list_asc(a, b);
}

static int cmp_notes_desc(const void *a, const void *b)
{
    const kv_t *x = a, *y = b;
    if (x->ts != y->ts)
        return x->ts < y->ts ? 1 : -1;
    return cmp_list_desc(a, b);
}

static int cmp_search(const void *a, const void *b)
{
    const kv_t *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return cmp_list_asc(a, b);
}

int store_query(store_t *s, int mode, const char *prefix, const char *substr,
                int desc, kv_t **out, size_t *n_out)
{
    pthread_mutex_lock(&s->mu);
    kv_t *arr = NULL;
    size_t cnt = 0, cap = 0;
    int truncated = 0;
    size_t plen = prefix ? strlen(prefix) : 0;
    size_t slen = substr ? strlen(substr) : 0;

    for (size_t b = 0; b < s->nbuckets && !truncated; b++) {
        for (entry_t *e = s->buckets[b]; e && !truncated; e = e->next) {
            if (expired(e))
                continue;
            if (cnt >= MAX_QUERY) {
                truncated = 1;
                break;
            }
            if (mode == Q_LIST) {
                if (prefix && (e->klen < plen ||
                               memcmp(e->key, prefix, plen) != 0))
                    continue;
                push_kv(&arr, &cnt, &cap, e->key, e->klen, NULL, 0, 0, e->ts,
                        0);
            } else if (mode == Q_NOTES) {
                if (e->klen < 5 || memcmp(e->key, "note:", 5) != 0)
                    continue;
                char *v = read_val(s, e);
                if (!v)
                    continue;
                if (substr && !stristr(v, e->vsize, substr, slen)) {
                    free(v);
                    continue;
                }
                push_kv(&arr, &cnt, &cap, e->key, e->klen, v, e->vsize, 1,
                        e->ts, 0);
                free(v);
            } else { /* Q_SEARCH */
                int ks = key_score(e->key, e->klen, substr, slen);
                char *v = read_val(s, e);
                if (!v)
                    continue;
                int vm = stristr(v, e->vsize, substr, slen);
                if (!ks && !vm) {
                    free(v);
                    continue;
                }
                push_kv(&arr, &cnt, &cap, e->key, e->klen, v, e->vsize, 1,
                        e->ts, ks + (vm ? 30 : 0));
                free(v);
            }
        }
    }
    pthread_mutex_unlock(&s->mu);

    if (mode == Q_SEARCH)
        qsort(arr, cnt, sizeof(*arr), cmp_search);
    else if (mode == Q_NOTES)
        qsort(arr, cnt, sizeof(*arr), cmp_notes_desc);
    else
        qsort(arr, cnt, sizeof(*arr), desc ? cmp_list_desc : cmp_list_asc);

    *out = arr;
    *n_out = cnt;
    return truncated;
}

void kv_free(kv_t *kvs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(kvs[i].key);
        free(kvs[i].val);
    }
    free(kvs);
}

size_t store_count(store_t *s)
{
    pthread_mutex_lock(&s->mu);
    size_t n = s->count;
    pthread_mutex_unlock(&s->mu);
    return n;
}

void store_stats(store_t *s, uint64_t *entries, uint64_t *log_bytes,
                 uint64_t *dead_bytes, uint64_t *reads, uint64_t *writes,
                 uint64_t *deletes, uint64_t *misses, int64_t *opened_at)
{
    pthread_mutex_lock(&s->mu);
    *entries = s->count;
    *log_bytes = s->size;
    *dead_bytes = s->dead_bytes;
    *reads = s->n_reads;
    *writes = s->n_writes;
    *deletes = s->n_deletes;
    *misses = s->n_misses;
    *opened_at = s->opened_at;
    pthread_mutex_unlock(&s->mu);
}

int store_snapshot(store_t *s, snap_fn_t fn, void *ctx)
{
    pthread_mutex_lock(&s->mu);
    int rc = 0;
    for (size_t b = 0; b < s->nbuckets; b++) {
        for (entry_t *e = s->buckets[b]; e; e = e->next) {
            if (expired(e))
                continue;
            char *v = read_val(s, e);
            if (!v)
                continue;
            if (fn(ctx, e->key, e->klen, v, e->vsize) != 0) {
                free(v);
                rc = -1;
                break;
            }
            free(v);
        }
        if (rc != 0)
            break;
    }
    pthread_mutex_unlock(&s->mu);
    return rc;
}

/*
 * Restore: write every record into a fresh temp log, fsync, rename over the
 * live log, then drop the in-memory index and rebuild it from the new file.
 * The store mutex is held for the whole operation, so concurrent readers and
 * writers see either the old or the new state, never a mix.
 */
int store_restore(store_t *s, const kv_t *kvs, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (kvs[i].klen == 0 || kvs[i].klen > MAX_KEY ||
            kvs[i].vlen > MAX_VAL)
            return -1;
    pthread_mutex_lock(&s->mu);

    size_t plen = strlen(s->path);
    char *tmp = xmalloc(plen + 5);
    memcpy(tmp, s->path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    int tfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) {
        fprintf(stderr, "exomind: restore open failed: %s\n", strerror(errno));
        free(tmp);
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    uint64_t pos = 0;
    int64_t ts = now_ms();
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++)
        rc = write_rec_at(s, tfd, &pos, kvs[i].key, (uint32_t)kvs[i].klen,
                          kvs[i].val, (uint32_t)kvs[i].vlen, ts, 0, 0);
    if (rc == 0 && fdatasync(tfd) != 0) {
        fprintf(stderr, "exomind: restore sync failed: %s\n", strerror(errno));
        rc = -1;
    }
    close(tfd);
    if (rc != 0) {
        unlink(tmp);
        free(tmp);
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    if (rename(tmp, s->path) != 0) {
        fprintf(stderr, "exomind: restore rename failed: %s\n",
                strerror(errno));
        unlink(tmp);
        free(tmp);
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    free(tmp);
    close(s->fd);
    s->fd = open(s->path, O_RDWR);
    if (s->fd < 0) {
        fprintf(stderr, "exomind: reopen after restore failed: %s\n",
                strerror(errno));
        exit(1);
    }
    for (size_t b = 0; b < s->nbuckets; b++) {
        entry_t *e = s->buckets[b];
        while (e) {
            entry_t *next = e->next;
            free(e->key);
            free(e);
            e = next;
        }
        s->buckets[b] = NULL;
    }
    s->count = 0;
    s->dead_bytes = 0;
    s->size = 0;
    store_load(s);
    s->n_writes += n;
    pthread_mutex_unlock(&s->mu);
    return (int)n;
}
