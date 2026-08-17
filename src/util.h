#ifndef EXOMIND_UTIL_H
#define EXOMIND_UTIL_H

#include <stddef.h>
#include <stdint.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

uint32_t crc32_init(void);
void crc32_update(uint32_t *crc, const void *data, size_t len);
uint32_t crc32_final(uint32_t crc);
uint32_t crc32_bytes(const void *data, size_t len);

int64_t now_ms(void);

/* percent-decodes %XX and '+' in place, returns new length */
size_t url_decode(char *s);

/* case-insensitive substring search */
int stristr(const char *hay, size_t hlen, const char *needle, size_t nlen);

/* minimal flat-JSON helpers (no allocation of intermediate trees) */
char *json_field(const char *json, size_t len, const char *field);
int json_array_each(const char *json, size_t len, size_t *pos,
                    const char **elem, size_t *elen);
char **json_arr_strings(const char *elem, size_t elen, size_t *n);

/* escape control chars for single-line output; returns malloc'd string */
char *escape_line(const char *s, size_t n);

/* escape for embedding in a JSON string; returns malloc'd string */
char *json_escape(const char *s, size_t n);

/* ---------------- vectors (exovec) ---------------- */

#define EXO_VEC_DIM 256
#define EXO_VEC_KEY_PREFIX "vec:"
#define EXO_VEC_KEY_PREFIX_LEN 4

/*
 * Deterministic local embedding: lowercase the text, split into words on any
 * non-alphanumeric byte, hash every character 3-gram of each word (FNV-1a,
 * mod 256) into a fixed 256-dim count vector; words shorter than 3 chars are
 * hashed whole. Counts are clamped to 255. idx/val must hold EXO_VEC_DIM
 * bytes each; on return *nnz_out pairs (index, count) are filled ascending
 * by index. Same text always yields the same vector.
 */
void vec_embed(const char *text, size_t len, uint8_t *idx, uint8_t *val,
               uint8_t *nnz_out);

/* compact binary codec for stored vectors: 'V' | dim[2 LE] | nnz[4 LE]
 * | nnz * (idx[1] val[1]). vec_encode returns a malloc'd buffer;
 * vec_parse validates and returns 0, or -1 if the value is malformed. */
int vec_encode(uint8_t nnz, const uint8_t *idx, const uint8_t *val,
               char **out, size_t *outlen);
int vec_parse(const char *v, size_t vlen, uint8_t *idx, uint8_t *val,
              uint8_t *nnz_out);

int64_t now_epoch(void);

#endif
