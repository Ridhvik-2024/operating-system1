/* tests/test_mem.c — M4 memory subsystem: ranking, persistence,
 * consolidation, working-tier cap. */
#include "t.h"
#include "aios/mem.h"
#include <sys/stat.h>

static void wipe(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void)!system(cmd);
}

static void run(void) {
    const char *store = "build/test-mem-store";
    wipe(store);

    mem_ctx *m = mem_open(store);
    CHECK(m != NULL, "open store");

    /* store topical entries */
    CHECK(mem_remember(m, MEM_EPISODIC,
                       "estimated the workspace token count at 420 tokens",
                       NULL) == AIOS_OK, "remember e1");
    CHECK(mem_remember(m, MEM_EPISODIC,
                       "the scheduler uses intents to prioritize aiproc tasks",
                       NULL) == AIOS_OK, "remember e2");
    CHECK(mem_remember(m, MEM_EPISODIC,
                       "wrote summary.md with the token estimate",
                       NULL) == AIOS_OK, "remember e3");
    CHECK(mem_remember(m, MEM_SEMANTIC,
                       "the token estimate for the workspace is about 420",
                       "{\"fact\":true}") == AIOS_OK, "remember s1");
    CHECK(mem_remember(m, MEM_WORKING,
                       "current step: reading files", NULL) == AIOS_OK,
          "remember w1");

    /* ranking: token-related query ranks token entries first */
    mem_entry *hits[8];
    int n = mem_recall(m, "how many tokens in the workspace?", 3, hits);
    CHECK(n >= 2, "recall found entries");
    CHECK_STR_CONTAINS(hits[0]->text, "token", "top hit is token-related");
    CHECK(hits[0]->score >= hits[1]->score, "scores sorted");

    /* relevance floor: unrelated query returns nothing */
    n = mem_recall(m, "kkkvvv xxxzzz qqqjjj", 3, hits);
    CHECK(n == 0, "irrelevant query filtered by floor");

    /* persistence across close/open */
    mem_stats st;
    mem_stats_of(m, &st);
    size_t episodic_before = st.episodic, semantic_before = st.semantic;
    CHECK(episodic_before == 3, "episodic count");
    CHECK(semantic_before == 1, "semantic count");
    CHECK(st.working == 1, "working count");
    CHECK(st.journal_bytes > 0, "journal exists");
    mem_close(m);

    m = mem_open(store);
    CHECK(m != NULL, "reopen store");
    mem_stats_of(m, &st);
    CHECK(st.episodic == episodic_before, "episodic persisted");
    CHECK(st.semantic == semantic_before, "semantic persisted");
    CHECK(st.working == 0, "working tier is RAM-only");

    /* consolidation: near-duplicates merge; top episodic facts get
     * promoted into the semantic tier (episodic + semantic conserved) */
    CHECK(mem_remember(m, MEM_EPISODIC,
                       "estimated the workspace token count at 420 tokens",
                       NULL) == AIOS_OK, "remember duplicate");
    int removed = mem_consolidate(m);
    CHECK(removed >= 1, "consolidation removed duplicates");
    mem_stats_of(m, &st);
    CHECK(st.episodic + st.semantic == 4, "entries conserved across tiers");
    CHECK(st.semantic > semantic_before, "promotion happened");

    /* recall still works after compaction */
    n = mem_recall(m, "workspace token estimate", 5, hits);
    CHECK(n >= 1, "recall after consolidate");
    mem_close(m);

    /* working tier ring cap */
    wipe(store);
    m = mem_open(store);
    CHECK(m != NULL, "fresh store");
    for (int i = 0; i < 70; i++) {
        char line[64];
        snprintf(line, sizeof line, "working entry number %d", i);
        mem_remember(m, MEM_WORKING, line, NULL);
    }
    mem_stats_of(m, &st);
    CHECK(st.working == 64, "working cap enforced");
    mem_close(m);
}

TEST_MAIN()
