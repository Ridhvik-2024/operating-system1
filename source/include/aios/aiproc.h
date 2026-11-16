/* aios/aiproc.h — M2: AI-native process model.
 * A process is an *intent* that runs on its own thread with a semantic
 * lifecycle: INTENT -> READY -> RUNNING -> BLOCKED -> DONE | FAILED.
 * Cancellation is cooperative (aiproc_should_stop) + forced join. */
#ifndef AIOS_AIPROC_H
#define AIOS_AIPROC_H

#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIPROC_INTENT  = 0, /* created, intent registered      */
    AIPROC_READY   = 1, /* scheduled, about to run         */
    AIPROC_RUNNING = 2,
    AIPROC_BLOCKED = 3, /* waiting on I/O / a tool / IPC   */
    AIPROC_DONE    = 4,
    AIPROC_FAILED  = 5,
    AIPROC_KILLED  = 6
} aiproc_state;

typedef struct aiproc aiproc;

/* Entry point: return 0 (or AIOS_OK) for DONE, non-zero for FAILED. */
typedef int (*aiproc_main)(aiproc *self, void *ud);

/* Spawn a process around an intent. Thread starts immediately. */
aiproc *aiproc_spawn(const char *intent, aiproc_main fn, void *ud);

aiproc_state aiproc_state_of(const aiproc *p);
const char  *aiproc_intent(const aiproc *p);
const char  *aiproc_state_name(aiproc_state s);
uint64_t     aiproc_id(const aiproc *p);
int          aiproc_exit_code(const aiproc *p);
double       aiproc_started_ms(const aiproc *p);
double       aiproc_cpu_hint(const aiproc *p); /* M2 placeholder for sched hints */

/* Cooperative cancellation: long-running bodies should poll this. */
bool aiproc_should_stop(const aiproc *p);
/* Set BLOCKED while waiting (informational for M2 scheduler). */
void aiproc_set_blocked(aiproc *p, bool blocked);

/* Wait for completion. ms==0 waits forever. Returns AIOS_OK when the
 * process reached a terminal state, AIOS_ERR_TIMEOUT otherwise. */
aios_status aiproc_wait(aiproc *p, unsigned ms);

/* Request stop and wait up to ms for the thread; force-join after. */
void aiproc_kill(aiproc *p, unsigned ms);

void aiproc_free(aiproc *p);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_AIPROC_H */
