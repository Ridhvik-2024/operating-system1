/* aios/stoich.c — M10: exact conservation analysis over a reaction network.
 *
 * Everything here is integer arithmetic. That is a design commitment, not a
 * convenience:
 *
 *   - a conservation law computed in floating point is a law plus an error bar,
 *     and the error bar is chosen by the inputs. A chain that exploits the
 *     difference between 0.1+0.2 and 0.3 is a real attack, and it stops being
 *     one the moment the arithmetic cannot represent 0.1.
 *   - the left null space is computed by fraction-free elimination and returned
 *     as the SMALLEST integer vector, so the answer is canonical. Two runs, two
 *     machines and two review passes see the same string.
 *
 * The cost is overflow. We bound it: species ≤ 16, terms ≤ 8, and deltas are
 * small integers authored by a human. The elimination therefore stays inside
 * int64 for any network a person would actually write, and `mul_ok`/`add_ok`
 * refuse rather than wrap when someone tries to exceed it.
 */
#include "aios/stoich.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small exact helpers -------------------------------------------- */

static long long gcd_ll(long long a, long long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long long t = a % b; a = b; b = t; }
    return a;
}


/* Overflow-checked multiply. Returns false instead of wrapping: a wrapped
 * coefficient is a conservation law computed in a different arithmetic than the
 * one the caller believes, which is the same class of bug as a float. */
static bool mul_ok(long long a, long long b, long long *out) {
    if (a == 0 || b == 0) { *out = 0; return true; }
    long long p = a * b;
    if (p / b != a) return false;
    *out = p;
    return true;
}

static bool add_ok(long long a, long long b, long long *out) {
    long long s = a + b;
    if ((a > 0 && b > 0 && s < 0) || (a < 0 && b < 0 && s >= 0)) return false;
    *out = s;
    return true;
}

/* ---- building ------------------------------------------------------- */

void stoich_init(stoich_net *net) {
    if (net) memset(net, 0, sizeof *net);
}

int stoich_species_index(const stoich_net *net, const char *name) {
    if (!net || !name) return -1;
    for (int i = 0; i < net->n_species; i++)
        if (strcmp(net->species[i].name, name) == 0) return i;
    return -1;
}

int stoich_species_add(stoich_net *net, const char *name, long long initial) {
    if (!net || !name || !*name) return -1;
    if (strlen(name) >= AIOS_STOICH_NAME_MAX) return -1;
    int existing = stoich_species_index(net, name);
    if (existing >= 0) return existing;          /* idempotent, not an error */
    if (net->n_species >= AIOS_STOICH_MAX_SPECIES) return -1;
    int i = net->n_species++;
    snprintf(net->species[i].name, sizeof net->species[i].name, "%s", name);
    net->species[i].initial = initial;
    /* Default envelope: may not move. A species whose envelope nobody set is a
     * species nobody authorised, and the failure mode of the opposite default is
     * a firewall that permits everything it was not told about. */
    net->species[i].lo = initial;
    net->species[i].hi = initial;
    return i;
}

void stoich_bounds(stoich_net *net, int species, long long lo, long long hi) {
    if (!net || species < 0 || species >= net->n_species) return;
    if (lo > hi) { long long t = lo; lo = hi; hi = t; }
    net->species[species].lo = lo;
    net->species[species].hi = hi;
}

int stoich_reaction_add(stoich_net *net, const char *name,
                        const stoich_term *terms, int n_terms) {
    if (!net || !name || !*name) return -1;
    if (strlen(name) >= AIOS_STOICH_NAME_MAX) return -1;
    if (n_terms < 0 || n_terms > AIOS_STOICH_MAX_TERMS) return -1;
    if (n_terms > 0 && !terms) return -1;
    if (net->n_reactions >= AIOS_STOICH_MAX_REACTIONS) return -1;

    /* Validate every term BEFORE mutating: a half-added reaction is a network
     * whose mathematics silently differs from the one that was described. */
    for (int k = 0; k < n_terms; k++) {
        if (terms[k].species < 0 || terms[k].species >= net->n_species) return -1;
        if (terms[k].delta == 0) return -1;      /* a no-op is a modelling bug */
        for (int j = 0; j < k; j++)
            if (terms[j].species == terms[k].species) return -1;  /* not sparse */
    }

    int r = net->n_reactions++;
    stoich_reaction *rx = &net->reaction[r];
    snprintf(rx->name, sizeof rx->name, "%s", name);
    rx->n_terms = n_terms;
    for (int k = 0; k < n_terms; k++) rx->terms[k] = terms[k];
    return r;
}

