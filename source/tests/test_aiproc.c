/* tests/test_aiproc.c — M2 AI-native process model. */
#include "t.h"
#include "aios/aiproc.h"
#include <pthread.h>
#include <time.h>

static int g_counter = 0;

static int task_quick(aiproc *p, void *ud) {
    (void)p; (void)ud;
    g_counter++;
    return 0;
}

static int task_slow(aiproc *p, void *ud) {
    (void)ud;
    for (int i = 0; i < 30 && !aiproc_should_stop(p); i++) {
        aiproc_set_blocked(p, true);
        struct timespec ts = { 0, 50 * 1000 * 1000 }; /* 50ms */
        nanosleep(&ts, NULL);
        aiproc_set_blocked(p, false);
    }
    return aiproc_should_stop(p) ? 1 : 0;
}

static void run(void) {
    /* quick task lifecycle */
    aiproc *p = aiproc_spawn("increment the counter", task_quick, NULL);
    CHECK(p != NULL, "spawn");
    CHECK_STR_CONTAINS(aiproc_intent(p), "counter", "intent recorded");
    CHECK(aiproc_wait(p, 2000) == AIOS_OK, "wait done");
    CHECK(aiproc_state_of(p) == AIPROC_DONE, "state DONE");
    CHECK(aiproc_exit_code(p) == 0, "exit code 0");
    CHECK(g_counter == 1, "body ran once");
    aiproc_free(p);

    /* wait timeout path */
    aiproc *s = aiproc_spawn("sleep a bit", task_slow, NULL);
    CHECK(aiproc_wait(s, 60) == AIOS_ERR_TIMEOUT, "wait times out");
    CHECK(aiproc_state_of(s) == AIPROC_RUNNING ||
          aiproc_state_of(s) == AIPROC_BLOCKED, "still running/blocked");
    aiproc_free(s); /* kill+join inside free */

    /* explicit kill -> cooperative stop */
    aiproc *k = aiproc_spawn("loop forever until stopped", task_slow, NULL);
    aiproc_wait(k, 30); /* let it start */
    aiproc_kill(k, 1000);
    CHECK(aiproc_state_of(k) == AIPROC_KILLED ||
          aiproc_state_of(k) == AIPROC_DONE, "killed state");
    aiproc_free(k);
}

TEST_MAIN()
