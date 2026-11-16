/* aios/aiproc.c — M2 implementation: intent-driven processes on pthreads. */
#include "aios/aiproc.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct aiproc {
    uint64_t id;
    char *intent;
    aiproc_main fn;
    void *ud;
    volatile int state;      /* aiproc_state */
    volatile int should_stop;
    volatile int exit_code;
    double started_ms;
    pthread_t tid;
    bool thread_live;
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

static uint64_t g_next_pid = 1;

const char *aiproc_state_name(aiproc_state s) {
    switch (s) {
    case AIPROC_INTENT:  return "INTENT";
    case AIPROC_READY:   return "READY";
    case AIPROC_RUNNING: return "RUNNING";
    case AIPROC_BLOCKED: return "BLOCKED";
    case AIPROC_DONE:    return "DONE";
    case AIPROC_FAILED:  return "FAILED";
    case AIPROC_KILLED:  return "KILLED";
    }
    return "?";
}

static void *trampoline(void *arg) {
    aiproc *p = (aiproc *)arg;
    pthread_mutex_lock(&p->mu);
    p->state = AIPROC_RUNNING;
    p->started_ms = aios_mono_ms();
    pthread_mutex_unlock(&p->mu);

    int rc = p->fn(p, p->ud);

    pthread_mutex_lock(&p->mu);
    p->exit_code = rc;
    p->state = (rc == 0) ? AIPROC_DONE
              : (p->should_stop ? AIPROC_KILLED : AIPROC_FAILED);
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

aiproc *aiproc_spawn(const char *intent, aiproc_main fn, void *ud) {
    if (!intent || !fn) return NULL;
    aiproc *p = (aiproc *)calloc(1, sizeof(aiproc));
    if (!p) return NULL;
    p->id = g_next_pid++;
    p->intent = strdup(intent);
    p->fn = fn;
    p->ud = ud;
    p->state = AIPROC_INTENT;
    if (!p->intent) { free(p); return NULL; }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);

    p->state = AIPROC_READY;
    if (pthread_create(&p->tid, NULL, trampoline, p) != 0) {
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
        free(p->intent);
        free(p);
        return NULL;
    }
    p->thread_live = true;
    return p;
}

aiproc_state aiproc_state_of(const aiproc *p) {
    return p ? (aiproc_state)p->state : AIPROC_INTENT;
}

const char *aiproc_intent(const aiproc *p) { return p ? p->intent : NULL; }
uint64_t aiproc_id(const aiproc *p)        { return p ? p->id : 0; }
int aiproc_exit_code(const aiproc *p)      { return p ? p->exit_code : -1; }
double aiproc_started_ms(const aiproc *p)  { return p ? p->started_ms : 0; }
double aiproc_cpu_hint(const aiproc *p)    { (void)p; return 0.0; }
bool aiproc_should_stop(const aiproc *p)   { return p ? !!p->should_stop : true; }

void aiproc_set_blocked(aiproc *p, bool blocked) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    if (p->state == AIPROC_RUNNING || p->state == AIPROC_BLOCKED)
        p->state = blocked ? AIPROC_BLOCKED : AIPROC_RUNNING;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

aios_status aiproc_wait(aiproc *p, unsigned ms) {
    if (!p) return AIOS_ERR_ARG;
    pthread_mutex_lock(&p->mu);
    if (ms == 0) {
        while (p->state < AIPROC_DONE)
            pthread_cond_wait(&p->cv, &p->mu);
        pthread_mutex_unlock(&p->mu);
        return AIOS_OK;
    }
    double deadline = aios_mono_ms() + (double)ms;
    while (p->state < AIPROC_DONE) {
        double remain = deadline - aios_mono_ms();
        if (remain <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(remain / 1000.0);
        ts.tv_nsec += (long)((remain - (double)(long)(remain / 1000.0) * 1000.0) * 1e6);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&p->cv, &p->mu, &ts);
    }
    aios_status rc = p->state >= AIPROC_DONE ? AIOS_OK : AIOS_ERR_TIMEOUT;
    pthread_mutex_unlock(&p->mu);
    return rc;
}

void aiproc_kill(aiproc *p, unsigned ms) {
    if (!p) return;
    p->should_stop = 1;
    if (p->thread_live) {
        if (aiproc_wait(p, ms) != AIOS_OK) {
            pthread_detach(p->tid);
            p->thread_live = false;
        }
    }
}

void aiproc_free(aiproc *p) {
    if (!p) return;
    if (p->thread_live) {
        aiproc_kill(p, 500);
    }
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p->intent);
    free(p);
}
