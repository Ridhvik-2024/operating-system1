/* aios/mem.h — M4: OS-owned 3-tier memory for agents.
 *
 * Tiers:
 *   MEM_WORKING  — RAM ring buffer, per-context, not persisted (cap 64).
 *   MEM_EPISODIC — append-only journal (persisted): what happened.
 *   MEM_SEMANTIC — consolidated facts (persisted): what is known.
 *
 * Retrieval: each entry gets a 256-dim embedding from hashed character
 * n-grams (3- and 4-grams, FNV-1a buckets, sublinear TF, L2-normalized).
 * Recall score = 0.62*cosine + 0.23*recency + 0.15*usefulness where
 * recency = exp(-age_hours / 72) and usefulness grows with recall hits.
 *
 * Persistence: JSONL journal <store>/memory.journal, fsync'd appends,
 * atomic compaction via mem_consolidate(). */
#ifndef AIOS_MEM_H
#define AIOS_MEM_H

#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEM_DIM 256

typedef enum { MEM_WORKING = 0, MEM_EPISODIC = 1, MEM_SEMANTIC = 2 } mem_tier;

typedef struct mem_entry {
    long        seq;
    double      ts;          /* unix ms when stored            */
    mem_tier    tier;
    int         hits;        /* recall usefulness counter      */
    char       *text;
    char       *meta_json;   /* owned copy of meta or NULL     */
    float       vec[MEM_DIM];
    double      score;       /* last recall score (out param)  */
} mem_entry;

typedef struct mem_ctx mem_ctx;

/* Open (or create) a memory store rooted at store_dir. */
mem_ctx *mem_open(const char *store_dir);
void     mem_close(mem_ctx *m);

/* Store text. Persisted for EPISODIC/SEMANTIC, RAM-only for WORKING.
 * meta_json may be NULL; it is stored verbatim (must be a JSON object
 * or null). Returns AIOS_OK / AIOS_ERR. */
aios_status mem_remember(mem_ctx *m, mem_tier tier, const char *text,
                         const char *meta_json);

/* Rank by score = 0.62*relevance + 0.23*recency + 0.15*usefulness over
 * EPISODIC+SEMANTIC (+ WORKING when include_working). Fills out[0..k-1]
 * (borrowed pointers — do not free entries; free() the array itself).
 * Hit counters of returned entries are incremented. Returns count. */
int mem_recall(mem_ctx *m, const char *query, int k, mem_entry **out);

/* Housekeeping: dedupe near-duplicates (cos > 0.93) in episodic,
 * promote top episodic facts into semantic tier, compact the journal.
 * Returns number of entries removed. */
int mem_consolidate(mem_ctx *m);

/* Stats snapshot. */
typedef struct { size_t working, episodic, semantic, journal_bytes; } mem_stats;
void mem_stats_of(mem_ctx *m, mem_stats *st);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_MEM_H */
