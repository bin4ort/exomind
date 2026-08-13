#ifndef EXOMIND_STORE_H
#define EXOMIND_STORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct store store_t;

enum { Q_LIST = 0, Q_SEARCH = 1, Q_NOTES = 2 };

typedef struct {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    int has_val;
    int64_t ts;
    int64_t score;
} kv_t;

/* open (or create) the data file; exits on fatal error */
store_t *store_open(const char *path);
void store_close(store_t *s);

/*
 * Set key -> val. ttl_sec == 0 means no expiry. If append is nonzero and the
 * key exists, the new value is appended after a single '\n' separator (unless
 * the existing value already ends with a newline). Does NOT fsync; call
 * store_sync() when a durable boundary is needed (interactive ops sync,
 * batch ops share one sync at the end).
 */
int store_set(store_t *s, const char *key, size_t klen,
              const char *val, size_t vlen, int64_t ttl_sec, int append);

int store_sync(store_t *s);

/* returns malloc'd value (NUL-terminated; *vlen is the byte length), or NULL */
char *store_get(store_t *s, const char *key, size_t klen, size_t *vlen,
                int64_t *ts);

/* returns 1 if the key existed, 0 if not, -1 on error */
int store_del(store_t *s, const char *key, size_t klen);

/*
 * Collect matching records into a sorted kv array.
 *   Q_LIST:   keys with optional prefix (sorted by key)
 *   Q_SEARCH: case-insensitive substring match in keys and values,
 *             scored and sorted best-first
 *   Q_NOTES:  keys with prefix "note:", optionally filtered by content,
 *             newest first
 * Returns 1 if the result set was truncated at the safety cap.
 */
int store_query(store_t *s, int mode, const char *prefix, const char *substr,
                int desc, kv_t **out, size_t *n_out);
void kv_free(kv_t *kvs, size_t n);

size_t store_count(store_t *s);
void store_stats(store_t *s, uint64_t *entries, uint64_t *log_bytes,
                 uint64_t *dead_bytes, uint64_t *reads, uint64_t *writes,
                 uint64_t *deletes, uint64_t *misses, int64_t *opened_at);

#endif
