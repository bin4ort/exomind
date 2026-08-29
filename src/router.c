/* exomind-server: the stack-wide MCP router.
 *
 * The console rework gives every exo module the same contract:
 *   exo<mod> /op?query [--body <text>]   (stdin body for POST when not a tty)
 *
 * When exomind is invoked as `exomind-server` it registers one MCP tool per
 * sibling module and routes each tool call to the sibling binary through
 * that contract (fork/exec, stdout captured). The daemons never need to be
 * up: a routed call runs the operation in-process in the sibling binary.
 *
 * Exit codes: 0 ok, 1 operation failed, 2 unknown operation/usage — the
 * sibling's own code, surfaced as MCP isError.
 *
 * MANDATORY WORKFLOW (enforced by code):
 * 1. Query exocontext for agent state BEFORE every tool call
 * 2. Execute the tool
 * 3. Auto-store result/findings in exomind memory AFTER every tool call
 * This eliminates the "attention problem" where agents forget to use tools.
 */
#include "http.h"
#include "../common/exo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

typedef struct {
    const char *tool;
    const char *binary; /* argv[0] of the sibling */
} router_mod_t;

static const router_mod_t MODS[] = {
    {"exosched", "exosched"},
    {"exoflow", "exoflow"},
    {"exoqms", "exoqms"},
    {"exocrawl", "exocrawl"},
    {"exocontext", "exocontext"},
    {"exodoc", "exodoc"},
    {"exokit", "exokit"},
    {"spawn", "exoflow"},  /* multi-agent spawning via exoflow */
};

/* forward declarations */
static int run_binary(const char *binary, const char *path,
                      const char *body, char *out, size_t cap,
                      int *exit_code);
static void router_verify_claims(const char *tool, const char *result);

/* mandatory: query exocontext for agent state before any tool call */
static void router_load_context(const char *agent_id)
{
    if (!agent_id || !agent_id[0])
        return;
    char out[65536];
    int rc = 0;
    run_binary("exocontext", "/context", agent_id, out, sizeof out, &rc);
    /* context printed to stdout - we could parse and inject, but the act of
     * calling it hydrates the daemon's working state for this agent */
    (void)rc;
}

/* mandatory: auto-store tool result as memory finding */
static void router_store_result(const char *tool, const char *args,
                                const char *result, int success)
{
    if (!result || !result[0])
        return;
    char key[512];
    snprintf(key, sizeof key, "p:tool:%s:%ld", tool, (long)time(NULL));
    char body[8192];
    snprintf(body, sizeof body,
             "{\"tool\":\"%s\",\"args\":%s,\"result\":%.4000s,\"success\":%d}",
             tool, args && args[0] ? args : "{}", result, success);
    char out[4096];
    int rc = 0;
    run_binary("exomind", "/note", body, out, sizeof out, &rc);
    (void)rc;

    /* MANDATORY: verify external claims in result (URLs, versions, APIs) */
    router_verify_claims(tool, result);
}

/* verify external claims: scan for URLs, version patterns, API endpoints
 * and auto-verify with exogit (GitHub/GitLab) or exocrawl (web search) */
static void router_verify_claims(const char *tool, const char *result)
{
    (void)tool; /* available for tool-specific verification logic */
    if (!result)
        return;
    char out[4096];
    int rc = 0;
    /* Look for GitHub/GitLab repo patterns: owner/repo or github.com/owner/repo */
    const char *p = result;
    while ((p = strstr(p, "github.com/")) != NULL) {
        p += 11;
        char repo[512];
        size_t i = 0;
        while (p[i] && p[i] != '/' && p[i] != ' ' && p[i] != '\n' &&
               p[i] != ')' && p[i] != ']' && p[i] != '"' && i < sizeof repo - 1)
            i++;
        if (i > 0 && strchr(p, '/')) {
            strncpy(repo, p, i);
            repo[i] = 0;
            char verify_url[2048];
            snprintf(verify_url, sizeof verify_url,
                     "/repo?url=https://github.com/%s", repo);
            char vout[8192];
            int vrc = 0;
            run_binary("exogit", verify_url, NULL, vout, sizeof vout, &vrc);
            if (vrc == 0 && vout[0]) {
                char vkey[512];
                snprintf(vkey, sizeof vkey, "p:verify:exogit:%ld", (long)time(NULL));
                char vbody[8192];
                snprintf(vbody, sizeof vbody,
                         "{\"claim\":\"github repo %s\",\"verified\":true,\"detail\":%.4000s}",
                         repo, vout);
                run_binary("exomind", "/note", vbody, out, sizeof out, &rc);
            }
        }
    }
    /* Look for version patterns: vX.Y.Z or X.Y.Z */
    p = result;
    while ((p = strpbrk(p, "v0123456789")) != NULL) {
        /* Simple heuristic: look for semver-like pattern */
        if (isdigit((unsigned char)*p) || (*p == 'v' && isdigit((unsigned char)p[1]))) {
            const char *start = p;
            if (*p == 'v') p++;
            while (p && *p && (isdigit((unsigned char)*p) || *p == '.'))
                p++;
            size_t len = (size_t)(p - start);
            if (len >= 3 && len < 32) {
                char ver[64];
                strncpy(ver, start, len);
                ver[len] = 0;
                /* Verify via exodoc or exogit if it's a known component */
                char vurl[1024];
                snprintf(vurl, sizeof vurl, "/search?q=%s+version", ver);
                char vout[8192];
                int vrc = 0;
                run_binary("exocrawl", vurl, NULL, vout, sizeof vout, &vrc);
                if (vrc == 0 && vout[0] && strstr(vout, ver)) {
                    char vkey[512];
                    snprintf(vkey, sizeof vkey, "p:verify:version:%ld", (long)time(NULL));
                    char vbody[8192];
                    snprintf(vbody, sizeof vbody,
                             "{\"claim\":\"version %s\",\"verified\":true,\"detail\":%.4000s}",
                             ver, vout);
                    run_binary("exomind", "/note", vbody, out, sizeof out, &rc);
                }
                break; /* only check first version-like pattern per result */
            }
        }
        p++;
    }
}

