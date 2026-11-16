/* aios-demo — M4 showcase: an AI-OS agent process pursuing a goal with
 * sandboxed tools and OS-owned persistent memory.
 *
 *   ./aios-demo                real LLM cortex (z-ai bridge)
 *   ./aios-demo --scripted     deterministic offline cortex
 *   ./aios-demo --goal "..."   custom intent
 *   ./aios-demo --steps 12     raise the step budget
 */
#include "aios/agent.h"
#include "aios/aiproc.h"
#include "aios/sembus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sembus *g_bus = NULL;

static void on_bus_event(const char *topic, const char *payload,
                         size_t len, void *ud) {
    (void)ud;
    printf("  [sembus] %s: %.*s\n", topic, (int)(len > 120 ? 120 : len), payload);
}

int main(int argc, char **argv) {
    const char *goal =
        "Summarize this workspace: list the files, read each text file, "
        "estimate the total token count (about chars/4), and write the "
        "estimate with a one-line description of each file into summary.md";
    bool scripted = false;
    const char *store = ".aios-store";
    const char *jail = "./workspace";
    int max_steps = 10;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scripted") == 0) scripted = true;
        else if (strcmp(argv[i], "--goal") == 0 && i + 1 < argc) goal = argv[++i];
        else if (strcmp(argv[i], "--store") == 0 && i + 1 < argc) store = argv[++i];
        else if (strcmp(argv[i], "--jail") == 0 && i + 1 < argc) jail = argv[++i];
        else if (strcmp(argv[i], "--steps") == 0 && i + 1 < argc) max_steps = atoi(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: aios-demo [--scripted] [--goal TEXT] "
                    "[--store DIR] [--jail DIR] [--steps N]\n");
            return 2;
        }
    }

    printf("AI-OS %s — agent runtime demo (M4)\n", AIOS_VERSION);
    printf("store=%s jail=%s cortex=%s\n\n", store, jail,
           scripted ? "scripted" : "llm");

    /* M2/M3 on display: the agent runs as an intent-driven aiproc and
     * announces lifecycle events on the semantic bus. */
    g_bus = sembus_create();
    sembus_sub(g_bus, "*", on_bus_event, NULL);

    mem_ctx *mem = mem_open(store);
    if (!mem) { fprintf(stderr, "cannot open memory store\n"); return 1; }
    tool_reg *tools = tool_reg_create(jail, store, "architect", mem);
    if (!tools) { fprintf(stderr, "cannot create tool registry\n"); return 1; }
    tool_reg_install_default(tools);

    cortex *cx = scripted ? cortex_scripted_create(goal) : cortex_llm_create(NULL);
    if (!cx) { fprintf(stderr, "cannot create cortex (is `z-ai` on PATH?)\n"); return 1; }

    agent_cfg cfg = {
        .name = "architect",
        .goal = goal,
        .cx = cx,
        .tools = tools,
        .mem = mem,
        .max_steps = max_steps
    };

    double t0 = aios_mono_ms();
    sembus_pub_kv(g_bus, "agent.start",
                  (const char *const[]){"agent", "architect", "cortex",
                                        scripted ? "scripted" : "llm", NULL});
    char final[2048] = {0};
    aios_status st = agent_run(&cfg, final, sizeof final);
    double ms = aios_mono_ms() - t0;
    sembus_pub_kv(g_bus, "agent.done",
                  (const char *const[]){"agent", "architect",
                                        "status", st == AIOS_OK ? "achieved" : aios_status_str(st),
                                        NULL});

    printf("\n== agent finished in %.0f ms (%s) ==\n%s\n", ms,
           st == AIOS_OK ? "goal achieved" : aios_status_str(st),
           final[0] ? final : "(no final answer)");

    mem_stats ms_stats;
    mem_stats_of(mem, &ms_stats);
    printf("\nmemory: working=%zu episodic=%zu semantic=%zu journal=%zuB\n",
           ms_stats.working, ms_stats.episodic, ms_stats.semantic,
           ms_stats.journal_bytes);
    printf("audit log: %s/audit.log\n", store);
    printf("tip: run ./aios-memctl %s recall \"token estimate\" to inspect "
           "OS-owned memory\n", store);

    cortex_free(cx);
    tool_reg_free(tools);
    mem_close(mem);
    sembus_destroy(g_bus);
    return st == AIOS_OK ? 0 : 1;
}
