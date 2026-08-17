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
/* the on-disk data file path of the store (for backups) */
const char *store_path(const store_t *s);
void store_stats(store_t *s, uint64_t *entries, uint64_t *log_bytes,
                 uint64_t *dead_bytes, uint64_t *reads, uint64_t *writes,
                 uint64_t *deletes, uint64_t *misses, int64_t *opened_at);

/* callback invoked per live record (no tombstones, no expired) */
typedef int (*snap_fn_t)(void *ctx, const char *key, size_t klen,
                         const char *val, size_t vlen);

/* iterate all live records in hash order; returns 0, or -1 if the callback
 * aborted. No fsync; the caller decides how the stream is consumed. */
int store_snapshot(store_t *s, snap_fn_t fn, void *ctx);

/*
 * Atomically replace the entire store with the given records: the new log is
 * written to a temp file, fsynced, and renamed over the live log before the
 * in-memory index is rebuilt. On any failure the old store stays intact.
 * Returns the number of records stored, or -1 on error.
 */
int store_restore(store_t *s, const kv_t *kvs, size_t n);

/*
 * Vector index (exovec): every live `vec:` key with a well-formed vector is
 * mirrored in an in-memory index, rebuilt at load and kept in sync with
 * writes, so similarity scans never touch the log. qidx/qval/qnnz form the
 * sparse query vector (as produced by vec_embed). Returns up to topk keys
 * with positive cosine similarity, best first (ties by key), as kv_t rows
 * whose score is cosine * 1e6. Expired vectors are skipped.
 */
int store_vec_sim(store_t *s, const uint8_t *qidx, const uint8_t *qval,
                  uint8_t qnnz, int topk, kv_t **out, size_t *n_out);

#endif
