/* microlm.c — M7: the AI-OS micro language model (runtime side).
 * See include/aios/microlm.h and tools/microlm_gen.py (the trainer).
 *
 * Classifier: multinomial naive Bayes over unigrams, Laplace-smoothed,
 * scored in log space with 256-fixed-point log-probs precomputed at
 * first use (deterministic; integer math only afterwards).
 * Generator: bigram chain over the OS voice corpus with an LCG seeded
 * by FNV-1a of the prompt — identical prompt, identical sentence.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aios/microlm.h"
#include "aios/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "microlm_weights.h"

#define MICROLM_MAX_TOKENS 64
#define MICROLM_MAX_WORD   31

/* ---------------- tokenizer ---------------- */

static int tokenize(const char *text, char toks[][MICROLM_MAX_WORD + 1],
                    int max_toks) {
    int n = 0;
    const char *p = text;
    while (*p && n < max_toks) {
        while (*p && !isalnum((unsigned char)*p) && *p != '\'') p++;
        if (!*p) break;
        int l = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '\'') &&
               l < MICROLM_MAX_WORD) {
            toks[n][l++] = (char)tolower((unsigned char)*p);
            p++;
        }
        toks[n][l] = 0;
        if (l) n++;
    }
    return n;
}

/* binary search in the sorted vocab; -1 when unknown */
static int word_index(const char *w) {
    int lo = 0, hi = MICROLM_NWORDS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(MICROLM_WORDS[mid], w);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* ---------------- classifier ---------------- */

/* log((cnt+1)/(tot+V)) in 1/256 units, precomputed once (deterministic).
 * log here is the classic <math.h> one at init time only. */
static short g_lp[MICROLM_NCLASSES * MICROLM_NWORDS];
static int   g_lp_ready = 0;

static void build_logprobs(void) {
    for (int c = 0; c < MICROLM_NCLASSES; c++) {
        double tot = (double)MICROLM_CLS_TOT[c];
        for (int w = 0; w < MICROLM_NWORDS; w++) {
            double p = ((double)MICROLM_CLS_CNT[c * MICROLM_NWORDS + w] + 1.0) /
                       (tot + (double)MICROLM_NWORDS);
            double lg = log(p) * 256.0;
            if (lg < -32000.0) lg = -32000.0;
            if (lg >  32000.0) lg =  32000.0;
            g_lp[c * MICROLM_NWORDS + w] = (short)lg;
        }
    }
    g_lp_ready = 1;
}

int aios_microlm_ready(void) { return 1; }

const char *aios_microlm_class_name(aios_lm_class c) {
    switch (c) {
    case AIOS_LM_BG:     return "bg";
    case AIOS_LM_URGENT: return "urgent";
    default:             return "normal";
    }
}

aios_lm_class aios_microlm_classify(const char *text, double *conf_out) {
    if (!g_lp_ready) build_logprobs();
    char toks[MICROLM_MAX_TOKENS][MICROLM_MAX_WORD + 1];
    int n = text ? tokenize(text, toks, MICROLM_MAX_TOKENS) : 0;

    long score[MICROLM_NCLASSES] = {0, 0, 0};
    int  hits[MICROLM_NCLASSES]  = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        int w = word_index(toks[i]);
        if (w < 0) continue;
        for (int c = 0; c < MICROLM_NCLASSES; c++) {
            score[c] += g_lp[c * MICROLM_NWORDS + w];
            hits[c]++;
        }
    }
    if (conf_out) {
        /* softmax over the integer scores (double max, exp via expf) */
        long mx = score[0];
        for (int c = 1; c < MICROLM_NCLASSES; c++)
            if (score[c] > mx) mx = score[c];
        double sum = 0.0, vals[MICROLM_NCLASSES];
        for (int c = 0; c < MICROLM_NCLASSES; c++) {
            vals[c] = exp((double)(score[c] - mx) / 256.0);
            sum += vals[c];
        }
        int best = 0;
        for (int c = 1; c < MICROLM_NCLASSES; c++)
            if (score[c] > score[best]) best = c;
        *conf_out = sum > 0.0 ? vals[best] / sum : 0.0;
    }
    int best = 0;
    for (int c = 1; c < MICROLM_NCLASSES; c++)
        if (score[c] > score[best]) best = c;
    return (aios_lm_class)best;
}

/* ---------------- generator ---------------- */

/* LCG: x = x*6364136223846793005ULL + 1442695040888963407ULL */
static uint64_t lcg_next(uint64_t *st) {
    *st = *st * 6364136223846793005ULL + 1442695040888963407ULL;
    return *st >> 33;
}

