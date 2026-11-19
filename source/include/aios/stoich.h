/* aios/stoich.h — M10: a conservation-law firewall for agent chains.
 *
 * WHAT THIS IS
 *
 * An agent chain is a sequence of tool calls. This file makes each tool call a
 * REACTION in a chemical reaction network: it consumes quantities of species and
 * produces quantities of other species. Money moves from checking to savings; a
 * file's bytes move from the jail to a trash area; a capability is spent and a
 * permission is produced.
 *
 * Given that, the classic structural results apply, and they are *checkable*:
 *
 *   - the CONSERVATION LAWS of the network are the integer vectors λ with
 *     λ·Δⱼ = 0 for every reaction j. Value cannot leave the system unless a
 *     reaction says it does. A tool call that destroys value — deletes a file,
 *     wires money out — is a reaction with a non-zero λ·Δ, and is refused.
 *
 *   - the DEFICIENCY δ = n − l − s (complexes, linkage classes, rank) tells you
 *     how constrained the network is. Feinberg's deficiency-zero theorem is the
 *     reason this is worth computing at all: for δ = 0 the network's long-run
 *     behaviour is pinned to its structure, so the structure is a real invariant
 *     rather than a heuristic. We compute δ and weak reversibility and report
 *     them; we do NOT integrate the dynamics. See "what is not here" below.
 *
 *   - the AUTHORIZED ENVELOPE is a box in species space (a min and max per
 *     species). A chain walks a path through that space. Each step may be
 *     individually legal and the chain may still end somewhere nobody agreed to,
 *     so the envelope is checked on the cumulative state, not per call.
 *
 * WHAT THIS IS NOT — read this before the code
 *
 * 1. There is no neural network here, and there should not be one *in this role*.
 *    The proposal this implements asks for a "learned neural conservation
 *    critic". A learned critic is a heuristic: it cannot be shown to hold, it
 *    drifts with its training distribution, and it fails silently on inputs
 *    outside that distribution. A thing that cannot be shown to hold is not a
 *    conservation law, and calling it one is the exact substitution this project
 *    keeps refusing to make. What is here instead is deterministic, exact
 *    (integer arithmetic, no floating point), and every refusal can be explained
 *    as a specific vector arithmetic. If a learned critic is ever added, it
 *    belongs ADJACENT to this: it may propose a species mapping, and the exact
 *    check still decides. It must never be the thing that decides.
 *
 * 2. The species mapping is AUTHORED, not inferred. Someone has to say that
 *    "usd:external" is a species and that a wire is a reaction into it. The
 *    mathematics is only as honest as that mapping, so the mapping is data you
 *    can read, review and diff — not weights you cannot.
 *
 * 3. Conservation does not imply authorisation. A chain can conserve perfectly
 *    and still be an attack (move all the money between two accounts you own).
 *    That is what the envelope and the forbidden-state check are for, and why
 *    the policy layer (M2) still runs. This layer is a second, structural opinion
 *    — not a replacement.
 *
 * 4. Values are long long. They are exact for counts and integer cents. If you
 *    feed it floating-point money you have already lost the property.
 */
#ifndef AIOS_STOICH_H
#define AIOS_STOICH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_STOICH_NAME_MAX     56
#define AIOS_STOICH_MAX_SPECIES  16
#define AIOS_STOICH_MAX_REACTIONS 16
#define AIOS_STOICH_MAX_TERMS    8
#define AIOS_STOICH_MAX_LAWS     16

/* One term of a reaction: species `species` changes by `delta`
 * (negative = consumed, positive = produced). Sparse on purpose: a reaction that
 * touches one species should not carry an array of zeroes into every check. */
typedef struct {
    int       species;
    long long delta;
} stoich_term;

typedef struct {
    char        name[AIOS_STOICH_NAME_MAX];
    stoich_term terms[AIOS_STOICH_MAX_TERMS];
    int         n_terms;
} stoich_reaction;

typedef struct {
    char      name[AIOS_STOICH_NAME_MAX];
    long long initial;      /* the state a chain starts from            */
    long long lo;           /* authorized envelope, inclusive           */
    long long hi;
} stoich_species;

typedef struct {
    stoich_species  species[AIOS_STOICH_MAX_SPECIES];
    int             n_species;
    stoich_reaction reaction[AIOS_STOICH_MAX_REACTIONS];
    int             n_reactions;
} stoich_net;

