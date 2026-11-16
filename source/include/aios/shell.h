/* aios/shell.h — M5: aiosh, the AI-native shell.
 *
 * The command front-end of AI-OS. One input line is a sequence of
 * pipelines; each pipeline runs commands left to right, the previous
 * stage's output (text + JSON) flowing into the next. A command is
 * resolved in this order:
 *
 *   1. builtin    — help/version/set/vars/count/grep/jobs/spawn/...
 *   2. tool       — anything in the M4 tool registry (calc, fs.*, ...)
 *   3. intent     — natural language: the router (scripted heuristics or
 *                   the llm cortex) picks ONE tool, an agent goal, or a
 *                   direct reply.
 *
 * The shell is also an agent supervisor: 'spawn' forks a background
 * agent job (own memory/tools/cortex, result file in <store>/jobs/),
 * 'jobs' / 'wait' / 'kill' supervise it. Every line publishes sh.cmd
 * on the shell's semantic bus.
 */
#ifndef AIOS_SHELL_H
#define AIOS_SHELL_H

#include "aios/types.h"
#include "aios/json.h"
#include "aios/mem.h"
#include "aios/tools.h"
#include "aios/sembus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh_shell sh_shell;

/* Result of one input line. Free with sh_result_free(). */
typedef struct {
    int       status;   /* 0 ok, 1 failed                        */
    char     *text;     /* display text (owned; "" when none)    */
    json_val *json;     /* structured output (owned; may be NULL)*/
    char     *error;    /* first error message (owned; NULL ok)  */
} sh_result;

void sh_result_free(sh_result *r);

/* Create a shell bound to a workspace jail (sandboxed tools) and an OS
 * store (memory journal, audit log, job results). Creates its own
 * semantic bus. Returns NULL on failure. */
sh_shell *sh_create(const char *jail_dir, const char *store_dir);
void      sh_free(sh_shell *sh);

/* Cortex backend for routing + agent commands: "scripted" (default,
 * deterministic) or "llm" (z-ai bridge). */
void        sh_set_cortex(sh_shell *sh, const char *mode);
const char *sh_cortex_mode(sh_shell *sh);

/* Execute one input line (';' sequences and '|' pipelines allowed).
 * Returns 0 = keep running, 1 = exit requested (exit/quit builtin).
 * out must be non-NULL; free with sh_result_free(). */
int  sh_exec(sh_shell *sh, const char *line, sh_result *out);

/* Background agent jobs. sh_poll_jobs reaps finished children; call it
 * before inspecting job state (the shell does this on every sh_exec). */
void   sh_poll_jobs(sh_shell *sh);
size_t sh_jobs_alive(sh_shell *sh);

/* The shell's OS services (borrowed — do not free). */
sembus  *sh_bus(sh_shell *sh);
mem_ctx *sh_mem(sh_shell *sh);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_SHELL_H */
