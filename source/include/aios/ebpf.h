/* aios/ebpf.h — M6: eBPF wakeup observer (scheduler hints, kernel side).
 *
 * Loads one tiny hand-assembled eBPF program attached to the
 * sched:sched_wakeup tracepoint; every wakeup of any task bumps a
 * per-pid counter in a hash map. The OS reads those counters to see
 * how "hot" a workload is without leaving the kernel's own accounting.
 *
 * Zero external dependencies: raw bpf(2) + perf_event_open(2), bytecode
 * assembled inline. Best-effort: aios_ebpf_start() returns -1 and sets
 * a readable error if the kernel refuses (older kernels, missing
 * tracepoints, verifier changes).
 */
#ifndef AIOS_EBPF_H
#define AIOS_EBPF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* load + attach. 0 ok, -1 unavailable (aios_ebpf_error() says why). */
int  aios_ebpf_start(void);

/* wakeup counter for one pid (0 if unknown). -1 if observer inactive. */
int  aios_ebpf_wakeups(long pid, uint64_t *out);

int  aios_ebpf_active(void);
void aios_ebpf_stop(void);
const char *aios_ebpf_error(void);

/* ---- M7: ctx-switch observer (sched:sched_switch) ----
 * Counts how often each task is switched OUT (descheduling events) —
 * the second scheduler-pressure signal the M7 autopilot governor reads
 * next to the M6 wakeup counts and the cgroup cpu.stat usage. Same
 * hand-assembled template, same best-effort contract. */
int  aios_ebpf_ctx_start(void);
int  aios_ebpf_ctx_count(long pid, uint64_t *out);
int  aios_ebpf_ctx_active(void);
void aios_ebpf_ctx_stop(void);
const char *aios_ebpf_ctx_error(void);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_EBPF_H */
