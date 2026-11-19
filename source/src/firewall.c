/* aios/firewall.c — M10: the authored half. Species, reactions, envelopes. */
#include "aios/firewall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fw_verdict_str(fw_verdict v) {
    switch (v) {
    case FW_ALLOW:           return "allow";
    case FW_DENY_LAW:        return "deny: conservation law broken";
    case FW_DENY_ENVELOPE:   return "deny: outside the authorized envelope";
    case FW_DENY_FORBIDDEN:  return "deny: grew a forbidden quantity";
    case FW_DENY_MODEL:      return "deny: the model itself is unsound";
    case FW_DENY_ARG:        return "deny: bad argument";
    }
    return "deny: unknown";
}

void fw_authorize(fw_model *m, int species, long long lo, long long hi) {
    if (!m) return;
    stoich_bounds(&m->net, species, lo, hi);
}

void fw_forbid_growth(fw_model *m, int species) {
    if (!m || species < 0 || species >= m->net.n_species) return;
    m->forbidden[species] = true;
}

static bool add_rx(stoich_net *net, const char *name, int species_a,
                   long long da, int species_b, long long db) {
    stoich_term t[2];
    int n = 0;
    t[n].species = species_a; t[n].delta = da; n++;
    if (species_b >= 0) { t[n].species = species_b; t[n].delta = db; n++; }
    return stoich_reaction_add(net, name, t, n) >= 0;
}

/* Does this law set weight all four file species equally? That is exactly "bytes
 * are neither created nor destroyed", and it is the property the firewall's file
 * protections rest on. Exposed so the constructor can enforce it and a test can
 * feed it a network that should fail.
 *
 * It exists because of a real bug: a `backup` declared as "+1 backup_bytes" with
 * nothing consumed destroyed this law silently. Two OTHER laws survived, so a
 * check as weak as "the model has some laws" would not have caught it. */
bool fw_laws_conserve_bytes(const stoich_net *net, const stoich_laws *laws) {
    if (!net || !laws) return false;
    int j = stoich_species_index(net, "jail_bytes");
    int t = stoich_species_index(net, "trash_bytes");
    int g = stoich_species_index(net, "destroyed_bytes");
    int b = stoich_species_index(net, "backup_bytes");
    if (j < 0 || t < 0 || g < 0 || b < 0) return false;
    for (int l = 0; l < laws->n; l++) {
        const stoich_law *law = &laws->laws[l];
        long long c = law->coeff[j];
        if (c && law->coeff[t] == c && law->coeff[g] == c && law->coeff[b] == c)
            return true;
    }
    return false;
}

