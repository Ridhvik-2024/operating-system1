/* shroute.h — internal: aiosh natural-language intent router (M5).
 * Turns one shell line the parser could NOT resolve as builtin or tool
 * into a decision: run one tool, start an agent, or reply directly. */
#ifndef AIOS_SHROUTE_H
#define AIOS_SHROUTE_H

#include "aios/tools.h"

typedef enum {
    SH_ROUTE_TOOL  = 0,   /* run exactly one tool from the registry   */
    SH_ROUTE_AGENT = 1,   /* hand the goal to the M4 agent runtime    */
    SH_ROUTE_REPLY = 2    /* shell answers directly (no tools needed) */
} sh_route_kind;

typedef struct {
    sh_route_kind kind;
    char     *tool;       /* SH_ROUTE_TOOL: tool name                */
    json_val *args;       /* SH_ROUTE_TOOL: args object (owned)      */
    char     *goal;       /* SH_ROUTE_AGENT: goal text               */
    char     *text;       /* SH_ROUTE_REPLY: reply text              */
    char     *thought;    /* router reasoning (may be NULL)          */
    int      bg;          /* SH_ROUTE_AGENT: M7 scheduling class — 1 =
                             background/idle-priority: the shell spawns
                             a background job (IDLE class) instead of
                             running the agent in the foreground     */
    int      urgent;      /* SH_ROUTE_AGENT: urgent/interactive wording;
                             stays foreground, noted for the journal */
} sh_decision;

void sh_decision_free(sh_decision *d);

/* mode: "scripted" (deterministic heuristics) or "llm" (z-ai bridge).
 * tools is consulted so llm routing can validate tool names.
 * On success out is filled (caller frees with sh_decision_free). */
aios_status sh_route(const char *mode, const char *line, tool_reg *tools,
                     sh_decision *out, char *err, size_t ecap);

#endif /* AIOS_SHROUTE_H */
