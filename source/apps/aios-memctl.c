/* aios-memctl — inspect the OS-owned agent memory store.
 *
 *   aios-memctl <store> stats
 *   aios-memctl <store> recall <query> [k]
 *   aios-memctl <store> consolidate
 */
#include "aios/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *tier_name(mem_tier t) {
    switch (t) {
    case MEM_WORKING:  return "working";
    case MEM_EPISODIC: return "episodic";
    case MEM_SEMANTIC: return "semantic";
    }
    return "?";
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: aios-memctl <store> stats|recall <query> [k]|consolidate\n");
        return 2;
    }
    mem_ctx *m = mem_open(argv[1]);
    if (!m) { fprintf(stderr, "cannot open store %s\n", argv[1]); return 1; }

    if (strcmp(argv[2], "stats") == 0) {
        mem_stats st;
        mem_stats_of(m, &st);
        printf("working=%zu episodic=%zu semantic=%zu journal_bytes=%zu\n",
               st.working, st.episodic, st.semantic, st.journal_bytes);
    } else if (strcmp(argv[2], "recall") == 0) {
        if (argc < 4) { fprintf(stderr, "recall needs a query\n"); return 2; }
        int k = argc >= 5 ? atoi(argv[4]) : 5;
        mem_entry *hits[16];
        int n = mem_recall(m, argv[3], k > 16 ? 16 : k, hits);
        printf("top %d for \"%s\":\n", n, argv[3]);
        for (int i = 0; i < n; i++)
            printf("  [%.3f] %-8s %s\n", hits[i]->score,
                   tier_name(hits[i]->tier), hits[i]->text);
        if (n == 0) printf("  (no relevant entries)\n");
    } else if (strcmp(argv[2], "consolidate") == 0) {
        int removed = mem_consolidate(m);
        printf("consolidated: %d entries removed/merged\n", removed);
    } else {
        fprintf(stderr, "unknown command %s\n", argv[2]);
        mem_close(m);
        return 2;
    }
    mem_close(m);
    return 0;
}
