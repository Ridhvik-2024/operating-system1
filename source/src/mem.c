/* aios/mem.c — M4 implementation: 3-tier agent memory with hashed
 * n-gram embeddings, scored recall, JSONL persistence, consolidation. */
#include "aios/mem.h"
#include "aios/json.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define WORKING_CAP   64
#define EPISODIC_CAP  512
#define SEMANTIC_CAP  256
#define JOURNAL_FILE  "memory.journal"
#define MAX_TEXT      8192
#define MAX_META      2048

struct mem_ctx {
    char dir[768];
    char journal_path[900];
    pthread_mutex_t mu;
    mem_entry **entries;   /* all tiers in one array, tier tagged */
    size_t n, cap;
    long   next_seq;
    bool   dirty;          /* journal has unpersisted compaction? */
};

/* ---------------- embedding ---------------- */

static bool is_word_sep(unsigned char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r' ||
           c == '.' || c == ',' || c == ';' || c == ':' ||
           c == '/' || c == '\\' || c == '(' || c == ')';
}

/* Hashed char n-gram embedding: 3- and 4-gram buckets, sublinear TF. */
static void embed_text(const char *text, size_t len, float vec[MEM_DIM]) {
    memset(vec, 0, sizeof(float) * MEM_DIM);
    if (!text || len == 0) return;
    double counts[MEM_DIM];
    memset(counts, 0, sizeof counts);
    size_t total = 0;

    for (size_t n = 3; n <= 4; n++) {
        if (len < n) continue;
        for (size_t i = 0; i + n <= len; i++) {
            /* skip grams that span separators in a way that merges words */
            if (is_word_sep((unsigned char)text[i]) ||
                is_word_sep((unsigned char)text[i + n - 1])) continue;
            uint64_t h = aios_fnv1a(text + i, n);
            counts[h % MEM_DIM] += 1.0;
            total++;
        }
    }
    if (total == 0) return;
    double norm = 0.0;
    for (int i = 0; i < MEM_DIM; i++) {
        if (counts[i] > 0.0) counts[i] = 1.0 + log(counts[i]); /* sublinear */
        norm += counts[i] * counts[i];
    }
    norm = sqrt(norm);
    if (norm <= 0.0) return;
    for (int i = 0; i < MEM_DIM; i++) vec[i] = (float)(counts[i] / norm);
}

static float cosine(const float *a, const float *b) {
    float dot = 0.0f;
    for (int i = 0; i < MEM_DIM; i++) dot += a[i] * b[i];
    if (dot < 0.0f) dot = 0.0f;
    if (dot > 1.0f) dot = 1.0f;
    return dot;
}

/* ---------------- entries ---------------- */

static mem_entry *entry_new(mem_tier tier, const char *text,
                            const char *meta_json) {
    mem_entry *e = (mem_entry *)calloc(1, sizeof(mem_entry));
    if (!e) return NULL;
    e->tier = tier;
    e->ts = aios_now_ms();
    e->text = strdup(text);
    e->meta_json = meta_json ? strdup(meta_json) : NULL;
    if (!e->text || (meta_json && !e->meta_json)) {
        free(e->text); free(e->meta_json); free(e);
        return NULL;
    }
    embed_text(text, strlen(text), e->vec);
    return e;
}

static void entry_free(mem_entry *e) {
    if (!e) return;
    free(e->text);
    free(e->meta_json);
    free(e);
}

/* ---------------- journal ---------------- */

/* Serialize one entry as a JSONL line into buf (caller frees). */
static char *entry_to_line(const mem_entry *e) {
    json_val *o = json_new_obj();
    if (!o) return NULL;
    json_obj_set(o, "seq",  json_new_num((double)e->seq));
    json_obj_set(o, "ts",   json_new_num(e->ts));
    json_obj_set(o, "tier", json_new_num((double)e->tier));
    json_obj_set(o, "hits", json_new_num((double)e->hits));
    json_obj_set(o, "text", json_new_str(e->text));
    if (e->meta_json) {
        json_val *meta = json_parse(e->meta_json,
                                    strlen(e->meta_json));
        json_obj_set(o, "meta", meta ? meta : json_new_null());
    } else {
        json_obj_set(o, "meta", json_new_null());
    }
    char *line = json_dump(o);
    json_free(o);
    return line;
}

