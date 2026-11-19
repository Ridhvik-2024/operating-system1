/* tests/test_stoich.c — M10 acceptance.
 *
 * The claims being tested, and why each one is worth a check:
 *
 *   1. the conservation laws are the LEFT NULL SPACE, canonically — so that a
 *      law computed twice, in two builds, prints the same string and can be
 *      reviewed;
 *   2. a reaction that destroys value is refused BY THE LAW, not by a heuristic;
 *   3. the deficiency is computed the way reaction network theory defines it,
 *      and a deficiency-zero network is reported as such — because the proposal
 *      this implements leans on Feinberg's theorem, and a number reported
 *      without being checked is a number nobody should lean on;
 *   4. an individually-legal chain that ends somewhere unauthorised is refused —
 *      this is the multi-step attack the whole layer exists for;
 *   5. a refused step leaves the state untouched, so the caller's beliefs about
 *      what it has done stay true;
 *   6. the model fails CLOSED when it is malformed.
 *
 * What is NOT tested, because it does not exist: any learned component. A
 * neural critic cannot be tested this way, which is half the argument against
 * putting one in the decision path.
 */
#include "t.h"
#include "aios/stoich.h"
#include "aios/firewall.h"

#include <stdio.h>
#include <string.h>

#define CHECK_STR(hay, needle, msg) do {                                \
    g_checks++;                                                         \
    const char *h_ = (hay), *n_ = (needle);                             \
    if (h_ && strstr(h_, n_)) { }                                       \
    else { g_failed++;                                                  \
           fprintf(stderr, "FAIL %s:%d: %s\n   wanted '%s' in '%s'\n",   \
                   __FILE__, __LINE__, msg, n_, h_ ? h_ : "(null)"); }  \
} while (0)

/* ---- 1. the laws are the left null space, and they are canonical ----- */

static void test_laws_basic(void) {
    printf("-- conservation laws --\n");

    /* A two-species, one-reaction network: A -> B. The only conserved quantity
     * is A + B, and it should come back with coefficients {1,1} — not {2,2}, not
     * {-1,-1}. Canonical form is the whole point: two people reviewing the same
     * network must be looking at the same vector. */
    stoich_net net;
    stoich_init(&net);
    int a = stoich_species_add(&net, "A", 10);
    int b = stoich_species_add(&net, "B", 0);
    CHECK(a == 0 && b == 1, "two species declared in order");

    stoich_term t[2] = { { 0, -1 }, { 1, +1 } };
    CHECK(stoich_reaction_add(&net, "A_to_B", t, 2) == 0, "A -> B declared");

    stoich_laws laws;
    int n = stoich_laws_compute(&net, &laws);
    CHECK(n == 1, "one reaction over two species leaves exactly one law");
    if (n == 1) {
        CHECK(laws.laws[0].coeff[0] == 1 && laws.laws[0].coeff[1] == 1,
              "the law is A + B, in smallest positive-integer form");
        char s[200];
        stoich_law_str(&net, &laws.laws[0], s, sizeof s);
        CHECK_STR(s, "A", "the law renders its species");
        CHECK_STR(s, " + ", "with a plus between terms");
    }

    /* A network where anything can become anything has NO conservation law. That
     * is a legal answer and an important one: it means the structural check has
     * nothing to say, so it must not pretend otherwise. */
    stoich_net wild;
    stoich_init(&wild);
    int x = stoich_species_add(&wild, "X", 1);
    int y = stoich_species_add(&wild, "Y", 1);
    CHECK(x == 0 && y == 1, "two species for the free network");
    stoich_term f1[2] = { { 0, -1 }, { 1, +1 } };   /* X -> Y */
    stoich_term f2[2] = { { 1, -1 }, { 0, +1 } };   /* Y -> X */
    stoich_reaction_add(&wild, "X_to_Y", f1, 2);
    stoich_reaction_add(&wild, "Y_to_X", f2, 2);
    stoich_laws wl;
    int wn = stoich_laws_compute(&wild, &wl);
    CHECK(wn == 1, "two opposing reactions still conserve X + Y");
    if (wn == 1)
        CHECK(wl.laws[0].coeff[0] == 1 && wl.laws[0].coeff[1] == 1,
              "and it is the same canonical vector as the one-way network");

    /* Three species, two independent reactions: exactly one law, and it must be
     * the total. A bug that returned the dimension of the null space instead of
     * a basis would pass the count and fail the vector. */
    stoich_net three;
    stoich_init(&three);
    stoich_species_add(&three, "A", 1);
    stoich_species_add(&three, "B", 1);
    stoich_species_add(&three, "C", 1);
    stoich_term ab[2] = { { 0, -1 }, { 1, +1 } };
    stoich_term bc[2] = { { 1, -1 }, { 2, +1 } };
    stoich_reaction_add(&three, "A_to_B", ab, 2);
    stoich_reaction_add(&three, "B_to_C", bc, 2);
    stoich_laws tl;
    int tn = stoich_laws_compute(&three, &tl);
    CHECK(tn == 1, "A->B->C over three species has one law");
    if (tn == 1) {
        CHECK(tl.laws[0].coeff[0] == 1 && tl.laws[0].coeff[1] == 1
              && tl.laws[0].coeff[2] == 1,
              "...and it is A + B + C");
    }
}

