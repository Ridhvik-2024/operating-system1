/* aios/agent.h — M4: agent runtime.
 * The agentic loop: observe -> plan (cortex) -> act (tools) -> reflect
 * (memory writeback). The cortex is pluggable:
 *   cortex_llm_create()      — real model via the `z-ai chat` CLI bridge
 *   cortex_scripted_create() — deterministic offline policy (tests/CI) */
#ifndef AIOS_AGENT_H
#define AIOS_AGENT_H

#include "aios/types.h"
#include "aios/json.h"
#include "aios/mem.h"
#include "aios/tools.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_action {
    char    *thought;    /* cortex reasoning (short)            */
    char    *tool;       /* tool to call, NULL when done        */
    json_val *args;      /* tool args (owned), NULL when done   */
    char    *final;      /* final answer when done              */
    char    *facts_json; /* optional JSON array of durable facts*/
    bool     done;
} agent_action;

void agent_action_free(agent_action *a);

typedef struct cortex cortex;
struct cortex {
    const char *name;
    /* Decide the next action from an observation JSON:
     * {"step":n,"goal":...,"tools":[...],"memories":[...],
     *  "last":{"tool":...,"ok":...,"error":...,"result":...}|null } */
    aios_status (*plan)(cortex *cx, const char *obs_json,
                        agent_action *out, char *err, size_t ecap);
    void  *ud;
    void (*destroy)(cortex *cx);
};

cortex *cortex_scripted_create(const char *goal);
cortex *cortex_llm_create(const char *system_extra);
void    cortex_free(cortex *cx);

/* The guardian: called before EVERY tool the loop is about to run.
 *
 * This exists because the agent loop used to call tool_exec() directly, which
 * made it the one place in the system where an action ran with no descriptor,
 * no policy verdict and no staging. Everything the shell did to become
 * mediated was bypassed by the thing most likely to act unsupervised.
 *
 * Implementations return 0 to permit, 1 to refuse (with `err` set), and may
 * redirect the write target by calling tool_reg_set_staging(). A NULL
 * `guard` means "no mediation" and is only legitimate in tests. */
typedef struct {
    int (*check)(void *ud, const char *tool, json_val *args,
                 char *err, size_t ecap);
    /* Called AFTER the tool ran, with the same tool name. This is where the
     * guardian compares what the worker says it did against what was declared
     * (M4). It may refuse the step, in which case the result is discarded and
     * the loop treats it as a failed step. Running unattended is exactly when
     * the check matters most, since no human is watching the diff. */
    int (*after)(void *ud, const char *tool, char *err, size_t ecap);
    void *ud;
} agent_guard;

typedef struct {
    const char *name;      /* agent identity (audit + memory)     */
    const char *goal;      /* intent to achieve                   */
    cortex     *cx;        /* planner (borrowed)                  */
    tool_reg   *tools;     /* tool registry (borrowed)            */
    mem_ctx    *mem;       /* OS memory (borrowed, optional)      */
    int         max_steps; /* hard budget (e.g. 8)                */
    agent_guard guard;     /* mediation for every step (required) */
} agent_cfg;

/* Run to completion. final_out gets the final answer (or last error).
 * Returns AIOS_OK when the cortex reported done, AIOS_ERR_LIMIT if the
 * step budget was exhausted, AIOS_ERR_* on fatal failures. */
aios_status agent_run(const agent_cfg *cfg, char *final_out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_AGENT_H */