static aios_status journal_append(mem_ctx *m, const mem_entry *e) {
    char *line = entry_to_line(e);
    if (!line) return AIOS_ERR_NOMEM;
    size_t len = strlen(line);
    FILE *f = fopen(m->journal_path, "ab");
    if (!f) { free(line); return AIOS_ERR_IO; }
    bool ok = fwrite(line, 1, len, f) == len;
    ok = ok && fwrite("\n", 1, 1, f) == 1;
    if (fflush(f) != 0) ok = false;
    if (fsync(fileno(f)) != 0) ok = false;
    fclose(f);
    free(line);
    return ok ? AIOS_OK : AIOS_ERR_IO;
}

/* Rewrite the journal from current episodic+semantic entries atomically. */
static aios_status journal_compact(mem_ctx *m) {
    char tmp[960];
    snprintf(tmp, sizeof tmp, "%s.compact", m->journal_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return AIOS_ERR_IO;
    aios_status st = AIOS_OK;
    for (size_t i = 0; i < m->n && st == AIOS_OK; i++) {
        mem_entry *e = m->entries[i];
        if (e->tier == MEM_WORKING) continue;
        char *line = entry_to_line(e);
        if (!line) { st = AIOS_ERR_NOMEM; break; }
        size_t len = strlen(line);
        if (fwrite(line, 1, len, f) != len || fwrite("\n", 1, 1, f) != 1)
            st = AIOS_ERR_IO;
        free(line);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) st = AIOS_ERR_IO;
    fclose(f);
    if (st != AIOS_OK) { remove(tmp); return st; }
    if (rename(tmp, m->journal_path) != 0) { remove(tmp); return AIOS_ERR_IO; }
    return AIOS_OK;
}

static aios_status load_journal(mem_ctx *m) {
    FILE *f = fopen(m->journal_path, "rb");
    if (!f) return AIOS_OK; /* fresh store */
    char *line = NULL;
    size_t cap = 0;
    long max_seq = 0;
    ssize_t got;
    while ((got = getline(&line, &cap, f)) > 0) {
        while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r'))
            line[--got] = 0;
        if (got == 0) continue;
        json_val *o = json_parse(line, (size_t)got);
        if (!o) continue;
        json_val *jtext = json_obj_get(o, "text");
        const char *text = json_str(jtext);
        if (text) {
            mem_tier tier = (mem_tier)(int)json_num(json_obj_get(o, "tier"));
            if (tier != MEM_EPISODIC && tier != MEM_SEMANTIC) tier = MEM_EPISODIC;
            json_val *jmeta = json_obj_get(o, "meta");
            char *meta_str = NULL;
            if (jmeta && json_type_of(jmeta) == JSON_OBJ) {
                meta_str = json_dump(jmeta);
            }
            mem_entry *e = entry_new(tier, text, meta_str);
            free(meta_str);
            if (e) {
                e->seq  = (long)json_num(json_obj_get(o, "seq"));
                e->ts   = json_num(json_obj_get(o, "ts"));
                e->hits = (int)json_num(json_obj_get(o, "hits"));
                if (e->seq > max_seq) max_seq = e->seq;
                if (m->n == m->cap) {
                    m->cap = m->cap ? m->cap * 2 : 64;
                    mem_entry **ne = (mem_entry **)realloc(
                        m->entries, m->cap * sizeof(*ne));
                    if (!ne) { entry_free(e); json_free(o); break; }
                    m->entries = ne;
                }
                m->entries[m->n++] = e;
            }
        }
        json_free(o);
    }
    free(line);
    fclose(f);
    m->next_seq = max_seq + 1;
    return AIOS_OK;
}

/* ---------------- scoring ---------------- */

static double recency_of(const mem_entry *e) {
    double age_h = (aios_now_ms() - e->ts) / 3600000.0;
    if (age_h < 0) age_h = 0;
    return exp(-age_h / 72.0);
}

static double score_of(const mem_entry *e, const float *qvec,
                       double max_hits) {
    double rel = (double)cosine(qvec, e->vec);
    double rec = recency_of(e);
    double use = max_hits > 0 ? log1p((double)e->hits) / log1p(max_hits) : 0.0;
    return 0.62 * rel + 0.23 * rec + 0.15 * use;
}

/* ---------------- public API ---------------- */

mem_ctx *mem_open(const char *store_dir) {
    if (!store_dir || !*store_dir) return NULL;
    mem_ctx *m = (mem_ctx *)calloc(1, sizeof(mem_ctx));
    if (!m) return NULL;
    if (strlen(store_dir) >= sizeof m->dir - 64) { free(m); return NULL; }
    snprintf(m->dir, sizeof m->dir, "%s", store_dir);
    snprintf(m->journal_path, sizeof m->journal_path, "%s/%s",
             m->dir, JOURNAL_FILE);
    if (aios_mkdirs(m->dir) != AIOS_OK) { free(m); return NULL; }
    pthread_mutex_init(&m->mu, NULL);
    load_journal(m);
    return m;
}

void mem_close(mem_ctx *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->n; i++) entry_free(m->entries[i]);
    free(m->entries);
    pthread_mutex_unlock(&m->mu);
    pthread_mutex_destroy(&m->mu);
    free(m);
}