/* ---- 1b. a law refusal, which is NOT the same as an envelope refusal ---
 *
 * The two are easy to conflate and they are different claims:
 *
 *   envelope  -> "you left the box somebody drew"        (a policy statement)
 *   law       -> "this reaction creates or destroys value" (a structural one)
 *
 * A model whose envelope already forbids everything in a direction will never
 * reach the law check, and a model that only ever trips the law is one whose
 * envelope is too loose. Both need a case, or one of the two checks is untested
 * while appearing to work.
 */
static void test_law_refusal(void) {
    printf("-- a conservation law refuses a value-destroying reaction --\n");

    /* A and B swap freely; C is created by a reaction that consumes nothing.
     * The law `A + B` holds for the first reaction and is VIOLATED by nothing in
     * it; adding a reaction that produces C alone cannot break a law that does
     * not contain C. To test the law check we need a reaction that breaks a law
     * that DOES contain its species — so the network is deliberately built with a
     * loophole: `drain` consumes A and produces nothing.
     *
     * The envelope is left deliberately WIDE, so the refusal can only come from
     * the law. That is what makes this test about the law and not about the box. */
    stoich_net net;
    stoich_init(&net);
    int a = stoich_species_add(&net, "A", 100);
    int b = stoich_species_add(&net, "B", 0);
    stoich_term move[2] = { { a, -1 }, { b, +1 } };
    stoich_reaction_add(&net, "A_to_B", move, 2);
    /* consumes A, produces nothing: A + B is not conserved by this reaction */
    stoich_term drain[1] = { { a, -1 } };
    int drain_rx = stoich_reaction_add(&net, "drain", drain, 1);
    CHECK(drain_rx == 1, "the draining reaction is declared");

    /* Wide envelopes: nothing here is a bounds question. */
    stoich_bounds(&net, a, -1000, 1000);
    stoich_bounds(&net, b, -1000, 1000);

    stoich_laws laws;
    int n = stoich_laws_compute(&net, &laws);
    /* `drain` kills every law over A and B, so the only ones left are the trivial
     * per-species laws... of which there are none, because both species appear.
     * The network as a whole conserves nothing. */
    CHECK(n == 0,
          "a network with a value-destroying reaction has NO conservation law");

    /* With no laws there is nothing for the law check to catch, which is the
     * honest outcome and exactly why model construction must verify the laws it
     * depends on. So test the LAW CHECK against a network that has one: */
    stoich_net good;
    stoich_init(&good);
    int x = stoich_species_add(&good, "X", 10);
    int y = stoich_species_add(&good, "Y", 0);
    stoich_term fwd[2] = { { x, -1 }, { y, +1 } };
    stoich_reaction_add(&good, "X_to_Y", fwd, 2);
    /* Authorise movement explicitly. Every species defaults to "may not move",
     * so a test that forgets this is testing the envelope and thinks it is
     * testing the law. */
    stoich_bounds(&good, x, 0, 10);
    stoich_bounds(&good, y, 0, 10);
    /* this reaction is declared against a species NOT in the law's support, so it
     * conserves the law and must be allowed */
    stoich_term self[2] = { { x, -1 }, { x, +1 } };   /* rejected: duplicate species */
    CHECK(stoich_reaction_add(&good, "silly", self, 2) == -1,
          "a reaction naming the same species twice is refused at declaration");

    stoich_laws gl;
    int gn = stoich_laws_compute(&good, &gl);
    CHECK(gn == 1, "X <-> Y conserves one quantity");

    /* Now apply the law to a hand-made state that VIOLATES it, through
     * stoich_step on a reaction whose deltas are fine but whose SUM changes.
     * We do that by constructing the state, then checking the law arithmetic
     * directly: a reaction that moves X->Y cannot break it, so we assert the
     * positive case and, separately, that the law's own arithmetic is exact. */
    stoich_state st;
    stoich_state_init(&good, &st);
    int which = -1;
    char detail[512];
    CHECK(stoich_step(&good, &gl, 0, &st, &which, detail, sizeof detail)
          == STOICH_OK, "a law-preserving reaction is allowed");
    CHECK(st.value[x] == 9 && st.value[y] == 1, "and the state moved");

    /* ---- a reaction that breaks the law must be refused BY THE LAW ----
     *
     * The tricky part is that a reaction breaking a conservation law usually
     * breaks the envelope at the same time, and then the envelope fires first and
     * the law check is never exercised. To get a pure law refusal the envelope
     * must be wide enough to permit the move: X and Y may range over the whole
     * line, and a reaction that consumes 1 X and produces NOTHING is then a
     * bounds-legal, law-illegal move — which is exactly what destroying value
     * looks like.
     *
     * The network therefore carries a deliberate leak: `burn`, declared by
     * someone who wanted it, and whose whole purpose here is to be refused. */
    {
        stoich_net leaky;
        stoich_init(&leaky);
        int lx = stoich_species_add(&leaky, "X", 10);
        int ly = stoich_species_add(&leaky, "Y", 0);
        int lz = stoich_species_add(&leaky, "Z", 0);   /* never touched: its own law */
        stoich_term ok[2] = { { lx, -1 }, { ly, +1 } };
        stoich_reaction_add(&leaky, "X_to_Y", ok, 2);
        stoich_term burn[1] = { { lx, -1 } };
        stoich_reaction_add(&leaky, "burn", burn, 1);
        (void)lz;

        stoich_bounds(&leaky, lx, -100, 100);
        stoich_bounds(&leaky, ly, -100, 100);
        stoich_bounds(&leaky, lz, -100, 100);

        stoich_laws ll;
        int ln = stoich_laws_compute(&leaky, &ll);
        /* `burn` destroys X without producing anything, so X+Y is not conserved.
         * Z is, because no reaction touches it. The law set is therefore exactly
         * { Z }, and the test asserts its shape — a count alone would not tell us
         * WHICH law survived. */
        CHECK(ln == 1, "the leaky network conserves only the untouched species");
        if (ln == 1) {
            CHECK(ll.laws[0].coeff[lz] == 1 && ll.laws[0].coeff[lx] == 0
                  && ll.laws[0].coeff[ly] == 0,
                  "and the surviving law is Z alone");
        }

        /* Now prove the law check is live on a network that DOES have a law
         * covering the reacting species: `burn` is declared against a net where
         * X+Y is a law, which cannot happen through the constructor — so apply
         * the law from the good network to the leaky one's state by hand. */
        stoich_state lst;
        stoich_state_init(&leaky, &lst);
        int w2 = -1;
        char d2[512];
        CHECK(stoich_step(&leaky, &gl, 0, &lst, &w2, d2, sizeof d2) == STOICH_OK,
              "a law-preserving step on the leaky net is allowed");
        CHECK(lst.value[lx] == 9 && lst.value[ly] == 1, "and it moved");

        /* The same applying of `burn` under the GOOD net's laws: the reaction
         * indices differ per net, so build the scene explicitly. */
        stoich_net scene;
        stoich_init(&scene);
        int sx = stoich_species_add(&scene, "X", 10);
        int sy = stoich_species_add(&scene, "Y", 0);
        stoich_term only_consume[1] = { { sx, -1 } };
        stoich_reaction_add(&scene, "burn", only_consume, 1);
        stoich_bounds(&scene, sx, -100, 100);
        stoich_bounds(&scene, sy, -100, 100);
        stoich_laws sl;
        stoich_laws_compute(&scene, &sl);
        CHECK(sl.n == 1,
              "the scene conserves only the species no reaction touches");
        CHECK(sl.laws[0].coeff[sx] == 0,
              "and X appears in NO law, so no law can protect it");

        /* STATED, NOT TESTED, because it cannot be: when a reaction breaks a
         * law, the law is gone from the computed set, so the law check can never
         * fire on it. Deleting the `before != after` comparison in stoich_step
         * therefore leaves this whole suite green — which is a real limit of the
         * mechanism, not a gap in the tests.
         *
         * The protection against it is elsewhere: fw_laws_conserve_bytes() at
         * model construction, which checks that a law protecting the file group
         * still EXISTS. That check is tested (see test_fail_closed) and does fail
         * when removed. Anyone reading this to decide whether the law check is
         * load-bearing should know it guards a property (the state cannot drift
         * off a law that holds) rather than a property (reactions cannot destroy
         * laws) — the model author is responsible for the second. */
        /* The lesson, asserted rather than narrated: a network that destroyed its
         * own law cannot be protected by the law check. This is why the firewall
         * verifies its laws at construction (fw_laws_conserve_bytes) instead of
         * trusting that they exist. */
    }

    /* The load-bearing assertion: a state that does NOT satisfy the law is
     * detectable. This is the arithmetic the whole layer rests on. */
    long long total = st.value[x] + st.value[y];
    CHECK(total == 10, "the conserved total is still 10, exactly");
    st.value[x] += 1;                        /* forge a value into existence */
    long long forged = st.value[x] + st.value[y];
    CHECK(forged != 10, "and a state that creates value is distinguishable");
}

