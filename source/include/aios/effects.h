/* aios/effects.h — M4: what a worker ACTUALLY did, compared with what it claimed.
 *
 * The staging layer (M3) is only as good as the claim it was built from. A
 * descriptor says "I will write a.txt"; nothing so far has checked whether the
 * worker wrote a.txt, or also wrote b.txt, which never appeared in a diff
 * because nothing knew to look.
 *
 * The protocol shape, and why:
 *
 *   request  {"tool":..., "args":..., "origin":..., "stage":...}
 *   response {"ok":true, "result":..., "effects":[...], "usage":{...}}
 *
 * `effects` is the worker's own account of what it touched — its testimony, not
 * proof. It is still worth having: a worker that lies here is a worker that has
 * to lie deliberately, and the SAME numbers are what PID 1 will measure
 * independently once the fs is actually mounted read-only and the diff is taken
 * by the kernel (L2). Testimony plus an independent check is a real invariant;
 * testimony alone is a logging convention, and this file says which is which.
 *
 * The comparison has one direction that matters:
 *
 *   declared but not actual  -> suspicious, but not dangerous (nothing happened)
 *   actual but not declared  -> an UNDECLARED EFFECT. This is the failure the
 *                               whole constitution is about, so it fails closed:
 *                               the result is discarded, the staging directory
 *                               is discarded, and the action is denied
 *                               after the fact.
 */
#ifndef AIOS_EFFECTS_H
#define AIOS_EFFECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aios/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_EFFECT_PATH_MAX 512
#define AIOS_EFFECT_MAX      32

/* What the worker says it did. `op` is the verb the descriptor vocabulary uses:
 * "write", "read", "delete", "list". */
typedef struct {
    char   path[AIOS_EFFECT_PATH_MAX];
    char   op[16];
    long   bytes;      /* bytes read or written; 0 for delete/list */
    bool   declared;   /* filled in by aios_effects_compare() */
} aios_effect;

typedef struct {
    aios_effect items[AIOS_EFFECT_MAX];
    size_t      n;
    bool        complete;   /* 1 = the worker enumerated everything it did    */
    bool        present;    /* 0 = the response carried no effects document   */
} aios_effects;

typedef struct {
    long ms;            /* wall clock inside the worker                    */
    long bytes_read;
    long bytes_written;
} aios_usage;

typedef enum {
    EFFECTS_OK                = 0,
    EFFECTS_UNDECLARED        = 1,   /* the worker did more than it said        */
    EFFECTS_INCOMPLETE        = 2,   /* it says it cannot fully account for it  */
    EFFECTS_MISSING           = 3,   /* no document at all                      */
    EFFECTS_MALFORMED         = 4,   /* a path that could not be trusted        */
    EFFECTS_UNDER_DELIVERED   = 5    /* declared work that did not happen       */
} aios_effects_verdict;

const char *aios_effects_verdict_str(aios_effects_verdict v);

/* Parse the `effects` and `usage` members of a worker response. Tolerant of
 * their absence (`present = false`); strict about their contents. Returns false
 * with err set if a document is present but malformed. */
bool aios_effects_from_json(const json_val *resp, aios_effects *out,
                            aios_usage *usage, char *err, size_t ecap);

/* The same parse, from the two members the tool layer already separated. The
 * shell holds the array and the flag, not an envelope, and rebuilding an
 * envelope just to take it apart again would be a place for the two halves to
 * disagree. */
bool aios_effects_from_array(const json_val *effects_arr, int complete,
                             aios_effects *out, char *err, size_t ecap);

/* Compare a worker's testimony against a descriptor's claim.
 *
 * `declared[]` is the action's effect list in "verb:path" form (the same strings
 * aios_action_add_effect accepts). An actual effect is "declared" when its path
 * matches a declared path whose verb permits it — a READ of a path does not
 * satisfy a declaration to WRITE it, because the two are different promises.
 *
 * Returns EFFECTS_UNDECLARED if anything happened that was not claimed, and
 * fills `first`/`first_cap` with the offending path so the audit entry can name
 * it. This function is pure: no I/O, no clock, no allocation. */
aios_effects_verdict aios_effects_compare(aios_effects *actual,
                                          const char *const *declared,
                                          int n_declared,
                                          char *first, size_t first_cap);

/* The check that does not rely on the worker's honesty.
 *
 * The worker's account can be forged; the tree it left behind cannot. Given the
 * declared writes, and which of those paths the caller found in the JAIL and in
 * the staging upper/, this reports false if anything that should have been
 * staged landed in the jail instead. `first` receives the offending path.
 *
 * Pure, like the comparison it mirrors: the caller does the stat() calls, which
 * keeps this testable without a filesystem and keeps the trust decision free of
 * I/O. */
bool aios_effects_mirror(const char *const *declared, int n_declared,
                         const char *const *in_jail, const char *const *in_upper,
                         int n_paths, char *first, size_t first_cap);

/* Where the effect should have happened, given an optional staging dir.
 * A staged write's real path is <stage>/upper/<rel>; the comparison is done on
 * the jail-relative path, so this strips the prefix. Returns false if the path
 * is not inside `stage` at all. */
bool aios_effects_to_rel(const char *actual_path, const char *stage_dir,
                         char *out, size_t ocap);

/* Render an effects document as a one-line summary ("3 effects: 2 write, 1
 * read") and, when there were undeclared ones, name them. Caller frees. */
char *aios_effects_text(const aios_effects *actual, char *first_undeclared);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_EFFECTS_H */
