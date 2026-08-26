/*
 * exogit: AI-native code-research daemon for GitHub / GitLab.
 *
 * Purpose (from deployment feedback): let an agent research how similar
 * software is built by other people, efficiently. The agent writes far
 * better code when it can read real implementations — and, crucially, the
 * evolution of that code: the commits that took a feature from "introduced"
 * to "working". exogit extracts exactly that, token-efficiently:
 *
 *   - /search  : find code on GitHub/GitLab (narrow the prompt)
 *   - /file    : fetch one source file (raw), line-numbered, capped
 *   - /repo    : list a repository's tree
 *   - /commits : the commit history of ONE file, with the diffs stripped
 *                down to only the changed lines (context dropped) so the
 *                agent receives the design decisions, not 2000 unchanged
 *                lines around a 7-line change.
 *
 * Plain text, lowercase ok/error replies, one op per console invocation.
 * TLS via the curl binary. No accounts, no cookies, no JS.
 */
#ifndef EXOGIT_H
#define EXOGIT_H

#define EXO_VERSION "0.1.0"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable text buffer ---- */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b, size_t cap);
void buf_free(buf_t *b);
void buf_puts(buf_t *b, const char *s);
void buf_putsn(buf_t *b, const char *s, size_t n);
void buf_printf(buf_t *b, const char *fmt, ...);

/* ---- HTTP transport via the curl binary ---- */
#define MAX_URL 8192
#define MAX_BODY (64u * 1024u * 1024u)

typedef struct {
    char **uas;
    size_t nuas;
    size_t ua_idx;
    int timeout_ms;
    const char *proxy;
    const char *token; /* optional bearer for API auth */
} net_t;

typedef struct {
    char *body;
    size_t blen;
    int status;
    int transport_ok;
} resp_t;

void net_init(net_t *n);
void net_free(net_t *n);
void resp_free(resp_t *r);
/* fetch with UA rotation + retry on 403/429/5xx. token added as
 * Authorization: Bearer when set. Returns 0 ok, -1 transport error. */
int net_fetch(net_t *n, const char *url, resp_t *r, char *err, size_t errsz);

/* ---- minimal JSON helpers (array/object iteration, string extraction) ---- */
/* Extract a top-level string value for `key` into out (decoded). Returns 1
 * if found. Handles the flat "key":"value" and "key": "value" forms and
 * basic escapes. */
int json_str(const char *doc, const char *key, char *out, size_t cap);

/* Brace-matched slice [start,end) of the i-th top-level object inside a
 * JSON array. Returns 1 and sets start/end on success, 0 if none. */
int json_array_object(const char *doc, size_t *cursor, size_t *start,
                      size_t *end);

/* Within [start,end), find the slice of a nested object value for `key`
 * (e.g. "commit" inside a commit object), returns 1 + sets s/e, else 0. */
int json_find_slice(const char *doc, size_t start, size_t end,
                    const char *key, size_t *s, size_t *e);

/* Extract a string value located within [start,end) for `key`. */
int json_str_in(const char *doc, size_t start, size_t end, const char *key,
                char *out, size_t cap);

/* ---- HTML -> text for search-result scraping ---- */
/* Strip tags, decode common entities, collapse whitespace. Writes into out
 * (cap). Returns written length. */
size_t html_to_text(const char *html, size_t hlen, char *out, size_t cap);

/* ---- operations (fill dout body) ---- */
typedef struct {
    buf_t body;
    int status;
    const char *ctype;
} dout_t;

void dout_init(dout_t *d, size_t cap);
void dout_free(dout_t *d);

void exogit_search(dout_t *d, net_t *net, const char *query);
void exogit_file(dout_t *d, net_t *net, const char *query);
void exogit_repo(dout_t *d, net_t *net, const char *query);
void exogit_commits(dout_t *d, net_t *net, const char *query);

/* shared spec text (GET / and the no-arg guide) */
const char *exogit_spec(void);

/* parse owner/repo (+branch) and file path out of a GitHub URL or
 * "owner/repo" shorthand; also handles gitlab.com. Returns 1 on success. */
int parse_repo_ref(const char *raw, char *host, size_t hostcap, char *owner,
                   size_t ownercap, char *repo, size_t repocap, char *branch,
                   size_t branchcap, char *path, size_t pathcap,
                   int *is_gitlab);

#endif