/* A conservation law: for every reaction j, Σ_i coeff[i]·Δᵢ(j) = 0.
 *
 * The coefficients are the SMALLEST integers with that property and the first
 * non-zero one positive, so two nets that have the same law print the same
 * string — a law you cannot compare is a law you cannot review. */
typedef struct {
    long long coeff[AIOS_STOICH_MAX_SPECIES];
    int       n_species;    /* coeff[] is meaningful for indices < this */
} stoich_law;

typedef struct {
    stoich_law laws[AIOS_STOICH_MAX_LAWS];
    int        n;
} stoich_laws;

/* The structural report from reaction network theory. */
typedef struct {
    int  n_complexes;       /* distinct left/right-hand sides over all reactions */
    int  n_linkage;         /* connected components of the reaction graph        */
    int  rank;              /* dim of the stoichiometric subspace = rank(A)      */
    int  deficiency;        /* δ = n − l − s                                     */
    bool weakly_reversible; /* every reaction has a path back to its source      */
    bool zero_deficient;    /* deficiency == 0                                   */
} stoich_crn;

/* ---- building a network ---- */

void stoich_init(stoich_net *net);

/* Returns the species index, or -1 (table full, duplicate name, bad args).
 * Callers that ignore -1 get no silent aliasing: adding a reaction against an
 * unknown species fails, it does not create one. */
int  stoich_species_add(stoich_net *net, const char *name, long long initial);
int  stoich_species_index(const stoich_net *net, const char *name);

/* Envelope: the region a chain is authorised to move within. Defaults to the
 * initial value (so an unspecified species may not move at all), because
 * "authorised by default" is how every one of these systems gets owned. */
void stoich_bounds(stoich_net *net, int species, long long lo, long long hi);

/* Add a reaction from up to 8 signed terms. Negative deltas are consumed
 * quantities, positive produced. Returns the reaction index, or -1. */
int  stoich_reaction_add(stoich_net *net, const char *name,
                         const stoich_term *terms, int n_terms);

/* ---- the mathematics ---- */

/* Every conservation law of the network, as an integer basis of the left null
 * space of the stoichiometry matrix. Returns the number found (0 is a legal
 * answer: a network where anything can become anything conserves nothing).
 *
 * Exact integer arithmetic throughout: a rational basis is scaled to a lcm, then
 * divided by its gcd, so the output is canonical rather than merely correct. */
int  stoich_laws_compute(const stoich_net *net, stoich_laws *out);

/* δ = n − l − s, weak reversibility, and the complex/linkage counts. */
bool stoich_crn_analyse(const stoich_net *net, stoich_crn *out);

/* Render a law as "usd:checking + usd:savings + usd:external" for review. */
void stoich_law_str(const stoich_net *net, const stoich_law *law,
                    char *out, size_t cap);

/* ---- applying a chain ---- */

typedef struct {
    long long value[AIOS_STOICH_MAX_SPECIES];
    int       n_species;
} stoich_state;

typedef enum {
    STOICH_OK = 0,
    STOICH_ERR_LAW = 1,        /* a conservation law does not hold             */
    STOICH_ERR_BOUNDS = 2,     /* the cumulative state left the envelope       */
    STOICH_ERR_UNKNOWN = 3,    /* a reaction name the net does not have       */
    STOICH_ERR_ARG = 4,
    STOICH_ERR_OVERFLOW = 5
} stoich_verdict;

const char *stoich_verdict_str(stoich_verdict v);

/* Start a chain from the net's initial values. */
void stoich_state_init(const stoich_net *net, stoich_state *st);

/* Apply ONE reaction, checking:
 *   1. every conservation law still holds, and
 *   2. the resulting state is inside the envelope.
 * On refusal `st` is left EXACTLY as it was — a chain that half-applies a
 * rejected step is worse than one that stops, because the caller's state then
 * no longer describes what it thinks it has done.
 *
 * `which` receives the index of the law or species responsible, and `detail`
 * a sentence naming it, so a refusal can be argued about rather than just
 * obeyed. */
stoich_verdict stoich_step(const stoich_net *net, const stoich_laws *laws,
                           int reaction, stoich_state *st,
                           int *which, char *detail, size_t dcap);

/* The whole chain at once, stopping at the first refusal. `applied` reports how
 * many steps really ran, so "refused at step 3 of 5" is always recoverable. */
stoich_verdict stoich_chain(const stoich_net *net, const stoich_laws *laws,
                            const int *reactions, int n,
                            stoich_state *st, int *applied,
                            int *which, char *detail, size_t dcap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_STOICH_H */
