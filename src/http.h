#ifndef EXOMIND_HTTP_H
#define EXOMIND_HTTP_H

#include "store.h"

void http_set_token(const char *token);

/* load extra tokens from a file; lines: token[:ro][:scope=<prefix>*].
 * Returns the number of tokens loaded, or -1 if the file cannot be read. */
int http_load_tokens(const char *path);

/* handle one HTTP connection on fd, then leave it open for the caller */
void http_handle_conn(int fd, store_t *s);

#endif