/* run `<binary> <path> [--body <body>]` and capture stdout */
static int run_binary(const char *binary, const char *path,
                      const char *body, char *out, size_t cap,
                      int *exit_code)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (body && body[0])
            execlp(binary, binary, path, "--body", body, (char *)NULL);
        else
            execlp(binary, binary, path, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t n = 0;
    ssize_t got;
    while (n + 1 < cap &&
           (got = read(fds[0], out + n, cap - 1 - n)) > 0)
        n += (size_t)got;
    out[n] = 0;
    close(fds[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (WIFEXITED(st))
        *exit_code = WEXITSTATUS(st);
    else
        *exit_code = 1;
    return 0;
}

int exo_router_call(const char *tool, const char *args, char *out,
                    size_t cap)
{
    /* Special handling for spawn tool: create a new exoflow sub-agent */
    if (!strcmp(tool, "spawn")) {
        char path[4096] = "";
        char body[4096] = "";
        if (!exo_json_str(args, "path", path, sizeof path) || !path[0])
            return snprintf(out, cap, "error: missing path (e.g. \"/spawn?task=...\")"),
                   1;
        exo_json_str(args, "body", body, sizeof body);
        char agent_id[256] = "";
        exo_json_str(args, "agent", agent_id, sizeof agent_id);
        router_load_context(agent_id[0] ? agent_id : "opencode");
        
        /* Build exoflow /flow payload from spawn args */
        char flow_body[8192];
        snprintf(flow_body, sizeof flow_body,
                 "{\"name\":\"spawn-%ld\",\"task\":%.4000s,\"tools\":%.1000s}",
                 (long)time(NULL),
                 body[0] ? body : "{}",
                 "");
        int rc = 0;
        if (run_binary("exoflow", "/flow", flow_body, out, cap, &rc) < 0)
            return snprintf(out, cap, "error: cannot run exoflow"), 1;
        router_store_result(tool, args, out, rc == 0);
        return rc == 0 ? 0 : 1;
    }

    for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++) {
        if (strcmp(tool, MODS[i].tool))
            continue;
        char path[4096] = "";
        char body[4096] = "";
        char agent_id[256] = "";
        if (!exo_json_str(args, "path", path, sizeof path) || !path[0])
            return snprintf(out, cap, "error: missing path (e.g. \"/list\")"),
                   1;
        exo_json_str(args, "body", body, sizeof body);
        exo_json_str(args, "agent", agent_id, sizeof agent_id);
        /* MANDATORY: load agent context before tool execution */
        router_load_context(agent_id[0] ? agent_id : "opencode");
        int rc = 0;
        if (run_binary(MODS[i].binary, path, body, out, cap, &rc) < 0)
            return snprintf(out, cap,
                            "error: cannot run %s (is it installed?)",
                            MODS[i].binary), 1;
        /* MANDATORY: auto-store result as memory finding */
        router_store_result(tool, args, out, rc == 0);
        return rc == 0 ? 0 : 1;
    }
    return snprintf(out, cap, "error: no such module tool %s", tool), 1;
}

static const exo_mcp_tool_t ROUTER_TOOLS[] = {
    {"exosched", "Scheduled reminders and one-shot deliveries. path examples: /remind?key=k&when=<RFC3339>&text=t, /timers, /timer?key=k, /ping.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"exoflow", "Swarm session orchestrator. path examples: /flow?name=n..., /flows, /loops, /next, /step, /ping.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"exoqms", "Quality management: audit runs, objectives, issues. path examples: /audit?criteria=metrics, /objectives?title=t&metric=m&target=n, /issues, /nc?title=t&severity=s&reason=r, /ping.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"exocrawl", "Web research with durable topic state. path examples: /search?q=..., /fetch?url=..., /scrape (POST body = URL list), /stats, /ping.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"exocontext", "Context continuity: sessions and auto-compression. path examples: /context?agent=..., /ping.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"exodoc", "Documentation auditor against docs/stack.tsv (batch module). path examples: /audit?stack=docs/stack.tsv&live=1&exomind=<url>&out=<file>.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"exokit", "Behavioral development kit over AI apps (batch module). path examples: /init?dir=..., /extract?src=...&out=..., /verify?kit=...&runner=..., /diff?a=...&b=..., /audit?kit=...", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"spawn", "Multi-agent spawning: spawn sub-agent for complex subtask. path examples: /spawn?task=<description>&agent=<id>&tools=exogit,exocrawl. Returns sub-agent ID and context.", "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
};

/* register the router toolset with exo_mcp_register (appends to the
 * global tool list; the caller must pass the shared dispatcher because
 * exo_mcp_register overwrites g_call) */
void exo_router_register(const char *name, const char *version,
                         int (*call)(const char *tool, const char *args,
                                     char *out, size_t cap))
{
    exo_mcp_register(ROUTER_TOOLS, sizeof ROUTER_TOOLS /
                                       sizeof ROUTER_TOOLS[0],
                     name, version, call);
}
