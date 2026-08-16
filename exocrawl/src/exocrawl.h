/*
 * exocrawl: AI-native web research daemon.
 *
 * Token-efficient scraping: HTML is reduced to plain text (headings,
 * paragraphs, links, images) with boilerplate (nav/ads/cookies) removed.
 * Search runs through SearXNG-style private metasearch (no accounts, no
 * cookies, no tracking). TLS is provided by the curl binary; everything
 * else is native C. Zero persistent state unless caching into exomind.
 */
#ifndef EXOCRAWL_H
#define EXOCRAWL_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

#define EXO_VERSION "0.1.0"
#define MAX_URL 4096
#define MAX_BODY (32u * 1024u * 1024u)
#define MAX_EXTRACT (1u << 20) /* 1 MB extraction cap per page */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b, size_t cap);
void buf_free(buf_t *b);
void buf_puts(buf_t *b, const char *s);
void buf_putc(buf_t *b, char c);
void buf_printf(buf_t *b, const char *fmt, ...);

/* URL helpers */
char *url_encode(const char *s);
char *url_host(const char *url, char *out, size_t cap);
char *url_path(const char *url, char *out, size_t cap);
int url_is_http(const char *url);

/* HTML entity decoding into out (escaped chars from source) */
size_t html_entity_decode(const char *src, size_t n, char *out, size_t cap);

/* ---- HTTP layer (curl binary transport) ---- */
typedef struct {
    char ua[256];
    int status;      /* HTTP status or 0 on transport error */
    char *body;      /* malloc'd, NUL-terminated */
    size_t blen;
    int transport_ok;
} resp_t;

typedef struct {
    char **uas;      /* identity rotation list */
    size_t nuas;
    size_t ua_idx;
    const char *proxy;   /* optional HTTP proxy */
    const char *engine_base; /* test override: prefix for engine urls */
    int timeout_ms;
} net_t;

void net_init(net_t *n);
int net_fetch(net_t *n, const char *url, resp_t *r, char *err, size_t errsz);
void resp_free(resp_t *r);

/* ---- HTML -> text extraction ---- */
typedef struct {
    char *title;
    char *text;      /* extracted reading text */
    char **links;    /* "anchor<TAB>url" lines */
    size_t nlinks;
    char **images;   /* "alt<TAB>src" lines */
    size_t nimages;
} page_t;

void page_free(page_t *p);
/* extract: plain-text page (headings as ##, lists as -, code verbatim).
 * max = output cap; returns chars written. links/images appended when
 * the respective flags are set (line-based, tab-separated). */
size_t page_extract(const char *html, size_t hlen, const char *base,
                  page_t *p, size_t max);

/* ---- metasearch ---- */
typedef struct {
    char *title;
    char *url;
    char *snippet;
} result_t;

int search_run(net_t *n, const char *q, const char *engines, int n_results,
               result_t **out, size_t *n_out, char *err, size_t errsz);
void results_free(result_t *r, size_t n);

/* ---- concurrency pool ---- */
typedef struct job_s job_t;
typedef void (*job_fn)(job_t *j);
struct job_s {
    job_fn fn;
    void *arg;
    job_t *next;
};

typedef struct {
    pthread_t *threads;
    size_t nthreads;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    job_t *head, *tail;
    int quit;
    size_t active;
} pool_t;

int pool_init(pool_t *p, size_t n);
void pool_submit(pool_t *p, job_fn fn, void *arg);
void pool_wait(pool_t *p); /* wait until idle */
void pool_destroy(pool_t *p);

/* per-host pacing: min ms between requests to the same host */
struct pace_s {
    char host[256];
    int64_t next_ms;
    struct pace_s *next;
};
typedef struct pace_s pace_t;
typedef struct pace_s *pace_list_t;
void pace_lock(void);
int64_t pace_wait(const char *host, int min_ms);

#endif