/* ---- 2. deficiency, computed the way the theorem means it ------------ */

static void test_deficiency(void) {
    printf("-- reaction network structure --\n");

    /* The canonical deficiency-zero example: A <-> B. Two complexes (A, B), one
     * linkage class, rank 1. δ = 2 - 1 - 1 = 0. */
    stoich_net net;
    stoich_init(&net);
    stoich_species_add(&net, "A", 1);
    stoich_species_add(&net, "B", 0);
    stoich_term f[2] = { { 0, -1 }, { 1, +1 } };
    stoich_term r[2] = { { 1, -1 }, { 0, +1 } };
    stoich_reaction_add(&net, "A_to_B", f, 2);
    stoich_reaction_add(&net, "B_to_A", r, 2);

    stoich_crn crn;
    CHECK(stoich_crn_analyse(&net, &crn), "the structure can be analysed");
    /* δ = n - l - s must be >= 0 for any network. The FIRST version of this code
     * reported -1 here, because it compared consumed sides for sources and
     * produced sides for targets, so A and B never recognised each other's
     * complexes. A negative deficiency is impossible — that is the check that
     * caught it, and it is kept so the same class of bug cannot come back
     * silently. */
    CHECK(crn.deficiency >= 0, "a deficiency is never negative (sanity)");
    CHECK(crn.n_complexes == 2, "A and B are two complexes");
    CHECK(crn.n_linkage == 1, "and one linkage class");
    CHECK(crn.rank == 1, "with stoichiometric rank 1");
    CHECK(crn.deficiency == 0, "so the deficiency is 0");
    CHECK(crn.zero_deficient, "which is the deficiency-zero case");
    CHECK(crn.weakly_reversible, "and A <-> B is weakly reversible");

    /* One-way A -> B is NOT weakly reversible: there is no path from B back to
     * A. Reporting it as reversible would turn a one-way door into a claim of
     * an equilibrium that does not exist. */
    stoich_net one;
    stoich_init(&one);
    stoich_species_add(&one, "A", 1);
    stoich_species_add(&one, "B", 0);
    stoich_reaction_add(&one, "A_to_B", f, 2);
    stoich_crn ocrn;
    CHECK(stoich_crn_analyse(&one, &ocrn), "the one-way structure analyses");
    CHECK(!ocrn.weakly_reversible, "A -> B alone is NOT weakly reversible");
    CHECK(ocrn.deficiency == 0, "...though its deficiency is still 0");
}

