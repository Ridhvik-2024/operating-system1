/* aios/ksched.h — M6 kernel bridge: scheduler intents (userspace side).
 *
 * The OS speaks to the kernel in intent classes, not nice numbers:
 *
 *   INTERACTIVE  what the human is talking to (aiosh console)  -> SCHED_OTHER, nice -5
 *   BATCH        tool workers doing bounded work               -> SCHED_BATCH, cgroup "tools"
 *   IDLE         background agents / spawned jobs              -> SCHED_IDLE,  cgroup "agents"
 *
 * Backed by three kernel facilities (all best-effort, never fatal):
 *   - sched_setattr(2)         policy per process
 *   - cgroup v2 cpu controller cost-of-AI work bounded per class
 *   - /dev/aios (aios_intent.ko) kernel-side intent registry + wakeup accounting
 */
#ifndef AIOS_KSCHED_H
#define AIOS_KSCHED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIOS_KS_INTERACTIVE = 0,
    AIOS_KS_BATCH       = 1,
    AIOS_KS_IDLE        = 2,
} aios_ksched_class;

/* cgroup group names per class ("system" holds everything else) */
const char *aios_ksched_cgroup_name(aios_ksched_class c);

/* apply the scheduling policy for a class to the CALLING process.
 * returns 0 ok, -1 unsupported/failed (errno set). */
int aios_ksched_apply(aios_ksched_class c);

/* register the calling pid with the kernel intent device (/dev/aios).
 * returns 0 ok, -1 if the kernel bridge module is not loaded. */
int aios_ksched_register(aios_ksched_class c);
int aios_ksched_unregister(void);

/* boot-time cgroup v2 setup (called by aios-init once):
 * mounts are expected done; creates the class groups and moves pid 1
 * into "system". Returns 0 ok, -1 best-effort failed. */
int aios_cgroup_boot(void);

/* move an arbitrary pid into a class group (tools/agents/system) */
int aios_cgroup_move_pid(long pid, aios_ksched_class c);

/* ---- M7: budget + usage plumbing for the autopilot governor ---- */

/* Set the cpu.max budget of a class group: pct in [1,100] means
 * pct% of one CPU ("pct*1000 100000" in cgroup v2). */
int aios_cgroup_budget_set(aios_ksched_class c, int pct);

/* Read the current budget back (cpu.max; "max" reports 100). */
int aios_cgroup_budget_get(aios_ksched_class c, int *pct);

/* Cumulative CPU time used by a class group, from its cpu.stat
 * (usage_usec line). */
int aios_cgroup_usage_us(aios_ksched_class c, uint64_t *out);

/* Per-class wakeup totals from /proc/aios (the "# per-class wakeups"
 * section the kernel bridge maintains). out[INTERACTIVE/BATCH/IDLE]. */
int aios_kernel_wakeups(uint64_t out[3], int *registered);

/* Registry scan: pids with their class, from the /proc/aios table.
 * Returns the number of entries (<= max), or -1 if /proc/aios absent. */
int aios_kernel_registry(long *pids, int *cls, int max);

/* combined human-readable status for the `kernel` shell builtin:
 * module registry, eBPF observers, cgroup limits, governor, policy. */
int aios_kernel_status(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_KSCHED_H */