bool fw_model_default(fw_model *m, long long jail_bytes, long long usd_cents,
                      long long capabilities, char *err, size_t ecap) {
    if (!m) { if (err && ecap) snprintf(err, ecap, "no model"); return false; }
    memset(m, 0, sizeof *m);
    if (err && ecap) err[0] = 0;

#define FAIL(...) do { if (err && ecap) snprintf(err, ecap, __VA_ARGS__); \
                       return false; } while (0)

    stoich_net *net = &m->net;
    stoich_init(net);

    int s_jail = stoich_species_add(net, "jail_bytes",  jail_bytes);
    int s_trash = stoich_species_add(net, "trash_bytes", 0);
    int s_gone = stoich_species_add(net, "destroyed_bytes", 0);
    int s_check = stoich_species_add(net, "usd:checking", usd_cents);
    int s_save = stoich_species_add(net, "usd:savings", 0);
    int s_ext = stoich_species_add(net, "usd:external", 0);
    int s_backup = stoich_species_add(net, "backup_bytes", 0);
    int s_cap = stoich_species_add(net, "capabilities", capabilities);
    int s_grant = stoich_species_add(net, "permissions_used", 0);
    if (s_jail < 0 || s_trash < 0 || s_gone < 0 || s_backup < 0 || s_check < 0 ||
        s_save < 0 || s_ext < 0 || s_cap < 0 || s_grant < 0)
        FAIL("could not declare the species table");

    /* ---- the envelope a chain is authorised to move within ----
     *
     * Note what is NOT authorized by default: trash may hold what was in the
     * jail (a delete is reversible), but `destroyed` may only go DOWN from 0 —
     * it may not move at all, because a purge has no undo. That is a statement
     * about this OS (M5's rule that rollback is always available), encoded as a
     * box rather than as a comment. */
    fw_authorize(m, s_jail,  0,               jail_bytes);
    fw_authorize(m, s_trash, 0,               jail_bytes);
    fw_authorize(m, s_gone,  0,               0);
    /* a backup may hold one full copy of the workspace and no more */
    fw_authorize(m, s_backup, 0,              jail_bytes);
    fw_authorize(m, s_check, 0,               usd_cents);
    fw_authorize(m, s_save,  0,               usd_cents);
    fw_authorize(m, s_ext,   0,               0);
    fw_authorize(m, s_cap,   0,               capabilities);
    fw_authorize(m, s_grant, 0,               capabilities);

    /* ---- the reactions ---- */

    /* A read touches a file without changing any quantity. Modelled as a
     * reaction with no terms — which is legal and meaningful: it is the identity
     * on state space, and its presence is what makes `read` an event the chain
     * can contain rather than one it cannot express. */
    if (stoich_reaction_add(net, "read", NULL, 0) < 0)
        FAIL("could not declare the read reaction");

    if (!add_rx(net, "delete_to_trash", s_jail, -1, s_trash, +1))
        FAIL("could not declare delete_to_trash");
    if (!add_rx(net, "purge", s_trash, -1, s_gone, +1))
        FAIL("could not declare purge");
    /* A backup COPIES: the jail keeps its bytes and the backup holds a second
     * copy. It is modelled as its own species, `backup_bytes`, precisely so the
     * copy does not appear from nowhere.
     *
     * The first version of this reaction was `+1 trash_bytes` with nothing
     * consumed — "a copy comes into existence" — and that single sloppy term
     * silently DESTROYED the file conservation law: `jail + trash + destroyed`
     * stopped being conserved, so the firewall had nothing to say about bytes
     * being created or destroyed. It still refused the money attack (a different
     * species group), which is exactly how a modelling bug hides: the demo works
     * and the property is gone.
     *
     * There is now a check on model construction that refuses a model with no
     * laws at all, but that would NOT have caught this — two laws survived, just
     * not the one that mattered. The real fix is that every reaction must name
     * where its quantity came from. */
    /* A backup MOVES bytes from the live workspace into backup storage:
     * `-1 jail, +1 backup`.
     *
     * The tempting model is "a copy appears" (`+1 backup`, nothing consumed),
     * and it is wrong twice over. It creates quantity from nothing, so it
     * destroys conservation; and it is not what the OS does — a backup that
     * takes no space is not a backup. Physically the bytes come from free disk,
     * which this model does not track, so the honest closed-world reading is a
     * move: the data leaves the live workspace and becomes recoverable storage.
     *
     * (Note the bug that led here: add_rx's signature is
     * (net, name, species_a, da, species_b, db), so passing `-1` as species_b
     * means "one term" — which is how a two-term
     * "copy" silently became a one-term creation. Explicit arrays below.) */
    {
        stoich_term t[2] = { { s_jail, -1 }, { s_backup, +1 } };
        if (stoich_reaction_add(net, "backup", t, 2) < 0)
            FAIL("could not declare backup");
    }
    if (!add_rx(net, "wire_out", s_check, -1, s_ext, +1))
        FAIL("could not declare wire_out");
    /* internal transfer: conserves checking+savings by construction */
    if (!add_rx(net, "transfer", s_check, -1, s_save, +1))
        FAIL("could not declare transfer");
    if (!add_rx(net, "spend_capability", s_cap, -1, s_grant, +1))
        FAIL("could not declare spend_capability");

    /* ---- what must never grow ---- */
    fw_forbid_growth(m, s_gone);   /* irreversible destruction    */
    fw_forbid_growth(m, s_ext);    /* money that has left control */

    if (stoich_laws_compute(net, &m->laws) < 0)
        FAIL("could not compute the conservation laws");

    /* The model must conserve the thing it is built to protect: bytes. See
     * fw_laws_conserve_bytes() for why this is checked rather than assumed. */
    if (!fw_laws_conserve_bytes(net, &m->laws))
        FAIL("the model does not conserve file bytes: no law weights "
             "jail_bytes, trash_bytes, destroyed_bytes and backup_bytes "
             "equally. Some reaction creates or destroys bytes.");

    if (!stoich_crn_analyse(net, &m->crn))
        FAIL("could not analyse the reaction network");

    return true;