aios_status mem_remember(mem_ctx *m, mem_tier tier, const char *text,
                         const char *meta_json) {
    if (!m || !text || tier < MEM_WORKING || tier > MEM_SEMANTIC)
        return AIOS_ERR_ARG;
    char clean[MAX_TEXT];
    size_t len = strlen(text);
    if (len >= sizeof clean) len = sizeof clean - 1;
    memcpy(clean, text, len);
    clean[len] = 0;
    /* strip newlines into spaces for cleaner lines */
    for (char *p = clean; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';

    if (meta_json && strlen(meta_json) >= MAX_META) return AIOS_ERR_ARG;

    pthread_mutex_lock(&m->mu);
    mem_entry *e = entry_new(tier, clean, meta_json);
    if (!e) { pthread_mutex_unlock(&m->mu); return AIOS_ERR_NOMEM; }

    if (tier == MEM_WORKING) {
        /* ring buffer: drop oldest working entry beyond cap */
        size_t wcount = 0;
        for (size_t i = 0; i < m->n; i++)
            if (m->entries[i]->tier == MEM_WORKING) wcount++;
        if (wcount >= WORKING_CAP) {
            for (size_t i = 0; i < m->n; i++) {
                if (m->entries[i]->tier == MEM_WORKING) {
                    entry_free(m->entries[i]);
                    memmove(&m->entries[i], &m->entries[i + 1],
                            (m->n - i - 1) * sizeof(*m->entries));
                    m->n--;
                    break;
                }
            }
        }
    } else {
        e->seq = m->next_seq++;
        aios_status st = journal_append(m, e);
        if (st != AIOS_OK) { entry_free(e); pthread_mutex_unlock(&m->mu); return st; }
    }

    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        mem_entry **ne = (mem_entry **)realloc(m->entries, m->cap * sizeof(*ne));
        if (!ne) { entry_free(e); pthread_mutex_unlock(&m->mu); return AIOS_ERR_NOMEM; }
        m->entries = ne;
    }
    m->entries[m->n++] = e;
    pthread_mutex_unlock(&m->mu);
    return AIOS_OK;
}

static int score_cmp(const void *a, const void *b) {
    const mem_entry *const *ea = (const mem_entry *const *)a;
    const mem_entry *const *eb = (const mem_entry *const *)b;
    if ((*eb)->score > (*ea)->score) return 1;
    if ((*eb)->score < (*ea)->score) return -1;
    return 0;
}

int mem_recall(mem_ctx *m, const char *query, int k, mem_entry **out) {
    if (!m || !query || k <= 0 || !out) return 0;
    *out = NULL;
    float qvec[MEM_DIM];
    embed_text(query, strlen(query), qvec);

    pthread_mutex_lock(&m->mu);
    double max_hits = 1.0;
    for (size_t i = 0; i < m->n; i++)
        if ((double)m->entries[i]->hits > max_hits) max_hits = (double)m->entries[i]->hits;

    mem_entry **cand = (mem_entry **)malloc(m->n * sizeof(*cand));
    if (!cand) { pthread_mutex_unlock(&m->mu); return 0; }
    size_t nc = 0;
    for (size_t i = 0; i < m->n; i++) {
        mem_entry *e = m->entries[i];
        /* relevance gate: hashed n-grams collide, so unrelated text sits
         * around cos ~0.15; require meaningful similarity (0.25) before
         * recency/usefulness may boost an entry into the results */
        if (cosine(qvec, e->vec) < 0.25f) continue;
        e->score = score_of(e, qvec, max_hits);
        cand[nc++] = e;
    }
    qsort(cand, nc, sizeof(*cand), score_cmp);

    int got = 0;
    for (size_t i = 0; i < nc && got < k; i++) {
        if (cand[i]->score <= 0.05) break; /* composite floor */
        out[got++] = cand[i];
        cand[i]->hits++;
    }
    pthread_mutex_unlock(&m->mu);
    free(cand);
    return got;
}

