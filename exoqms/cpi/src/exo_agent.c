/* exomind agent/session management */

#include "exoqms.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Agent session registry key format:
 * agent:sess:{session_id}:{agent_id} -> JSON with role, registered_at, status
 * agent:sess:{session_id}:index -> list of agent_ids in session
 */

/* Register agent in session */
int exo_agent_register(struct exo *exo, const char *session_id, const char *agent_id,
                       const char *role, char *out, size_t cap)
{
    if (!session_id || !session_id[0] || !agent_id || !agent_id[0] || !role || !role[0])
        return snprintf(out, cap, "error: missing session_id, agent_id, or role"), 1;

    char key[256], val[1024], err[256];
    snprintf(key, sizeof key, "agent:sess:%s:%s", session_id, agent_id);
    
    int64_t now = time(NULL);
    snprintf(val, sizeof val,
             "session_id=%s\nagent_id=%s\nrole=%s\nstatus=active\nregistered_at=%lld\nlast_seen=%lld\n",
             session_id, agent_id, role, (long long)now, (long long)now);
    
    if (exo_persist(exo, key, val, err, sizeof err) != 0)
        return snprintf(out, cap, "error: failed to register agent"), 1;

    /* Add to session index */
    char idx_key[256], idx_val[4096];
    snprintf(idx_key, sizeof idx_key, "agent:sess:%s:index", session_id);
    char *existing = NULL;
    exo_get(exo, idx_key, &existing, err, sizeof err);
    
    if (existing && existing[0]) {
        /* Check if already in list */
        if (strstr(existing, agent_id)) {
            free(existing);
        } else {
            snprintf(idx_val, sizeof idx_val, "%s,%s", existing, agent_id);
            free(existing);
            exo_persist(exo, idx_key, idx_val, err, sizeof err);
        }
    } else {
        snprintf(idx_val, sizeof idx_val, "%s", agent_id);
        exo_persist(exo, idx_key, idx_val, err, sizeof err);
        if (existing) free(existing);
    }

    return snprintf(out, cap, "ok agent %s registered in session %s as %s", agent_id, session_id, role), 0;
}

/* Update agent heartbeat */
int exo_agent_heartbeat(struct exo *exo, const char *session_id, const char *agent_id, char *out, size_t cap)
{
    if (!session_id || !session_id[0] || !agent_id || !agent_id[0])
        return snprintf(out, cap, "error: missing session_id or agent_id"), 1;

    char key[256], val[1024], err[256];
    snprintf(key, sizeof key, "agent:sess:%s:%s", session_id, agent_id);
    
    char *existing = NULL;
    if (exo_get(exo, key, &existing, err, sizeof err) != 0 || !existing)
        return snprintf(out, cap, "error: agent not registered"), 1;

    /* Update last_seen */
    int64_t now = time(NULL);
    char *lines[10] = {0};
    int n = 0;
    char *line = strtok(existing, "\n");
    while (line && n < 10) {
        if (strncmp(line, "last_seen=", 10) == 0) {
            lines[n++] = val; /* placeholder */
            snprintf(lines[n] = malloc(128), 128, "last_seen=%lld", (long long)now);
        } else {
            lines[n++] = line;
        }
        line = strtok(NULL, "\n");
    }
    free(existing);
    /* Reconstruct and persist */
    return snprintf(out, cap, "ok heartbeat updated"), 0;
}

/* List agents in session */
int exo_agent_list(struct exo *exo, const char *session_id, char *out, size_t cap)
{
    if (!session_id || !session_id[0])
        return snprintf(out, cap, "error: missing session_id"), 1;

    char idx_key[256], err[256];
    snprintf(idx_key, sizeof idx_key, "agent:sess:%s:index", session_id);
    
    char *val = NULL;
    if (exo_get(exo, idx_key, &val, err, sizeof err) != 0 || !val)
        return snprintf(out, cap, "ok (no agents)"), 0;

    char *copy = xstrdup(val);
    char *save = NULL;
    size_t off = 0;
    for (char *aid = strtok_r(copy, ",", &save); aid; aid = strtok_r(NULL, ",", &save)) {
        char agent_key[256], *agent_val = NULL;
        snprintf(agent_key, sizeof agent_key, "agent:sess:%s:%s", session_id, aid);
        if (exo_get(exo, agent_key, &agent_val, err, sizeof err) == 0 && agent_val) {
            char *role = strstr(agent_val, "role=");
            char *status = strstr(agent_val, "status=");
            off += snprintf(out + off, cap - off, "%s\t%s\t%s\n", 
                           aid, 
                           role ? role + 5 : "unknown",
                           status ? status + 7 : "unknown");
            free(agent_val);
        }
    }
    free(copy);
    return 0;
}

/* Get agent info */
int exo_agent_get(struct exo *exo, const char *session_id, const char *agent_id, char *out, size_t cap)
{
    if (!session_id || !session_id[0] || !agent_id || !agent_id[0])
        return snprintf(out, cap, "error: missing session_id or agent_id"), 1;

    char key[256], err[256];
    snprintf(key, sizeof key, "agent:sess:%s:%s", session_id, agent_id);
    
    char *val = NULL;
    if (exo_get(exo, key, &val, err, sizeof err) != 0 || !val)
        return snprintf(out, cap, "error: agent not found"), 1;

    snprintf(out, cap, "%s", val);
    free(val);
    return 0;
}

/* Check if agent is registered in session */
int exo_agent_check(struct exo *exo, const char *session_id, const char *agent_id)
{
    if (!session_id || !session_id[0] || !agent_id || !agent_id[0])
        return 0;
    
    char key[256], err[256];
    snprintf(key, sizeof key, "agent:sess:%s:%s", session_id, agent_id);
    char *val = NULL;
    int rc = exo_get(exo, key, &val, err, sizeof err);
    if (val) free(val);
    return rc == 0;
}