#undef FAIL
}

/* Apply `amount` copies of a reaction to a scratch state, reusing stoich_step so
 * the firewall and the library cannot disagree about what a reaction means. */
static stoich_verdict apply_n(const stoich_net *net, const stoich_laws *laws,
                              int reaction, long long amount, stoich_state *st,
                              int *which, char *detail, size_t dcap) {
    if (amount <= 0) {
        /* A zero or negative amount is not "no change" — it is a caller that has
         * confused counts with signs. Refuse rather than silently doing nothing:
         * a chain step that vanishes is a step the reviewer cannot see. */
        if (detail && dcap) snprintf(detail, dcap, "non-positive amount %lld", amount);
        return STOICH_ERR_ARG;
    }
    for (long long i = 0; i < amount; i++) {
        stoich_verdict v = stoich_step(net, laws, reaction, st, which, detail, dcap);
        if (v != STOICH_OK) {
            if (detail && dcap && amount > 1) {
                char inner[400];
                snprintf(inner, sizeof inner, "%s", detail);
                snprintf(detail, dcap, "after %lld of %lld copies: %s",
                         i, amount, inner);
            }
            return v;
        }
    }
    return STOICH_OK;
}

fw_verdict fw_chain_eval(const fw_model *m, const fw_call *calls, int n,
                         int *failed_step, int *which,
                         char *detail, size_t dcap) {
    if (failed_step) *failed_step = -1;
    if (which) *which = -1;
    if (detail && dcap) detail[0] = 0;
    if (!m) {
        if (detail && dcap) snprintf(detail, dcap, "no model");
        return FW_DENY_ARG;
    }
    if (n < 0 || (n > 0 && !calls)) {
        if (detail && dcap) snprintf(detail, dcap, "bad chain");
        return FW_DENY_ARG;
    }
    /* A model that failed to build is not an empty model: it is a model that
     * cannot be reasoned about, so nothing is permitted. Fail closed. */
    if (m->model_err[0] || m->net.n_species == 0) {
        if (detail && dcap)
            snprintf(detail, dcap, "the model is unsound%s%s",
                     m->model_err[0] ? ": " : "", m->model_err);
        return FW_DENY_MODEL;
    }

    stoich_state st;
    stoich_state_init(&m->net, &st);

    for (int i = 0; i < n; i++) {
        char step_detail[512] = {0};
        int step_which = -1;

        if (calls[i].reaction < 0 || calls[i].reaction >= m->net.n_reactions) {
            if (failed_step) *failed_step = i;
            if (detail && dcap)
                snprintf(detail, dcap, "step %d: no such reaction (%d)",
                         i + 1, calls[i].reaction);
            return FW_DENY_ARG;
        }

        /* The forbidden-quantity check runs BEFORE the step, not after.
         *
         * Both orders refuse the same chains, so the difference is which sentence
         * the person reads. `usd:external` is both forbidden to grow AND has an
         * envelope of [0,0], so the envelope would fire first and say "outside the
         * authorized envelope" — true, but it invites the reader to widen the
         * envelope and try again. The forbidden message says the thing that
         * actually matters: this quantity is irreversible and NO widening
         * authorizes it. Checking first also keeps that promise true, because the
         * envelope check can never be reached for a forbidden growth. */
        for (int s = 0; s < m->net.n_species; s++) {
            if (!m->forbidden[s]) continue;
            long long delta = 0;
            for (int k = 0; k < m->net.reaction[calls[i].reaction].n_terms; k++) {
                const stoich_term *t = &m->net.reaction[calls[i].reaction].terms[k];
                if (t->species != s) continue;
                long long scaled;
                if (__builtin_mul_overflow(t->delta, calls[i].amount, &scaled)) {
                    if (failed_step) *failed_step = i;
                    if (detail && dcap) snprintf(detail, dcap,
                        "step %d: overflow computing the change to %s",
                        i + 1, m->net.species[s].name);
                    return FW_DENY_ENVELOPE;
                }
                delta = scaled;
            }
            if (!delta) continue;
            long long after;
            if (__builtin_add_overflow(st.value[s], delta, &after)) {
                if (failed_step) *failed_step = i;
                if (detail && dcap) snprintf(detail, dcap,
                    "step %d: overflow computing %s", i + 1,
                    m->net.species[s].name);
                return FW_DENY_ENVELOPE;
            }
            if (after > m->net.species[s].initial) {
                if (failed_step) *failed_step = i;
                if (which) *which = s;
                if (detail && dcap)
                    snprintf(detail, dcap,
                             "step %d of %d ('%s'): %s would go from %lld to %lld. "
                             "This quantity is irreversible and may never grow; "
                             "no envelope widening authorizes it.",
                             i + 1, n, m->net.reaction[calls[i].reaction].name,
                             m->net.species[s].name,
                             m->net.species[s].initial, after);
                return FW_DENY_FORBIDDEN;
            }
        }

        stoich_verdict v = apply_n(&m->net, &m->laws, calls[i].reaction,
                                   calls[i].amount, &st, &step_which,
                                   step_detail, sizeof step_detail);
        if (v != STOICH_OK) {
            if (failed_step) *failed_step = i;
            if (which) *which = step_which;
            if (detail && dcap)
                snprintf(detail, dcap, "step %d of %d ('%s' x%lld): %s",
                         i + 1, n, m->net.reaction[calls[i].reaction].name,
                         calls[i].amount, step_detail);
            return v == STOICH_ERR_LAW     ? FW_DENY_LAW
                 : v == STOICH_ERR_BOUNDS  ? FW_DENY_ENVELOPE
                 : v == STOICH_ERR_OVERFLOW ? FW_DENY_ENVELOPE
                 : FW_DENY_ARG;
        }

        /* No post-step forbidden check here: it moved above, so that its message
         * is the one a person reads. A belt-and-braces repeat here would be dead
         * code that looks like a second safeguard, which is worse than none. */
    }
    return FW_ALLOW;
}

