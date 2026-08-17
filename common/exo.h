/*
 * exo-common — the shared console/server layer for every module.
 *
 * Provides the uniform CLI surface:
 *   --help [modules]  per-module or whole-stack usage guide
 *   --log-level <error|warn|info|debug>
 *   --keys <file>     key file (with `keys add|list|remove` subcommands)
 *   --rate-limit <n>  requests/second (429 when exhausted)
 *   --mcp             stdio Model Context Protocol server (a binary
 *                     installed as <module>-server forces this mode)
 * and the MCP JSON-RPC core used by every <module>-server.
 *
 * Zero dependencies, C11. Compiled into each module binary.
 */
#ifndef EXO_COMMON_H
#define EXO_COMMON_H

#include <stddef.h>
#include <stdint.h>

enum { EXO_LOG_ERROR = 0, EXO_LOG_WARN, EXO_LOG_INFO, EXO_LOG_DEBUG };

void exo_set_log_level(int lv);
int exo_log_level(void);
int exo_parse_log_level(const char *s);
void exo_log(int lv, const char *fmt, ...);

/* ---- keys file management (private keys / auth tokens) ---- */
int exo_keys_add(const char *file, const char *entry, char *err, size_t esz);
int exo_keys_remove(const char *file, const char *name, char *err,
                    size_t esz);
int exo_keys_list(const char *file, char *err, size_t esz);

/* ---- rate limiting (token bucket, per daemon) ---- */
void exo_rate_init(long per_sec);
int exo_rate_take(void);

/* ---- help registry: every module registers itself + siblings ---- */
typedef struct {
    const char *name;   /* module name, e.g. "exomind" */
    const char *spec;   /* the module's usage guide (its GET / text) */
} exo_help_t;

void exo_help_add(const exo_help_t *t, size_t n);
void exo_help_add_siblings(void);
void exo_help_print_one(const char *name);
void exo_help_print_all(void);

/* ---- minimal JSON helpers (for the MCP layer) ---- */
typedef struct {
    const char *p;
    size_t len;
} exo_json_t;

/* locate `key` in a flat JSON object; out->p/len = raw value slice
 * (including quotes for strings). Returns the value start or NULL. */
const char *exo_json_find(const char *doc, const char *key, exo_json_t *out);
/* decoded string value of `key` in `doc` (handles \" \\n etc), 0 on miss */
int exo_json_str(const char *doc, const char *key, char *out, size_t cap);

/* ---- MCP (Model Context Protocol) stdio server core ---- */
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema; /* JSON object string */
} exo_mcp_tool_t;

void exo_mcp_register(const exo_mcp_tool_t *tools, size_t n,
                      const char *server_name, const char *server_version,
                      int (*call)(const char *tool, const char *args_json,
                                  char *out, size_t cap));
/* serve MCP over stdin/stdout until EOF; returns 0 */
int exo_mcp_stdio(void);

#endif