/* ---- conservation laws: exact left null space of the stoichiometry matrix -- */

/* Fraction-free (Bareiss-style) elimination on an (n_species × n_reactions)
 * matrix, tracking the row operations that build the left null space.
 *
 * We keep a square transform T initialised to the identity and apply the SAME
 * row operations to T as to the matrix. At the end, the rows of T below the
 * rank are the left null space: they annihilate every column by construction.
 * Working over integers with exact division keeps everything canonical, and
 * every step is checked for overflow.
 */
int stoich_laws_compute(const stoich_net *net, stoich_laws *out) {
    if (!net || !out) return 0;
    memset(out, 0, sizeof *out);

    const int rows = net->n_species, cols = net->n_reactions;
    if (rows <= 0) return 0;
    if (cols <= 0) {
        /* No reactions: nothing is constrained, so EVERY species is its own
         * law. That is the correct reading — a network that cannot change
         * conserves every species trivially. */
        for (int i = 0; i < rows && out->n < AIOS_STOICH_MAX_LAWS; i++) {
            stoich_law *l = &out->laws[out->n++];
            l->coeff[i] = 1;
            l->n_species = rows;
        }
        return out->n;
    }

    long long A[AIOS_STOICH_MAX_SPECIES][AIOS_STOICH_MAX_REACTIONS];
    long long T[AIOS_STOICH_MAX_SPECIES][AIOS_STOICH_MAX_SPECIES];
    memset(A, 0, sizeof A);
    memset(T, 0, sizeof T);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            for (int k = 0; k < net->reaction[c].n_terms; k++) {
                if (net->reaction[c].terms[k].species == r) {
                    A[r][c] = net->reaction[c].terms[k].delta;
                    break;
                }
            }
        }
        T[r][r] = 1;
    }

    int rank = 0;
    for (int c = 0; c < cols && rank < rows; c++) {
        int piv = -1;
        for (int r = rank; r < rows; r++)
            if (A[r][c] != 0) { piv = r; break; }
        if (piv < 0) continue;

        if (piv != rank) {
            for (int j = 0; j < cols; j++) {
                long long t = A[rank][j]; A[rank][j] = A[piv][j]; A[piv][j] = t;
            }
            for (int j = 0; j < rows; j++) {
                long long t = T[rank][j]; T[rank][j] = T[piv][j]; T[piv][j] = t;
            }
        }

        for (int r = rank + 1; r < rows; r++) {
            if (A[r][c] == 0) continue;
            long long a = A[rank][c], b = A[r][c];
            long long g = gcd_ll(a, b);
            long long tmp;
            if (g == 0) continue;
            long long f1 = b / g, f2 = a / g;   /* make the c-th entry cancel */
            for (int j = 0; j < cols; j++) {
                long long x, y;
                if (!mul_ok(f2, A[rank][j], &x)) return 0;
                if (!mul_ok(f1, A[r][j], &y)) return 0;
                if (!add_ok(x, -y, &tmp)) return 0;
                A[r][j] = tmp;
            }
            for (int j = 0; j < rows; j++) {
                long long x, y;
                if (!mul_ok(f2, T[rank][j], &x)) return 0;
                if (!mul_ok(f1, T[r][j], &y)) return 0;
                if (!add_ok(x, -y, &tmp)) return 0;
                T[r][j] = tmp;
            }
        }
        rank++;
    }

    /* Rows rank..rows-1 of T annihilate every reaction column. Make each one
     * canonical: divide by the gcd, first non-zero positive. */
    for (int r = rank; r < rows && out->n < AIOS_STOICH_MAX_LAWS; r++) {
        long long g = 0;
        for (int j = 0; j < rows; j++) if (T[r][j]) g = gcd_ll(g, T[r][j]);
        if (g == 0) continue;                       /* a zero row is not a law */
        stoich_law *l = &out->laws[out->n++];
        l->n_species = rows;
        int first_nonzero = -1;
        for (int j = 0; j < rows; j++) {
            l->coeff[j] = T[r][j] / g;
            if (l->coeff[j] && first_nonzero < 0) first_nonzero = j;
        }
        if (first_nonzero >= 0 && l->coeff[first_nonzero] < 0)
            for (int j = 0; j < rows; j++) l->coeff[j] = -l->coeff[j];
    }
    return out->n;
}