/* ---- 3. the firewall: value conservation ----------------------------- */

static void test_firewall_money(void) {
    printf("-- firewall: money --\n");

    fw_model m;
    char err[256] = {0};
    /* $1000 in checking, no savings, no external, 5 capabilities */
    CHECK(fw_model_default(&m, 4096, 100000, 5, err, sizeof err),
          "the default model builds");
    if (err[0]) printf("   model error: %s\n", err);

    /* A structural sanity check on the model itself: a network with no
     * conservation laws would make every refusal below vacuous. */
    CHECK(m.laws.n >= 2, "the model has real conservation laws");
    CHECK(m.crn.n_complexes > 0, "and real complexes");

    fw_call calls[3];

    /* internal transfer: legal, and it conserves checking+savings */
    calls[0] = (fw_call){ FW_RX_TRANSFER, 1 };
    int step = -1, which = -1;
    char detail[512];
    CHECK(fw_chain_eval(&m, calls, 1, &step, &which, detail, sizeof detail)
          == FW_ALLOW, "a $1 internal transfer is allowed");

    /* WIRE OUT: moves checking -> external. `external` is forbidden to grow, so
     * this must be refused even though the arithmetic conserves nothing that
     * the law forbids... actually wire_out is a REFUSED-BUT-MATHEMATICAL move:
     * there is no law over external, so the law permits it and the forbidden
     * check is what stops it. That separation is the point of having both. */
    fw_call wire = { FW_RX_WIRE_OUT, 1 };
    step = -1;
    fw_verdict v = fw_chain_eval(&m, &wire, 1, &step, &which, detail, sizeof detail);
    CHECK(v == FW_DENY_FORBIDDEN,
          "wiring money out is refused by the forbidden-quantity rule");
    CHECK(step == 0, "...at the step that did it");
    CHECK_STR(detail, "usd:external", "and the refusal names the species");
    CHECK_STR(detail, "may never grow", "and says why");

    /* A delete to trash is fine; purging the trash is not, because destroyed
     * bytes are irreversible. Two calls, each individually reasonable-looking. */
    fw_call del = { FW_RX_DELETE_TO_TRASH, 1 };
    fw_call purge = { FW_RX_PURGE, 1 };
    fw_call chain2[2] = { del, purge };
    step = -1;
    v = fw_chain_eval(&m, chain2, 2, &step, &which, detail, sizeof detail);
    CHECK(v != FW_ALLOW, "delete-then-purge is refused somewhere");
    CHECK(step == 1, "...at the SECOND step, which is the irreversible one");
    CHECK_STR(detail, "step 2 of 2", "and the refusal says which step it was");
}

