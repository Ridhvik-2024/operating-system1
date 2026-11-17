/* shroute.h — internal: aiosh natural-language intent router (M5).
 * Turns one shell line the parser could NOT resolve as builtin or tool
 * into a decision: run one tool, start an agent, or reply directly. */
#ifndef AIOS_SHROUTE_H
#define AIOS_SHROUTE_H

#include "aios/action.h"
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

    /* M1: the descriptor this decision proposes. Built here because this is
     * the only code that knows what was asked for; the origin is stamped by
     * the CALLER (a routed line is an AI decision, an explicit command is a
     * human one), so a decision leaves here with origin UNSPECIFIED and the
     * action is invalid until the caller says who is accountable.
     *
     * Ownership: sh_decision_to_action() transfers it out. sh_decision_free()
     * frees whatever is left, so a caller that ignores the descriptor cannot
     * leak it. */
    aios_action *action;
} sh_decision;

/* Transfer the router's descriptor to the caller (NULL if it built none).
 * The returned action has origin UNSPECIFIED: stamp it before use. */
aios_action *sh_decision_to_action(sh_decision *d);

void sh_decision_free(sh_decision *d);

/* mode: "scripted" (deterministic heuristics) or "llm" (z-ai bridge).
 * tools is consulted so llm routing can validate tool names.
 * On success out is filled (caller frees with sh_decision_free). */
aios_status sh_route(const char *mode, const char *line, tool_reg *tools,
                     sh_decision *out, char *err, size_t ecap);

#endif /* AIOS_SHROUTE_H */
