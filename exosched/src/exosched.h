#ifndef EXOSCHED_H
#define EXOSCHED_H

#include <stddef.h>
#include <stdint.h>

#define EXOSCHED_VERSION "0.1.0"
#define EXO_KEY_PREFIX "exosched:timer:"
#define TIMER_ID_MAX 40
#define MAX_MSG (64u * 1024u)
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct timer {
    char id[TIMER_ID_MAX];
    int64_t wall_fire;   /* wall-clock epoch seconds */
    int64_t mono_fire;   /* CLOCK_MONOTONIC nanoseconds */
    char *msg;
    struct timer *next;
} timer_rec_t;

/* ---- util.c ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int64_t now_ms(void);
int64_t now_epoch(void);
int64_t mono_ns(void);
uint32_t rand32(void);
int ci_prefix(const char *line, const char *prefix);
char *esc_line(const char *s, size_t n);
char *unesc_line(const char *s);
char *json_escape(const char *s, size_t n);

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

void buf_put(buf_t *b, const void *d, size_t n);
void buf_puts(buf_t *b, const char *s);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);

/* ---- exomind.c : HTTP client for the exomind storage backend ---- */
typedef struct exo {
    char host[256];
    int port;
} exo_t;

int exo_init(exo_t *e, const char *url, char *err, size_t errsz);
int exo_request(exo_t *e, const char *method, const char *target,
                const char *body, size_t blen, int json_ct,
                char **out, size_t *outlen, int *status,
                char *err, size_t errsz);
int exo_persist(exo_t *e, const char *key, const char *value, long ttl,
                char *err, size_t errsz);
int exo_del(exo_t *e, const char *key, int *existed, char *err, size_t errsz);
int exo_note(exo_t *e, const char *text, char *err, size_t errsz);
int exo_list(exo_t *e, const char *prefix, char ***keys, size_t *n,
             char *err, size_t errsz);
int exo_batch_get(exo_t *e, char **keys, size_t n, char ***vals,
                  char *err, size_t errsz);

/* ---- schedule.c ---- */
int parse_schedule(const char *body, size_t len, int64_t *fire_epoch,
                   char **msg, char *err, size_t errsz);

/* ---- timers.c : single timer thread + registry ---- */
void timers_init(void);
void timers_shutdown(void);
int timer_add(const char *id, int64_t wall_fire, const char *msg);
int timer_cancel(const char *id);
timer_rec_t *timer_find(const char *id);
size_t timer_count(void);
timer_rec_t *timers_snapshot(size_t *n);
void timers_snapshot_free(timer_rec_t *snap, size_t n);
int timers_reload(exo_t *e);
void *timer_loop(void *arg);

/* ---- ws.c : RFC 6455 push channel ---- */
void ws_init(void);
void ws_broadcast(const char *id, int64_t epoch, const char *msg);
void ws_handle_conn(int fd);
int ws_make_accept(const char *key, char *accept, size_t cap);

/* ---- http.c : exosched's own HTTP layer ---- */
void http_handle_conn(int fd, exo_t *e);
void http_set_token(const char *tok);

#endif
