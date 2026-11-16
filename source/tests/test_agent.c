/* tests/test_agent.c — M4 integration: full agentic loop with the
 * scripted cortex, memory writeback, cross-session recall. */
#include "t.h"
#include "aios/agent.h"
#include <sys/stat.h>

static void rmrf(const char *d) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", d);
    (void)!system(cmd);
}

static void run(void) {
    const char *jail = "build/test-agent-jail";
    const char *store = "build/test-agent-store";
    rmrf(jail); rmrf(store);
    (void)!system("mkdir -p build/test-agent-jail build/test-agent-store");
    (void)!system("printf 'AI-OS notes: intents drive scheduling\\n' > build/test-agent-jail/notes.txt");
    (void)!system("printf 'module,lines\\ncore,1840\\n' > build/test-agent-jail/metrics.csv");

    mem_ctx *mem = mem_open(store);
    CHECK(mem != NULL, "open mem");
    tool_reg *tools = tool_reg_create(jail, store, "tester", mem);
    CHECK(tools != NULL, "open tools");
    tool_reg_install_default(tools);

    const char *goal = "summarize workspace";
    cortex *cx = cortex_scripted_create(goal);
    CHECK(cx != NULL, "scripted cortex");

    agent_cfg cfg = {
        .name = "tester-agent",
        .goal = goal,
        .cx = cx,
        .tools = tools,
        .mem = mem,
        .max_steps = 8
    };
    char final[1024] = {0};
    aios_status st = agent_run(&cfg, final, sizeof final);
    CHECK(st == AIOS_OK, "agent completed goal");
    CHECK_STR_CONTAINS(final, "summary.md", "final mentions output file");

    /* output file written into the jail */
    struct stat sb;
    CHECK(stat("build/test-agent-jail/summary.md", &sb) == 0,
          "summary.md exists in jail");

    /* memory got episodic + semantic entries */
    mem_stats ms;
    mem_stats_of(mem, &ms);
    CHECK(ms.episodic >= 3, "episodic writeback");
    CHECK(ms.semantic >= 1, "semantic facts recorded");

    /* recall finds the completion among its top hits (the current
     * session goal itself may legitimately rank first) */
    mem_entry *hits[4];
    int n = mem_recall(mem, "did the agent write the workspace summary?", 3, hits);
    CHECK(n >= 1, "recall after run");
    bool found_summary = false;
    for (int i = 0; i < n; i++)
        if (hits[i]->text && strstr(hits[i]->text, "summary")) found_summary = true;
    if (!found_summary && n >= 1)
        fprintf(stderr, "[dbg] top hits for debugging:\n");
    for (int i = 0; i < n && !found_summary; i++)
        fprintf(stderr, "[dbg]   [%.3f] %s\n", hits[i]->score, hits[i]->text);
    CHECK(found_summary, "summary recallable from top hits");

    /* audit log captured every tool call */
    char apath[256];
    snprintf(apath, sizeof apath, "%s/audit.log", store);
    CHECK(stat(apath, &sb) == 0 && sb.st_size > 200, "audit log populated");

    cortex_free(cx);

    /* --- cross-session: a NEW agent instance recalls prior knowledge --- */
    tool_reg *tools2 = tool_reg_create(jail, store, "tester2", mem);
    tool_reg_install_default(tools2);
    cortex *cx2 = cortex_scripted_create(goal);
    agent_cfg cfg2 = {
        .name = "tester-agent-2",
        .goal = "summarize workspace",
        .cx = cx2,
        .tools = tools2,
        .mem = mem,
        .max_steps = 8
    };
    char final2[1024] = {0};
    st = agent_run(&cfg2, final2, sizeof final2);
    CHECK(st == AIOS_OK, "second session completed");
    CHECK_STR_CONTAINS(final2, "summary.md", "second session final");

    mem_stats_of(mem, &ms);
    CHECK(ms.semantic >= 2, "facts accumulated across sessions");
    CHECK(ms.episodic >= 6, "episodes accumulated across sessions");

    cortex_free(cx2);
    tool_reg_free(tools);
    tool_reg_free(tools2);
    mem_close(mem);
}

TEST_MAIN()
