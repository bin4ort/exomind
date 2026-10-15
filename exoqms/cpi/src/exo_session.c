/* Session context management */

#include "exoqms.h"
#include "cpi.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Session context for auto-prefixing */
typedef struct {
    char session_id[64];
    char agent_id[64];
    int auto_scope;
} session_ctx_t;

static session_ctx_t g_session_ctx = {0};

/* Session context management */
int exo_session_ctx_set(struct exo *exo, const char *session_id, const char *agent_id, char *out, size_t cap)
{
    if (!session_id || !session_id[0] || !agent_id || !agent_id[0])
        return snprintf(out, cap, "error: missing session_id or agent_id"), 1;

    /* Verify agent is registered */
    if (!exo_agent_check(exo, session_id, agent_id))
        return snprintf(out, cap, "error: agent not registered in session"), 1;

    strncpy(g_session_ctx.session_id, session_id, sizeof g_session_ctx.session_id);
    strncpy(g_session_ctx.agent_id, agent_id, sizeof g_session_ctx.agent_id);
    g_session_ctx.auto_scope = 1;

    return snprintf(out, cap, "ok session scoped to %s/%s", session_id, agent_id), 0;
}

/* Clear session context */
int exo_session_ctx_clear(char *out, size_t cap)
{
    memset(&g_session_ctx, 0, sizeof g_session_ctx);
    return snprintf(out, cap, "ok session cleared"), 0;
}

/* Get current session context */
int exo_session_ctx_get(char *out, size_t cap)
{
    if (!g_session_ctx.auto_scope)
        return snprintf(out, cap, "ok (no session)"), 0;
    return snprintf(out, cap, "ok session=%s agent=%s", g_session_ctx.session_id, g_session_ctx.agent_id), 0;
}

/* Enable/disable auto-scoping */
int exo_session_ctx_autoscope(int enable)
{
    g_session_ctx.auto_scope = enable ? 1 : 0;
    return 0;
}

/* Check if auto-scoping is enabled */
int exo_session_ctx_autoscope_get()
{
    return g_session_ctx.auto_scope;
}

/* Build scoped key */
static int build_scoped_key(const char *key, char *scoped, size_t cap)
{
    if (!g_session_ctx.auto_scope) {
        strncpy(scoped, key, cap);
        return 0;
    }
    if (!key || !key[0])
        return -1;
    /* Don't double-prefix if already scoped */
    if (strncmp(key, "session:", 8) == 0)
        return strncpy(scoped, key, cap), 0;
    return snprintf(scoped, cap, "session:%s:agent:%s:%s", 
                   g_session_ctx.session_id, g_session_ctx.agent_id, key);
}

/* Session-scoped set */
int exo_session_set(struct exo *exo, const char *key, const char *value, char *out, size_t cap)
{
    char scoped[512];
    char err[256];
    if (build_scoped_key(key, scoped, sizeof scoped) != 0)
        return snprintf(out, cap, "error: no session context"), 1;

    return exo_persist(exo, scoped, value, err, sizeof err);
}

/* Session-scoped get */
int exo_session_get(struct exo *exo, const char *key, char *out, size_t cap)
{
    char scoped[512];
    if (build_scoped_key(key, scoped, sizeof scoped) != 0)
        return snprintf(out, cap, "error: no session context"), 1;

    char *val = NULL;
    char err[256];
    if (exo_get(exo, scoped, &val, err, sizeof err) != 0 || !val)
        return snprintf(out, cap, "error: not found"), 1;

    snprintf(out, cap, "%s", val);
    free(val);
    return 0;
}

/* Session-scoped list */
int exo_session_list(struct exo *exo, const char *prefix, char *out, size_t cap)
{
    char scoped_prefix[512];
    if (build_scoped_key(prefix, scoped_prefix, sizeof scoped_prefix) != 0)
        return snprintf(out, cap, "error: no session context"), 1;

    char **keys = NULL;
    size_t nk = 0;
    char err[256];
    if (exo_list(exo, prefix, &keys, &nk, err, sizeof err) != 0)
        return snprintf(out, cap, "error: list failed"), 1;

    size_t off = 0;
    for (size_t i = 0; i < nk && off < cap - 1; i++) {
        off += snprintf(out + off, cap - off, "%s\n", keys[i]);
        free(keys[i]);
    }
    free(keys);
    return 0;
}

/* Session-scoped search */
int exo_session_search(struct exo *exo, const char *query, char *out, size_t cap)
{
    /* Need exo connection */
    return snprintf(out, cap, "error: not implemented"), 1;
}

/* Session-scoped note */
int exo_session_note(struct exo *exo, const char *text, char *out, size_t cap)
{
    /* Add session/agent context to note */
    char scoped_text[4096];
    if (g_session_ctx.auto_scope) {
        snprintf(scoped_text, sizeof scoped_text, "[session:%s agent:%s] %s",
                g_session_ctx.session_id, g_session_ctx.agent_id, text);
    } else {
        strncpy(scoped_text, text, sizeof scoped_text);
    }
    char err[256];
    return exo_note(NULL, scoped_text, err, sizeof err);
}

/* Enable/disable auto-scoping */
int exo_session_autoscope(int enable)
{
    g_session_ctx.auto_scope = enable ? 1 : 0;
    return 0;
}

/* Check if auto-scoping is enabled */
int exo_session_autoscope_get()
{
    return g_session_ctx.auto_scope;
}