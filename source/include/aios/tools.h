/* aios/tools.h — M4: tool registry.
 * Two classes of tools:
 *   NATIVE    — executed in-process (memory.search, proc.info).
 *   SANDBOXED — executed by forking the `tool-exec` worker process
 *               (calc, fs.read, fs.write, fs.list, selftest.*).
 * Every execution is journaled to <store_dir>/audit.log (JSONL). */
#ifndef AIOS_TOOLS_H
#define AIOS_TOOLS_H

#include "aios/types.h"
#include "aios/json.h"
#include "aios/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tool_def {
    const char *name;
    const char *desc;
    const char *args_help;     /* human-readable args description */
    bool        sandboxed;     /* run in tool-exec worker process */
    /* native handlers only (sandboxed==false): */
    aios_status (*native)(json_val *args, json_val **out,
                          char *err, size_t ecap);
} tool_def;

typedef struct tool_reg tool_reg;

/* jail_dir:    filesystem root for sandboxed fs tools (agent-visible fs)
 * store_dir:   where audit.log lives
 * agent_name:  identity stamped into audit records
 * mem:         optional memory context for memory.* tools (may be NULL) */
tool_reg *tool_reg_create(const char *jail_dir, const char *store_dir,
                          const char *agent_name, mem_ctx *mem);
void      tool_reg_free(tool_reg *tr);

aios_status  tool_reg_add(tool_reg *tr, const tool_def *def);
size_t       tool_reg_count(const tool_reg *tr);
const tool_def *tool_reg_find(tool_reg *tr, const char *name);

/* JSON array [{"name","desc","args"}...] — caller frees. */
char     *tool_reg_help_json(tool_reg *tr);

/* Execute: args is consumed (freed) by tool_exec in all cases.
 * On success *result_out holds the tool's JSON result. err gets a
 * human-readable message on failure. */
aios_status tool_exec(tool_reg *tr, const char *name, json_val *args,
                      json_val **result_out, char *err, size_t ecap);

/* Default M4 toolset (calc, fs.*, memory.search, proc.info). */
void tool_reg_install_default(tool_reg *tr);

/* Wall-clock budget for sandboxed tool calls (default 10s, min 0.5s). */
void tool_set_sandbox_timeout(double seconds);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_TOOLS_H */