int aios_microlm_generate(const char *prompt, char *out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (MICROLM_NSTARTS == 0 || MICROLM_NBIGRAMS == 0) return 0;

    uint64_t seed = 0x9E3779B97F4A7C15ULL ^ aios_fnv1a(prompt ? prompt : "",
                                                             prompt ? strlen(prompt) : 0);
    uint64_t st = seed ? seed : 0x123456789ABCDEFULL;
    size_t pos = 0;

    int cur = (int)(lcg_next(&st) % MICROLM_NSTARTS);
    cur = MICROLM_STARTS[cur];

    for (int step = 0; step < 16 && cur != 0xFFFF; step++) {
        const char *w = MICROLM_WORDS[cur];
        size_t wl = strlen(w);
        if (pos + wl + 2 >= cap) break;
        if (pos) { out[pos++] = ' '; }
        memcpy(out + pos, w, wl);
        pos += wl;
        /* collect continuations of `cur` (sorted array — find the range) */
        int lo = 0, hi = MICROLM_NBIGRAMS - 1, first = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int f = (int)MICROLM_BIGRAMS[mid * 2];
            if (f < cur) lo = mid + 1;
            else if (f > cur) hi = mid - 1;
            else { first = mid; hi = mid - 1; }
        }
        if (first < 0) break;
        int last = first;
        while (last + 1 < MICROLM_NBIGRAMS &&
               MICROLM_BIGRAMS[(last + 1) * 2] == cur)
            last++;
        int pick = first + (int)(lcg_next(&st) % (unsigned)(last - first + 1));
        cur = (int)MICROLM_BIGRAMS[pick * 2 + 1];
    }
    if (!pos) return 0;
    out[0] = (char)toupper((unsigned char)out[0]);
    if (pos + 1 < cap) { out[pos++] = '.'; }
    out[pos] = 0;
    return (int)pos;
}

/* ---------------- stats + selftest ---------------- */

int aios_microlm_stats(char *out, size_t cap) {
    if (!out || !cap) return -1;
    return snprintf(out, cap,
        "micro-lm: weights=%s vocab=%d classes=%d (bg/urgent/normal) "
        "starts=%d bigrams=%d (built by tools/microlm_gen.py, deterministic)",
        MICROLM_WEIGHTS_VERSION, (int)MICROLM_NWORDS, (int)MICROLM_NCLASSES,
        (int)MICROLM_NSTARTS, (int)MICROLM_NBIGRAMS);
}

int aios_microlm_selftest(void) {
    static const struct { const char *text; aios_lm_class want; } cases[] = {
        { "run the nightly report in the background at idle priority", AIOS_LM_BG     },
        { "no rush whenever the system is quiet",                      AIOS_LM_BG     },
        { "do the cleanup later overnight",                            AIOS_LM_BG     },
        { "keep that at idle priority please",                         AIOS_LM_BG     },
        { "whenever you get a chance tidy the notes",                  AIOS_LM_BG     },
        { "queue it as background work for tonight",                   AIOS_LM_BG     },
        { "the console is frozen fix it asap",                         AIOS_LM_URGENT },
        { "respond immediately the user is waiting",                   AIOS_LM_URGENT },
        { "this is urgent stop everything now",                        AIOS_LM_URGENT },
        { "the shell is stuck handle it right now",                    AIOS_LM_URGENT },
        { "drop the idle work and answer right now",                   AIOS_LM_URGENT },
        { "hurry the demo starts now",                                 AIOS_LM_URGENT },
        { "summarize the workspace files",                             AIOS_LM_NORMAL },
        { "what files are in the notes folder",                        AIOS_LM_NORMAL },
        { "calculate the token estimate",                              AIOS_LM_NORMAL },
        { "explain how the scheduler treats agents",                   AIOS_LM_NORMAL },
        { "read the metrics and draft a memo",                         AIOS_LM_NORMAL },
        { "show me the kernel registry",                               AIOS_LM_NORMAL },
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        double conf = 0.0;
        aios_lm_class got = aios_microlm_classify(cases[i].text, &conf);
        if (got != cases[i].want || conf < 0.0 || conf > 1.0) fails++;
    }
    /* generator determinism + boundedness */
    char a[256], b[256];
    if (aios_microlm_generate("about the scheduler", a, sizeof a) <= 0) fails++;
    if (aios_microlm_generate("about the scheduler", b, sizeof b) <= 0) fails++;
    if (strcmp(a, b) != 0) fails++;
    if (aios_microlm_generate("different topic entirely", a, sizeof a) <= 0) fails++;
    return fails;
}