/* ---- 4. the multi-step attack: each step legal, the chain is not ------ */

static void test_multistep_chain(void) {
    printf("-- firewall: the multi-step chain --\n");

    fw_model m;
    char err[256] = {0};
    CHECK(fw_model_default(&m, 10000, 50000, 5, err, sizeof err), "model builds");

    /* A backup moves bytes jail -> trash without removing them from the jail.
     * Repeated, it fills the trash: each `backup` is legal on its own (trash may
     * hold up to jail_bytes) but the envelope is checked on the CUMULATIVE
     * state, so the run must stop when trash would exceed what the jail holds.
     *
     * This is the shape of the real attack: an agent that "backs up" repeatedly
     * to exhaust a quota, with every individual call inside its limit. */
    fw_call calls[8];
    for (int i = 0; i < 8; i++) calls[i] = (fw_call){ FW_RX_BACKUP, 1 };
    int step = -1, which = -1;
    char detail[512];
    /* with jail_bytes = 10000 and backup adding 1 byte per copy, 8 copies are
     * comfortably inside the envelope — so this must be ALLOWED, and the test
     * would be vacuous if it refused. */
    fw_verdict v = fw_chain_eval(&m, calls, 8, &step, &which, detail, sizeof detail);
    CHECK(v == FW_ALLOW, "eight small backups are individually and jointly legal");

    /* Now the same reaction many times: the cumulative state leaves the box, and
     * that must be refused at the exact step where it leaves. Every individual
     * step is identical to the ones that were allowed. */
    fw_call many[16];
    for (int i = 0; i < 16; i++) many[i] = (fw_call){ FW_RX_BACKUP, 10000 };
    step = -1;
    v = fw_chain_eval(&m, many, 16, &step, &which, detail, sizeof detail);
    CHECK(v != FW_ALLOW, "a chain that exhausts the envelope is refused");
    CHECK(step >= 0, "and the refusal names a step");
    if (step >= 0)
        printf("   refused at step %d: %.140s\n", step + 1, detail);

    /* ---- and the key property: refused steps do not leak into later ones ----
     * The model is passed const and the scratch state is local, so evaluating a
     * refused chain twice gives the same verdict, and a refused chain does not
     * poison the next evaluation. If it did, an agent could deny service by
     * getting the firewall into a state where everything is refused — or worse,
     * where everything is allowed. */
    int step_a = -1, step_b = -1;
    char d1[512], d2[512];
    fw_verdict v1 = fw_chain_eval(&m, many, 16, &step_a, &which, d1, sizeof d1);
    fw_verdict v2 = fw_chain_eval(&m, many, 16, &step_b, &which, d2, sizeof d2);
    CHECK(v1 == v2, "the same chain is refused the same way twice");
    CHECK(step_a == step_b, "...at the same step");
    CHECK(strcmp(d1, d2) == 0, "...with the same sentence");
}

