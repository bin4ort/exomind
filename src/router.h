#ifndef EXOMIND_ROUTER_H
#define EXOMIND_ROUTER_H

#include "../common/exo.h"

/* MCP router toolset for exomind-server: one tool per sibling module,
 * routed through the sibling binary's console-op contract. */
const exo_mcp_tool_t *exo_router_tools(size_t *n);

int exo_router_call(const char *tool, const char *args, char *out,
                    size_t cap);

void exo_router_register(const char *name, const char *version,
                         int (*call)(const char *tool, const char *args,
                                     char *out, size_t cap));

#endif