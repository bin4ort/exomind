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

#endif