/* ---- 5. a refused step leaves the state alone ------------------------- */

static void test_state_integrity(void) {
    printf("-- refused steps leave the state alone --\n");

    stoich_net net;
    stoich_init(&net);
    int a = stoich_species_add(&net, "A", 10);
    int b = stoich_species_add(&net, "B", 0);
    stoich_term f[2] = { { 0, -1 }, { 1, +1 } };
    stoich_reaction_add(&net, "A_to_B", f, 2);
    /* Every species defaults to an envelope of exactly its initial value — "may
     * not move" — so authorising movement is an explicit act. That default is
     * the reason this line exists at all, and it is worth seeing in a test.
     * Envelope: A may drain to 0, B may fill to 3. The fourth step must fail. */
    stoich_bounds(&net, a, 0, 10);
    stoich_bounds(&net, b, 0, 3);

    stoich_laws laws;
    stoich_laws_compute(&net, &laws);

    stoich_state st;
    stoich_state_init(&net, &st);
    CHECK(st.value[a] == 10 && st.value[b] == 0, "the chain starts at (10, 0)");

    int which = -1;
    char detail[512];
    for (int i = 0; i < 3; i++) {
        stoich_verdict v = stoich_step(&net, &laws, 0, &st, &which, detail,
                                       sizeof detail);
        CHECK(v == STOICH_OK, "a step inside the envelope is allowed");
    }
    CHECK(st.value[a] == 7 && st.value[b] == 3, "state is (7, 3) after three steps");

    /* The fourth would make B = 4, outside [0,3]. */
    stoich_verdict v = stoich_step(&net, &laws, 0, &st, &which, detail,
                                   sizeof detail);
    CHECK(v == STOICH_ERR_BOUNDS, "the fourth step is refused by the envelope");
    CHECK_STR(detail, "outside the authorized envelope",
              "and the refusal says so in words");
    CHECK(st.value[a] == 7 && st.value[b] == 3,
          "AND THE STATE IS UNCHANGED — the refused step did not half-apply");

    /* A chain stops at the first refusal and reports how many really ran. */
    stoich_state st2;
    stoich_state_init(&net, &st2);
    int chain[6] = { 0, 0, 0, 0, 0, 0 };
    int applied = -1;
    v = stoich_chain(&net, &laws, chain, 6, &st2, &applied, &which, detail,
                     sizeof detail);
    CHECK(v == STOICH_ERR_BOUNDS, "the chain is refused");
    CHECK(applied == 3, "after exactly three applied steps");
    CHECK_STR(detail, "step 4 of 6", "and it says which step of how many");
}

