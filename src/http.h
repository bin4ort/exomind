#ifndef EXOMIND_HTTP_H
#define EXOMIND_HTTP_H

#include "store.h"

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

void http_set_token(const char *token);

/* load extra tokens from a file; lines: token[:ro][:scope=<prefix>*].
 * Returns the number of tokens loaded, or -1 if the file cannot be read. */
int http_load_tokens(const char *path);

/* handle one HTTP connection on fd, then leave it open for the caller */
void http_handle_conn(int fd, store_t *s);
const char *http_help_text(void);
/* internal dispatch for the MCP bridge (no HTTP auth) */
int http_dispatch(const char *method, const char *path, const char *query,
                  const char *body, size_t body_len, buf_t *out,
                  int *status, const char **ctype, store_t *s);
extern int g_rate_limit_active;
void http_buf_free(buf_t *b);

#endif
