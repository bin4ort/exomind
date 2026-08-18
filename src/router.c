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
 */
#include "http.h"
#include "../common/exo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
};

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
    for (size_t i = 0; i < sizeof MODS / sizeof MODS[0]; i++) {
        if (strcmp(tool, MODS[i].tool))
            continue;
        char path[4096] = "";
        char body[4096] = "";
        if (!exo_json_str(args, "path", path, sizeof path) || !path[0])
            return snprintf(out, cap, "error: missing path (e.g. \"/list\")"),
                   1;
        exo_json_str(args, "body", body, sizeof body);
        int rc = 0;
        if (run_binary(MODS[i].binary, path, body, out, cap, &rc) < 0)
            return snprintf(out, cap,
                            "error: cannot run %s (is it installed?)",
                            MODS[i].binary), 1;
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