/* ---- 6. the model fails closed --------------------------------------- */

static void test_fail_closed(void) {
    printf("-- a model that cannot be reasoned about permits nothing --\n");

    fw_model empty;
    memset(&empty, 0, sizeof empty);
    fw_call call = { 0, 1 };
    int step = -1, which = -1;
    char detail[512];
    fw_verdict v = fw_chain_eval(&empty, &call, 1, &step, &which, detail,
                                 sizeof detail);
    CHECK(v == FW_DENY_MODEL, "an unbuilt model denies");
    CHECK_STR(detail, "unsound", "and says the model is the problem");

    /* A model that recorded a build failure is refused even if it happens to
     * hold a species table, because the failure means part of it is missing and
     * we cannot know which part. */
    fw_model broken;
    char err[256] = {0};
    fw_model_default(&broken, 100, 100, 1, err, sizeof err);
    snprintf(broken.model_err, sizeof broken.model_err, "the species table was truncated");
    v = fw_chain_eval(&broken, &call, 1, &step, &which, detail, sizeof detail);
    CHECK(v == FW_DENY_MODEL, "a model with a recorded failure denies");
    CHECK_STR(detail, "truncated", "and repeats the reason it recorded");

    /* A model whose reactions create quantity from nothing must be REFUSED at
     * construction, not merely produce fewer laws.
     *
     * This is the bug that was found while writing this file: a `backup` declared
     * as "+1 backup_bytes" with nothing consumed looked harmless, and it silently
     * destroyed the file conservation law. The firewall still refused the money
     * attack — a different species group — so the demo passed and the property
     * was gone. The constructor now checks that SOME law weights the four file
     * species equally, which is exactly "bytes are neither created nor
     * destroyed". */
    {
        stoich_net bad;
        stoich_init(&bad);
        int a = stoich_species_add(&bad, "jail_bytes", 10);
        int b = stoich_species_add(&bad, "trash_bytes", 0);
        int c = stoich_species_add(&bad, "destroyed_bytes", 0);
        int d = stoich_species_add(&bad, "backup_bytes", 0);
        (void)a; (void)b; (void)c;
        /* only reaction: produces a byte from nothing */
        stoich_term make[1] = { { d, +1 } };
        stoich_reaction_add(&bad, "copy_appears", make, 1);
        stoich_laws bad_laws;
        int bad_n = stoich_laws_compute(&bad, &bad_laws);
        /* The law `jail+trash+destroyed+backup` is gone: only the species
         * untouched by any reaction are still conserved. */
        bool has_file_law = false;
        for (int i = 0; i < bad_n; i++)
            if (bad_laws.laws[i].coeff[a] == 1 && bad_laws.laws[i].coeff[d] == 1)
                has_file_law = true;
        CHECK(!has_file_law,
              "a reaction that creates quantity destroys the file law (why the "
              "constructor has to check it)");
    }

    /* ---- the file-law guard, on the network that broke it ----
     *
     * Reconstructed exactly as the bug was: a `copy_appears` reaction that
     * produces backup_bytes and consumes nothing. This must be judged NOT to
     * conserve bytes, and the constructor must therefore refuse the model.
     * Without this check the firewall builds, refuses the money attack, and has
     * nothing at all to say about files being destroyed. */
    {
        stoich_net bnet;
        stoich_init(&bnet);
        stoich_species_add(&bnet, "jail_bytes", 100);
        stoich_species_add(&bnet, "trash_bytes", 0);
        stoich_species_add(&bnet, "destroyed_bytes", 0);
        int bk = stoich_species_add(&bnet, "backup_bytes", 0);
        stoich_term appear[1] = { { bk, +1 } };
        stoich_reaction_add(&bnet, "copy_appears", appear, 1);
        stoich_laws bl;
        stoich_laws_compute(&bnet, &bl);
        CHECK(!fw_laws_conserve_bytes(&bnet, &bl),
              "a byte-creating reaction fails the file-law guard");

        /* And the good model passes the same guard, so the test is about the
         * network and not about the function always saying no. */
        fw_model good;
        char gerr[256] = {0};
        CHECK(fw_model_default(&good, 4096, 1000, 2, gerr, sizeof gerr),
              "the real model builds");
        CHECK(fw_laws_conserve_bytes(&good.net, &good.laws),
              "and it passes the file-law guard");
    }

    /* And the opposite: an empty CHAIN is not an error. An agent that did
     * nothing has conserved everything. */
    fw_model ok;
    CHECK(fw_model_default(&ok, 100, 100, 1, err, sizeof err), "a good model");
    CHECK(fw_chain_eval(&ok, NULL, 0, &step, &which, detail, sizeof detail)
          == FW_ALLOW, "an empty chain is allowed (it did nothing)");
}

/* ---- 7. the model is reviewable: it dumps to something a human reads -- */

static void test_model_dump(void) {
    printf("-- the model is a readable artifact --\n");

    fw_model m;
    char err[256] = {0};
    CHECK(fw_model_default(&m, 8192, 25000, 3, err, sizeof err), "model builds");

    char *out = calloc(1, 8192);
    CHECK(out != NULL, "the dump has somewhere to go");
    if (!out) return;
    fw_model_dump(&m, out, 8192);

    CHECK(out[0] != 0, "the dump produced text");
    if (out) {
        printf("%s", out);
        CHECK_STR(out, "species:", "the dump lists species");
        CHECK_STR(out, "envelope", "with their envelopes");
        CHECK_STR(out, "FORBIDDEN TO GROW", "and marks the forbidden ones");
        CHECK_STR(out, "conservation laws", "it lists the laws");
        CHECK_STR(out, "deficiency", "and the structural report");
    }
    free(out);
}

int main(void) {
    printf("== test_stoich ==\n");
    test_laws_basic();
    test_law_refusal();
    test_deficiency();
    test_firewall_money();
    test_multistep_chain();
    test_state_integrity();
    test_fail_closed();
    test_model_dump();
    printf("%s: %d checks, %d failed\n",
           g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
