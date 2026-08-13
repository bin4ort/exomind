#ifndef EXOMIND_HTTP_H
#define EXOMIND_HTTP_H

#include "store.h"

void http_set_token(const char *token);

/* handle one HTTP connection on fd, then leave it open for the caller */
void http_handle_conn(int fd, store_t *s);

#endif