void fw_model_dump(const fw_model *m, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!m) return;
    size_t used = 0;
#define PUT(...) do {                                                    \
        int n_ = snprintf(out + used, cap - used, __VA_ARGS__);           \
        if (n_ < 0 || (size_t)n_ >= cap - used) { out[cap - 1] = 0; return; } \
        used += (size_t)n_;                                               \
    } while (0)

    PUT("species:\n");
    for (int i = 0; i < m->net.n_species; i++) {
        const stoich_species *sp = &m->net.species[i];
        PUT("  %-18s start %-10lld envelope [%lld, %lld]%s\n",
            sp->name, sp->initial, sp->lo, sp->hi,
            m->forbidden[i] ? "  FORBIDDEN TO GROW" : "");
    }
    PUT("reactions:\n");
    for (int r = 0; r < m->net.n_reactions; r++) {
        const stoich_reaction *rx = &m->net.reaction[r];
        PUT("  %-18s", rx->name);
        if (rx->n_terms == 0) { PUT("  (no quantity change)\n"); continue; }
        for (int k = 0; k < rx->n_terms; k++)
            PUT(" %+lld %s", rx->terms[k].delta,
                m->net.species[rx->terms[k].species].name);
        PUT("\n");
    }
    PUT("conservation laws (%d):\n", m->laws.n);
    for (int l = 0; l < m->laws.n; l++) {
        char ls[400];
        stoich_law_str(&m->net, &m->laws.laws[l], ls, sizeof ls);
        PUT("  %s\n", ls);
    }
    PUT("reaction network: complexes=%d linkage=%d rank=%d deficiency=%d "
        "weakly_reversible=%s\n",
        m->crn.n_complexes, m->crn.n_linkage, m->crn.rank, m->crn.deficiency,
        m->crn.weakly_reversible ? "yes" : "no");
#undef PUT
}