/* ---- deficiency-zero structural analysis ---------------------------- */

/* A COMPLEX is a whole vector over species: everything on one side of a
 * reaction. A -> B has TWO complexes, {A:1} and {B:1}, and the reaction runs
 * from the first to the second. Two sides are the same complex only when every
 * species' amount matches — which is why this compares full vectors and not
 * "the consumed side".
 *
 * The first version of this compared consumed sides for sources and produced
 * sides for targets, so A -> B and B -> A never recognised each other's sides and
 * the network reported 2 linkage classes and a deficiency of -1. A negative
 * deficiency is impossible, which is how it was caught: the arithmetic refuses
 * to lie even when the code does. */
static void complex_of(const stoich_reaction *rx, int sign,
                       long long out[AIOS_STOICH_MAX_SPECIES]) {
    (void)0;
    memset(out, 0, sizeof(long long) * AIOS_STOICH_MAX_SPECIES);
    for (int k = 0; k < rx->n_terms; k++) {
        const stoich_term *t = &rx->terms[k];
        long long d = t->delta;
        if ((sign < 0 && d > 0) || (sign > 0 && d < 0)) continue;
        out[t->species] = d < 0 ? -d : d;
    }
}

static bool complex_equal(const long long *a, const long long *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

/* Find (or intern) a complex, returning its index. Complexes are stored as full
 * vectors so that a side can be compared with any other side. */
static int complex_intern(long long table[][AIOS_STOICH_MAX_SPECIES],
                          int *count, int cap, int n_species,
                          const long long *v) {
    for (int i = 0; i < *count; i++)
        if (complex_equal(table[i], v, n_species)) return i;
    if (*count >= cap) return -1;
    memcpy(table[*count], v, sizeof(long long) * AIOS_STOICH_MAX_SPECIES);
    return (*count)++;
}

bool stoich_crn_analyse(const stoich_net *net, stoich_crn *out) {
    if (!net || !out) return false;
    memset(out, 0, sizeof *out);
    const int nr = net->n_reactions;
    if (nr <= 0) {
        out->n_complexes = 0;
        out->n_linkage = 0;
        out->rank = 0;
        out->deficiency = 0;
        out->weakly_reversible = true;
        out->zero_deficient = true;
        return true;
    }

    /* Complexes: each reaction contributes a source and a target vector, and
     * identical vectors are interned to one complex. */
    static long long table[AIOS_STOICH_MAX_REACTIONS * 2][AIOS_STOICH_MAX_SPECIES];
    int n_complexes = 0;
    int complex_of_source[AIOS_STOICH_MAX_REACTIONS];
    int complex_of_target[AIOS_STOICH_MAX_REACTIONS];
    const int cap = AIOS_STOICH_MAX_REACTIONS * 2;
    long long v[AIOS_STOICH_MAX_SPECIES];

    for (int r = 0; r < nr; r++) {
        const stoich_reaction *rx = &net->reaction[r];
        complex_of(rx, -1, v);
        int si = complex_intern(table, &n_complexes, cap, net->n_species, v);
        if (si < 0) return false;
        complex_of(rx, +1, v);
        int ti = complex_intern(table, &n_complexes, cap, net->n_species, v);
        if (ti < 0) return false;
        complex_of_source[r] = si;
        complex_of_target[r] = ti;
    }
    out->n_complexes = n_complexes;

    /* Linkage classes: connected components of the undirected graph whose
     * vertices are complexes and whose edges are reactions. */
    int parent[AIOS_STOICH_MAX_REACTIONS * 2];
    for (int i = 0; i < n_complexes; i++) parent[i] = i;
    for (int r = 0; r < nr; r++) {
        int ra = complex_of_source[r], rb = complex_of_target[r];
        while (parent[ra] != ra) ra = parent[ra];
        while (parent[rb] != rb) rb = parent[rb];
        if (ra != rb) parent[ra] = rb;
    }
    int n_link = 0;
    for (int i = 0; i < n_complexes; i++)
        if (parent[i] == i) n_link++;
    out->n_linkage = n_link;

    /* Rank of the stoichiometry matrix, by exact integer elimination on a copy.
     * Reusing the law computation would give the nullity; we want the rank, and
     * stating it directly keeps the two reports independent. */
    if (net->n_species > 0) {
        long long A[AIOS_STOICH_MAX_SPECIES][AIOS_STOICH_MAX_REACTIONS];
        memset(A, 0, sizeof A);
        for (int c = 0; c < nr; c++)
            for (int k = 0; k < net->reaction[c].n_terms; k++)
                A[net->reaction[c].terms[k].species][c] =
                    net->reaction[c].terms[k].delta;

        int rank = 0;
        for (int c = 0; c < nr && rank < net->n_species; c++) {
            int piv = -1;
            for (int r = rank; r < net->n_species; r++)
                if (A[r][c] != 0) { piv = r; break; }
            if (piv < 0) continue;
            if (piv != rank)
                for (int j = 0; j < nr; j++) {
                    long long t = A[rank][j]; A[rank][j] = A[piv][j]; A[piv][j] = t;
                }
            for (int r = rank + 1; r < net->n_species; r++) {
                if (A[r][c] == 0) continue;
                long long a = A[rank][c], b = A[r][c];
                long long g = gcd_ll(a, b);
                if (!g) continue;
                long long f1 = b / g, f2 = a / g;
                for (int j = 0; j < nr; j++) {
                    long long x, y;
                    if (!mul_ok(f2, A[rank][j], &x)) break;
                    if (!mul_ok(f1, A[r][j], &y)) break;
                    long long d;
                    if (!add_ok(x, -y, &d)) break;
                    A[r][j] = d;
                }
            }
            rank++;
        }
        out->rank = rank;
    }
    out->deficiency = out->n_complexes - out->n_linkage - out->rank;
    out->zero_deficient = (out->deficiency == 0);

    /* Weak reversibility: every reaction lies on a cycle of the complex graph.
     * Checked as reachability from each target back to each source, which is
     * O(n²) on a graph of at most 32 vertices — cheap enough to be exact rather
     * than approximate. */
    bool reach[AIOS_STOICH_MAX_REACTIONS * 2][AIOS_STOICH_MAX_REACTIONS * 2];
    memset(reach, 0, sizeof reach);
    for (int i = 0; i < n_complexes; i++) reach[i][i] = true;
    for (int r = 0; r < nr; r++)
        reach[complex_of_source[r]][complex_of_target[r]] = true;
    for (int k = 0; k < n_complexes; k++)
        for (int i = 0; i < n_complexes; i++)
            if (reach[i][k])
                for (int j = 0; j < n_complexes; j++)
                    if (reach[k][j]) reach[i][j] = true;
    bool wr = true;
    for (int r = 0; r < nr && wr; r++)
        if (!reach[complex_of_target[r]][complex_of_source[r]]) wr = false;
    out->weakly_reversible = wr;
    return true;
}

void stoich_law_str(const stoich_net *net, const stoich_law *law,
                    char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!net || !law) return;
    size_t used = 0;
    for (int i = 0; i < law->n_species && i < net->n_species; i++) {
        long long c = law->coeff[i];
        if (!c) continue;
        const char *sign = c < 0 ? " - " : used ? " + " : "";
        long long ac = c < 0 ? -c : c;
        int n;
        if (ac == 1)
            n = snprintf(out + used, cap - used, "%s%s", sign,
                         net->species[i].name);
        else
            n = snprintf(out + used, cap - used, "%s%lld %s", sign, ac,
                         net->species[i].name);
        if (n < 0 || (size_t)n >= cap - used) { out[cap - 1] = 0; return; }
        used += (size_t)n;
    }
    if (!used) snprintf(out, cap, "(no terms)");
}