int mem_consolidate(mem_ctx *m) {
    if (!m) return 0;
    pthread_mutex_lock(&m->mu);

    float qvec[MEM_DIM];
    memset(qvec, 0, sizeof qvec); /* neutral vector: score = recency+use only */
    double max_hits = 1.0;
    for (size_t i = 0; i < m->n; i++)
        if ((double)m->entries[i]->hits > max_hits) max_hits = (double)m->entries[i]->hits;

    int removed = 0;

    /* 1) dedupe near-duplicates within episodic (cos > 0.93), keep newer */
    for (size_t i = 0; i < m->n; i++) {
        mem_entry *a = m->entries[i];
        if (a->tier != MEM_EPISODIC) continue;
        for (size_t j = i + 1; j < m->n; j++) {
            mem_entry *b = m->entries[j];
            if (b->tier != MEM_EPISODIC) continue;
            if (cosine(a->vec, b->vec) > 0.93f) {
                a->hits += b->hits + 1;   /* merged usefulness */
                if (b->seq > a->seq) a->ts = b->ts; /* keep fresher ts */
                entry_free(b);
                memmove(&m->entries[j], &m->entries[j + 1],
                        (m->n - j - 1) * sizeof(*m->entries));
                m->n--;
                removed++;
                j--; /* re-examine slot */
            }
        }
    }

    /* 2) promote top-8 episodic by (recency+use) into semantic tier */
    mem_entry **cand = (mem_entry **)malloc((m->n ? m->n : 1) * sizeof(*cand));
    size_t nc = 0;
    if (cand) {
        for (size_t i = 0; i < m->n; i++)
            if (m->entries[i]->tier == MEM_EPISODIC) cand[nc++] = m->entries[i];
        for (size_t i = 0; i < nc; i++)
            cand[i]->score = score_of(cand[i], qvec, max_hits);
        qsort(cand, nc, sizeof(*cand), score_cmp);
        int promoted = 0;
        for (size_t i = 0; i < nc && promoted < 8; i++) {
            if (cand[i]->score < 0.30) break;
            cand[i]->tier = MEM_SEMANTIC;
            promoted++;
        }
        free(cand);
    }

    /* 3) enforce caps: evict lowest-(ts+hits) episodic beyond EPISODIC_CAP */
    size_t epi = 0;
    for (size_t i = 0; i < m->n; i++)
        if (m->entries[i]->tier == MEM_EPISODIC) epi++;
    while (epi > EPISODIC_CAP) {
        /* find oldest low-hit episodic */
        size_t victim = (size_t)-1;
        double worst = 1e18;
        for (size_t i = 0; i < m->n; i++) {
            mem_entry *e = m->entries[i];
            if (e->tier != MEM_EPISODIC) continue;
            double v = e->ts - 1000.0 * (double)e->hits * 3600.0;
            if (v < worst) { worst = v; victim = i; }
        }
        if (victim == (size_t)-1) break;
        entry_free(m->entries[victim]);
        memmove(&m->entries[victim], &m->entries[victim + 1],
                (m->n - victim - 1) * sizeof(*m->entries));
        m->n--;
        epi--;
        removed++;
    }

    /* 4) compact journal to current state */
    if (journal_compact(m) != AIOS_OK) {
        /* leave in-memory state consistent even if fs failed */
    }
    pthread_mutex_unlock(&m->mu);
    return removed;
}

void mem_stats_of(mem_ctx *m, mem_stats *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    if (!m) return;
    pthread_mutex_lock(&m->mu);
    for (size_t i = 0; i < m->n; i++) {
        switch (m->entries[i]->tier) {
        case MEM_WORKING:  st->working++;  break;
        case MEM_EPISODIC: st->episodic++; break;
        case MEM_SEMANTIC: st->semantic++; break;
        }
    }
    struct stat sb;
    if (stat(m->journal_path, &sb) == 0) st->journal_bytes = (size_t)sb.st_size;
    pthread_mutex_unlock(&m->mu);
}
