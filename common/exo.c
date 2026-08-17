/* exo-common implementation. See exo.h for the API contract. */
#include "exo.h"

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------- logging ---------------- */

static int g_log_level = EXO_LOG_INFO;

int exo_log_level(void)
{
    return g_log_level;
}

void exo_set_log_level(int lv)
{
    g_log_level = lv;
}

int exo_parse_log_level(const char *s)
{
    if (!s)
        return -1;
    if (!strcmp(s, "error"))
        return EXO_LOG_ERROR;
    if (!strcmp(s, "warn") || !strcmp(s, "warning"))
        return EXO_LOG_WARN;
    if (!strcmp(s, "info"))
        return EXO_LOG_INFO;
    if (!strcmp(s, "debug"))
        return EXO_LOG_DEBUG;
    return -1;
}

void exo_log(int lv, const char *fmt, ...)
{
    if (lv > g_log_level)
        return;
    static const char *tag[] = {"error", "warn", "info", "debug"};
    fprintf(stderr, "[%s] ", tag[lv & 3]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ---------------- keys file ---------------- */

static int file_has_line(const char *path, const char *name, size_t nlen)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        size_t l = strcspn(line, "\n:");
        if (l == nlen && strncmp(line, name, nlen) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int exo_keys_add(const char *file, const char *entry, char *err, size_t esz)
{
    if (!file || !file[0]) {
        snprintf(err, esz, "no keys file (use --keys <file>)");
        return -1;
    }
    size_t nlen = strcspn(entry, ":");
    if (nlen == 0) {
        snprintf(err, esz, "empty key name");
        return -1;
    }
    if (file_has_line(file, entry, nlen)) {
        snprintf(err, esz, "key '%s' already present", entry);
        return -1;
    }
    FILE *f = fopen(file, "a");
    if (!f) {
        snprintf(err, esz, "cannot open %s: %s", file, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n", entry);
    fclose(f);
    (void)err;
    (void)esz;
    return 0;
}

int exo_keys_remove(const char *file, const char *name, char *err,
                    size_t esz)
{
    if (!file || !file[0]) {
        snprintf(err, esz, "no keys file (use --keys <file>)");
        return -1;
    }
    size_t nlen = strlen(name);
    FILE *in = fopen(file, "r");
    if (!in) {
        snprintf(err, esz, "cannot open %s: %s", file, strerror(errno));
        return -1;
    }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", file);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        snprintf(err, esz, "cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    char line[4096];
    int removed = 0;
    while (fgets(line, sizeof line, in)) {
        size_t l = strcspn(line, "\n:");
        if (l == nlen && strncmp(line, name, nlen) == 0) {
            removed = 1;
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (removed) {
        if (rename(tmp, file) != 0) {
            snprintf(err, esz, "rename: %s", strerror(errno));
            return -1;
        }
    } else {
        unlink(tmp);
    }
    (void)err;
    (void)esz;
    return removed ? 1 : 0;
}

int exo_keys_list(const char *file, char *err, size_t esz)
{
    if (!file || !file[0]) {
        snprintf(err, esz, "no keys file (use --keys <file>)");
        return -1;
    }
    FILE *f = fopen(file, "r");
    if (!f) {
        snprintf(err, esz, "cannot open %s: %s", file, strerror(errno));
        return -1;
    }
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (line[0] == '#' || !line[0])
            continue;
        n++;
        printf("%d %s\n", n, line);
    }
    fclose(f);
    (void)err;
    (void)esz;
    return 0;
}

/* ---------------- rate limiting ---------------- */

static long g_rate_per_sec = 0;
static long g_rate_bucket = 0;
static int64_t g_rate_last_ms = 0;

void exo_rate_init(long per_sec)
{
    g_rate_per_sec = per_sec > 0 ? per_sec : 0;
    g_rate_bucket = g_rate_per_sec;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_rate_last_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int exo_rate_take(void)
{
    if (g_rate_per_sec <= 0)
        return 1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    long elapsed = (long)(now - g_rate_last_ms);
    if (elapsed > 0) {
        g_rate_last_ms = now;
        g_rate_bucket += g_rate_per_sec * elapsed / 1000;
        if (g_rate_bucket > g_rate_per_sec * 2)
            g_rate_bucket = g_rate_per_sec * 2;
    }
    if (g_rate_bucket > 0) {
        g_rate_bucket--;
        return 1;
    }
    return 0;
}

/* ---------------- help registry ---------------- */

/* the whole-stack guide: sibling modules register this plus their own */
static const exo_help_t EXO_SIBLINGS[] = {
    {"exomind",
     "# exomind - external long-term memory for AI agents\n"
     "usage: exomind [--host addr] [--port 7654] [--data file]\n"
     "       [--token t | --keys file] [--rate-limit n]\n"
     "       [--log-level lv] [--mcp]\n"
     "subcommands: keys add|list|remove; --help [modules]\n"},
    {"exosched",
     "# exosched - the alarm clock for AI agents\n"
     "usage: exosched --exomind <url> [--host addr] [--port 7655]\n"
     "       [--token t | --keys file] [--rate-limit n] [--log-level lv]\n"},
    {"exoflow",
     "# exoflow - swarm orchestrator\n"
     "usage: exoflow --exomind <url> [--host addr] [--port 7656]\n"
     "       [--token t | --keys file] [--rate-limit n] [--log-level lv]\n"},
    {"exodoc",
     "# exodoc - documentation auditor (ISO 9001 7.5)\n"
     "usage: exodoc audit --live --stack <manifest>\n"},
    {"exoqms",
     "# exoqms - universal QMS\n"
     "usage: exoqms --port 7657 --exomind <url> [--code path] [--kit path]\n"
     "       [--rate-limit n] [--log-level lv]\n"},
    {"exocrawl",
     "# exocrawl - AI-native research daemon\n"
     "usage: exocrawl [--host addr] [--port 7658] [--proxy url] [--robots]\n"
     "       [--token t | --keys file] [--rate-limit n] [--log-level lv]\n"},
    {"exocontext",
     "# exocontext - context continuity\n"
     "usage: exocontext --exomind <url> [--host addr] [--port 7659]\n"
     "       [--token t | --keys file] [--rate-limit n] [--log-level lv]\n"},
    {"exokit",
     "# exokit - behavioral development kit\n"
     "usage: exokit init|extract|verify|diff|audit\n"},
};

void exo_help_add_siblings(void)
{
    exo_help_add(EXO_SIBLINGS, sizeof EXO_SIBLINGS / sizeof EXO_SIBLINGS[0]);
}

#define MAX_HELP_MODULES 16

static const exo_help_t *g_help[MAX_HELP_MODULES];
static size_t g_nhelp = 0;

void exo_help_add(const exo_help_t *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        /* a module's own full spec replaces its sibling summary */
        for (size_t j = 0; j < g_nhelp; j++)
            if (strcmp(g_help[j]->name, t[i].name) == 0) {
                g_help[j] = &t[i];
                goto next;
            }
        if (g_nhelp < MAX_HELP_MODULES)
            g_help[g_nhelp++] = &t[i];
    next:;
    }
}

void exo_help_print_one(const char *name)
{
    for (size_t i = 0; i < g_nhelp; i++)
        if (strcmp(g_help[i]->name, name) == 0) {
            printf("%s", g_help[i]->spec);
            return;
        }
    printf("## %s\n(no usage guide registered)\n", name);
}

void exo_help_print_all(void)
{
    for (size_t i = 0; i < g_nhelp; i++) {
        printf("\n================================================================\n"
               "## %s\n"
               "================================================================\n",
               g_help[i]->name);
        printf("%s", g_help[i]->spec);
    }
}

/* ---------------- minimal JSON ---------------- */

/* skip whitespace */
static const char *jskip(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* find `key` (a JSON string) in a JSON object, return the value slice */
const char *exo_json_find(const char *doc, const char *key, exo_json_t *out)
{
    const char *p = jskip(doc);
    if (*p != '{')
        return NULL;
    p++;
    size_t kl = strlen(key);
    for (;;) {
        p = jskip(p);
        if (*p != '"')
            return NULL;
        const char *q = p + 1;
        while (*q && *q != '"') {
            if (*q == '\\')
                q++;
            if (*q)
                q++;
        }
        if (*q != '"')
            return NULL;
        if ((size_t)(q - p - 1) == kl && strncmp(p + 1, key, kl) == 0) {
            const char *v = jskip(q + 1);
            if (*v != ':')
                return NULL;
            v = jskip(v + 1);
            out->p = v;
            /* value extent: string (with escapes), nested object/array,
             * or raw token */
            if (*v == '"') {
                const char *e = v + 1;
                while (*e && *e != '"') {
                    if (*e == '\\')
                        e++;
                    if (*e)
                        e++;
                }
                if (*e == '"')
                    out->len = (size_t)(e + 1 - v);
            } else if (*v == '{' || *v == '[') {
                int depth = 0;
                const char *e = v;
                for (; *e; e++) {
                    if (*e == '"') {
                        e++;
                        while (*e && *e != '"') {
                            if (*e == '\\')
                                e++;
                            if (*e)
                                e++;
                        }
                        continue;
                    }
                    if (*e == '{' || *e == '[')
                        depth++;
                    else if (*e == '}' || *e == ']') {
                        depth--;
                        if (depth == 0) {
                            e++;
                            break;
                        }
                    }
                }
                out->len = (size_t)(e - v);
            } else {
                const char *e = v;
                while (*e && *e != ',' && *e != '}' && *e != ']' &&
                       *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r')
                    e++;
                out->len = (size_t)(e - v);
            }
            return v;
        }
        /* skip to the next key: this value ends at the next ',' at depth 0 */
        const char *v = jskip(q + 1);
        if (*v != ':')
            return NULL;
        v = jskip(v + 1);
        int depth = 0;
        const char *e = v;
        for (; *e; e++) {
            if (*e == '"') {
                e++;
                while (*e && *e != '"') {
                    if (*e == '\\')
                        e++;
                    if (*e)
                        e++;
                }
                continue;
            }
            if (*e == '{' || *e == '[')
                depth++;
            else if (*e == '}' || *e == ']') {
                depth--;
                if (depth < 0)
                    return NULL;
            } else if (*e == ',' && depth == 0) {
                p = e + 1;
                break;
            } else if (*e == '}' && depth == 0) {
                return NULL;
            }
            if (!*e)
                return NULL;
        }
        if (!*e)
            return NULL;
    }
}

/* decode a JSON string value slice into out */
static int json_str_decode(const exo_json_t *v, char *out, size_t cap)
{
    if (!v->p || v->p[0] != '"')
        return 0;
    const char *p = v->p + 1;
    const char *end = v->p + v->len;
    size_t n = 0;
    while (p < end && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p + 1 < end) {
            char e = p[1];
            switch (e) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case '"': out[n++] = '"'; break;
            case '\\': out[n++] = '\\'; break;
            case '/': out[n++] = '/'; break;
            default: out[n++] = '\\'; out[n++] = e; break;
            }
            p += 2;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

int exo_json_str(const char *doc, const char *key, char *out, size_t cap)
{
    exo_json_t v;
    if (!exo_json_find(doc, key, &v))
        return 0;
    return json_str_decode(&v, out, cap);
}

/* ---------------- MCP stdio core ---------------- */

#define MCP_MAX_TOOLS 64

static const exo_mcp_tool_t *g_tools[MCP_MAX_TOOLS];
static size_t g_ntools = 0;
static char g_srv_name[128] = "exo";
static char g_srv_ver[64] = "0";
static int (*g_call)(const char *tool, const char *args_json, char *out,
                     size_t cap) = NULL;

void exo_mcp_register(const exo_mcp_tool_t *tools, size_t n,
                      const char *server_name, const char *server_version,
                      int (*call)(const char *tool, const char *args_json,
                                  char *out, size_t cap))
{
    for (size_t i = 0; i < n && g_ntools < MCP_MAX_TOOLS; i++)
        g_tools[g_ntools++] = &tools[i];
    snprintf(g_srv_name, sizeof g_srv_name, "%s", server_name);
    snprintf(g_srv_ver, sizeof g_srv_ver, "%s", server_version);
    g_call = call;
}

/* JSON-escape a string into out (append-only, no escaping of '"' is
 * skipped — this is a full escape) */
static void json_esc_append(char *out, size_t cap, const char *s)
{
    size_t n = strlen(out);
    for (const char *p = s; *p && n + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': out[n++] = '\\'; out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        case '\n': out[n++] = '\\'; out[n++] = 'n'; break;
        case '\r': out[n++] = '\\'; out[n++] = 'r'; break;
        case '\t': out[n++] = '\\'; out[n++] = 't'; break;
        default:
            if (c < 0x20) {
                out[n++] = '\\';
                out[n++] = 'u';
                static const char hex[] = "0123456789abcdef";
                out[n++] = '0';
                out[n++] = '0';
                out[n++] = hex[(c >> 4) & 15];
                out[n++] = hex[c & 15];
            } else {
                out[n++] = (char)c;
            }
        }
    }
    out[n] = 0;
}

static void mcp_reply(const char *id, const char *body)
{
    /* body is the full JSON result/error object (without id) */
    if (id && id[0])
        printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id, body);
    else
        printf("{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":%s}\n", body);
    fflush(stdout);
}

static void mcp_error(const char *id, long code, const char *msg)
{
    if (id && id[0])
        printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%ld,"
               "\"message\":\"%s\"}}\n", id, code, msg);
    else
        printf("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":%ld,"
               "\"message\":\"%s\"}}\n", code, msg);
    fflush(stdout);
}

static void mcp_noresponse(void)
{
    /* notifications: nothing to send */
}

int exo_mcp_stdio(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    while ((got = getline(&line, &cap, stdin)) >= 0) {
        if (got > 0 && line[got - 1] == '\n')
            line[--got] = 0;
        if (got > 0 && line[got - 1] == '\r')
            line[--got] = 0;
        char *doc = line;
        while (*doc == ' ' || *doc == '\t')
            doc++;
        if (!doc[0])
            continue;

        char id[64] = "";
        char idraw[64] = "";
        {
            exo_json_t v;
            if (exo_json_find(doc, "id", &v)) {
                size_t n = v.len < sizeof idraw - 1 ? v.len
                                                    : sizeof idraw - 1;
                memcpy(idraw, v.p, n);
                idraw[n] = 0;
                if (idraw[0] == '"') {
                    exo_json_t sv = v;
                    json_str_decode(&sv, id, sizeof id);
                } else {
                    snprintf(id, sizeof id, "%s", idraw);
                }
            }
        }

        char method[128] = "";
        exo_json_str(doc, "method", method, sizeof method);

        if (!method[0]) {
            mcp_error(id, -32600, "missing method");
            continue;
        }
        if (strcmp(method, "initialize") == 0) {
            char body[4096];
            snprintf(body, sizeof body,
                     "{\"protocolVersion\":\"2024-11-05\","
                     "\"capabilities\":{\"tools\":{}},"
                     "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
                     g_srv_name, g_srv_ver);
            mcp_reply(id, body);
            continue;
        }
        if (strcmp(method, "notifications/initialized") == 0 ||
            strcmp(method, "notifications/cancelled") == 0) {
            mcp_noresponse();
            continue;
        }
        if (strcmp(method, "ping") == 0) {
            mcp_reply(id, "{}");
            continue;
        }
        if (strcmp(method, "tools/list") == 0) {
            char body[65536];
            size_t n = 0;
            n += (size_t)snprintf(body + n, sizeof body - n,
                                  "{\"tools\":[");
            for (size_t i = 0; i < g_ntools; i++) {
                if (i)
                    n += (size_t)snprintf(body + n, sizeof body - n, ",");
                n += (size_t)snprintf(body + n, sizeof body - n,
                                      "{\"name\":\"%s\",\"description\":",
                                      g_tools[i]->name);
                /* escape description */
                char esc[2048] = "\"";
                json_esc_append(esc, sizeof esc, g_tools[i]->description);
                size_t el = strlen(esc);
                if (el + 1 < sizeof esc) {
                    esc[el] = '"';
                    esc[el + 1] = 0;
                }
                n += (size_t)snprintf(body + n, sizeof body - n, "%s",
                                      esc);
                n += (size_t)snprintf(body + n, sizeof body - n,
                                      ",\"inputSchema\":%s}",
                                      g_tools[i]->input_schema);
            }
            n += (size_t)snprintf(body + n, sizeof body - n, "]}");
            mcp_reply(id, body);
            continue;
        }
        if (strcmp(method, "tools/call") == 0) {
            /* the tool name and arguments live inside params */
            char params[16384] = "{}";
            {
                exo_json_t v;
                if (exo_json_find(doc, "params", &v) && v.p && v.len &&
                    v.p[0] == '{') {
                    size_t n = v.len < sizeof params - 1 ? v.len
                                                         : sizeof params - 1;
                    memcpy(params, v.p, n);
                    params[n] = 0;
                }
            }
            char name[256] = "";
            exo_json_str(params, "name", name, sizeof name);
            if (!name[0]) {
                mcp_error(id, -32602, "missing tool name");
                continue;
            }
            int known = 0;
            for (size_t i = 0; i < g_ntools; i++)
                if (strcmp(g_tools[i]->name, name) == 0) {
                    known = 1;
                    break;
                }
            if (!known) {
                char msg[512];
                snprintf(msg, sizeof msg, "unknown tool: %s", name);
                mcp_error(id, -32602, msg);
                continue;
            }
            char args[8192] = "{}";
            {
                exo_json_t v;
                if (exo_json_find(params, "arguments", &v) && v.p && v.len) {
                    size_t n = v.len < sizeof args - 1 ? v.len
                                                       : sizeof args - 1;
                    memcpy(args, v.p, n);
                    args[n] = 0;
                }
            }
            char result[65536] = "";
            if (!g_call || g_call(name, args, result, sizeof result) != 0) {
                char msg[70000];
                snprintf(msg, sizeof msg,
                         "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
                         "\"isError\":true}",
                         result[0] ? result : "tool call failed");
                mcp_reply(id, msg);
                continue;
            }
            char text[70000] = "\"";
            json_esc_append(text, sizeof text, result);
            size_t tl = strlen(text);
            if (tl + 1 < sizeof text) {
                text[tl] = '"';
                text[tl + 1] = 0;
            }
            char body[70000];
            size_t n = (size_t)snprintf(body, sizeof body,
                     "{\"content\":[{\"type\":\"text\",\"text\":");
            n += (size_t)snprintf(body + n, sizeof body - n, "%s", text);
            snprintf(body + n, sizeof body - n, "],\"isError\":false}");

            mcp_reply(id, body);
            continue;
        }
        {
            char msg[512];
            snprintf(msg, sizeof msg, "method not found: %s", method);
            mcp_error(id, -32601, msg);
        }
    }
    free(line);
    return 0;
}