/* ---- applying a chain ----------------------------------------------- */

const char *stoich_verdict_str(stoich_verdict v) {
    switch (v) {
    case STOICH_OK:          return "ok";
    case STOICH_ERR_LAW:     return "a conservation law does not hold";
    case STOICH_ERR_BOUNDS:  return "the state left the authorized envelope";
    case STOICH_ERR_UNKNOWN: return "no such reaction";
    case STOICH_ERR_ARG:     return "invalid argument";
    case STOICH_ERR_OVERFLOW:return "arithmetic overflow";
    }
    return "unknown";
}

void stoich_state_init(const stoich_net *net, stoich_state *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    if (!net) return;
    st->n_species = net->n_species;
    for (int i = 0; i < net->n_species; i++) st->value[i] = net->species[i].initial;
}

stoich_verdict stoich_step(const stoich_net *net, const stoich_laws *laws,
                           int reaction, stoich_state *st,
                           int *which, char *detail, size_t dcap) {
    if (!net || !laws || !st) return STOICH_ERR_ARG;
    if (reaction < 0 || reaction >= net->n_reactions) return STOICH_ERR_UNKNOWN;
    if (which) *which = -1;
    if (detail && dcap) detail[0] = 0;

    /* Apply to a COPY, then commit. A refusal must leave the caller's state
     * exactly as it was: otherwise a caller that catches the error and continues
     * is reasoning about a state that never existed. */
    long long next[AIOS_STOICH_MAX_SPECIES];
    memcpy(next, st->value, sizeof next);
    for (int k = 0; k < net->reaction[reaction].n_terms; k++) {
        const stoich_term *t = &net->reaction[reaction].terms[k];
        long long sum;
        if (!add_ok(next[t->species], t->delta, &sum)) return STOICH_ERR_OVERFLOW;
        next[t->species] = sum;
    }

    /* 1. Every law must still hold.
     *
     * A law is a weighted sum of species that no reaction can change, so the
     * check is on the SUM before and after — not on each species. Checking
     * species individually would be a different (much stronger, and wrong)
     * property: it would forbid any reaction from moving through a species at
     * all. */
    for (int l = 0; l < laws->n; l++) {
        const stoich_law *law = &laws->laws[l];
        long long before = 0, after = 0, x, y, tmp;
        for (int i = 0; i < law->n_species && i < net->n_species; i++) {
            if (!law->coeff[i]) continue;
            if (!mul_ok(law->coeff[i], st->value[i], &x)) return STOICH_ERR_OVERFLOW;
            if (!add_ok(before, x, &tmp)) return STOICH_ERR_OVERFLOW;
            before = tmp;
            if (!mul_ok(law->coeff[i], next[i], &y)) return STOICH_ERR_OVERFLOW;
            if (!add_ok(after, y, &tmp)) return STOICH_ERR_OVERFLOW;
            after = tmp;
        }
        if (before != after) {
            if (which) *which = l;
            if (detail && dcap) {
                char ls[400];
                stoich_law_str(net, law, ls, sizeof ls);
                snprintf(detail, dcap,
                         "reaction '%s' changes the conserved quantity [%s]: "
                         "%lld -> %lld (it must stay %lld). Something is being "
                         "created or destroyed that no reaction accounts for.",
                         net->reaction[reaction].name, ls, before, after, before);
            }
            return STOICH_ERR_LAW;
        }
    }

    /* 2. The envelope, on the CUMULATIVE state.
     *
     * This is why the check is a chain check: each step can be legal while the
     * chain walks somewhere nobody authorised. A box boundary is reached by a
     * sequence of individually small moves, which is exactly the shape of the
     * multi-step attack this layer exists for. */
    for (int i = 0; i < net->n_species; i++) {
        if (next[i] < net->species[i].lo || next[i] > net->species[i].hi) {
            if (which) *which = i;
            if (detail && dcap)
                snprintf(detail, dcap,
                         "after reaction '%s', %s would be %lld, outside the "
                         "authorized envelope [%lld, %lld]",
                         net->reaction[reaction].name, net->species[i].name,
                         next[i], net->species[i].lo, net->species[i].hi);
            return STOICH_ERR_BOUNDS;
        }
    }

    memcpy(st->value, next, sizeof next);
    st->n_species = net->n_species;
    return STOICH_OK;
}

stoich_verdict stoich_chain(const stoich_net *net, const stoich_laws *laws,
                            const int *reactions, int n,
                            stoich_state *st, int *applied,
                            int *which, char *detail, size_t dcap) {
    if (!net || !laws || !st) return STOICH_ERR_ARG;
    if (applied) *applied = 0;
    if (n < 0 || (n > 0 && !reactions)) return STOICH_ERR_ARG;

    for (int i = 0; i < n; i++) {
        char step_detail[512] = {0};
        int step_which = -1;
        stoich_verdict v = stoich_step(net, laws, reactions[i], st,
                                       &step_which, step_detail, sizeof step_detail);
        if (v != STOICH_OK) {
            if (which) *which = step_which;
            if (detail && dcap) {
                /* Say WHERE in the chain, always. "Refused" without a step
                 * number is unarguable, and a refusal nobody can argue with is
                 * indistinguishable from a bug. */
                snprintf(detail, dcap,
                         "step %d of %d: %s", i + 1, n, step_detail);
            }
            return v;
        }
        if (applied) *applied = i + 1;
    }
    return STOICH_OK;
}
