/* aios/microlm.h — M7: the AI-OS micro language model.
 *
 * The OS ships with its own very small language model, trained at build
 * time on an embedded corpus (tools/microlm_gen.py -> microlm_weights.h):
 *
 *   1. an intent classifier  — maps a natural-language request to a
 *      scheduling class so the OS can route work by MEANING:
 *        bg      "run this in the background at idle priority"
 *        urgent  "the console is frozen, fix it asap"
 *        normal  everything else
 *   2. a sentence generator  — produces short OS-flavoured prose for
 *      status lines and demos (bigram LM over the OS voice corpus).
 *
 * Deliberately tiny (a few hundred parameters, ~12 KB of weights):
 * deterministic, zero network, zero runtime training. It exists so the
 * booted OS speaks AI-natively out of the box; bigger LLMs plug into the
 * same seam later (cortex llm / the z-ai bridge) without changing the OS
 * interfaces.
 */
#ifndef AIOS_MICROLM_H
#define AIOS_MICROLM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIOS_LM_BG = 0,     /* background / idle-priority work               */
    AIOS_LM_URGENT = 1, /* interactive, do-it-now work                   */
    AIOS_LM_NORMAL = 2, /* default class                                 */
} aios_lm_class;

/* 1 when the trained weights are linked in (always, currently). */
int aios_microlm_ready(void);

const char *aios_microlm_class_name(aios_lm_class c);

/* Classify one line of natural language. conf_out (may be NULL) receives
 * the winner's softmax confidence in [0,1]. Deterministic. */
aios_lm_class aios_microlm_classify(const char *text, double *conf_out);

/* Generate one short sentence (the OS voice) seeded by the prompt.
 * Deterministic: identical prompt -> identical sentence. Returns the
 * number of bytes written (0 on failure). Output is NUL-terminated. */
int aios_microlm_generate(const char *prompt, char *out, size_t cap);

/* Human-readable model summary ("micro-lm: weights=m7.1 vocab=293 ..."). */
int aios_microlm_stats(char *out, size_t cap);

/* Built-in acceptance check: every baked phrase must land in its class.
 * Returns the number of failures (0 = pass). */
int aios_microlm_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_MICROLM_H */
