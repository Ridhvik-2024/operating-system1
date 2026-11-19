/* tests/test_nullspace.c — M11 acceptance.
 *
 * The proposal makes one true claim and one false one. This suite proves both,
 * because a milestone that only tests the working half would be a brochure.
 *
 *   TRUE   a Stiefel rotation preserves every inner product (attention angles),
 *          to floating-point noise. Tested on a real QR-derived orthogonal Q.
 *
 *   FALSE  that a secret rotation hides anything. A rotation is a LINEAR
 *          BIJECTION with n(n-1)/2 parameters, so it is recoverable from n known
 *          plaintexts, and then it exposes everything. This suite recovers an
 *          unknown Q from known plaintexts and then reads a HELD-OUT document
 *          that was never in the known set — which is the attack, demonstrated,
 *          not argued.
 *
 *   COND.  projection removes a linearly encoded attribute exactly, and does
 *          nothing to one encoded in the norm. The auditor must certify the
 *          first pipeline as clean (after a real removal) and refuse the
 *          vendor's rotate-only pipeline, which reports perfect linear privacy
 *          while the quadratic probe reads the attribute straight out.
 *
 * The synthetic corpus is generated deterministically from a small xorshift, so
 * two runs agree to the last bit. No RNG, no library, no numpy: the audit a
 * hospital would run has to be reproducible on the machine it runs on.
 */
#include "t.h"
#include "aios/nullspace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 240
#define D 8
#define K 3

static unsigned long g_s = 0x9E3779B97F4A7C15UL;
static double urand(void) {                    /* uniform in (-1, 1) */
    g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17;
    return ((double)(g_s >> 11) / (double)(1UL << 52)) - 1.0;
}
static double nrand(void) {   /* ~N(0,1): 12 uniforms in (-1,1) sum to var 4, so
                                 subtract 6 to centre and halve the spread. The
                                 first version forgot this and had std 2, which
                                 halved the effective signal and made a plainly
                                 separable corpus look marginal. */
    double s = 0.0;
    for (int i = 0; i < 12; i++) s += urand();
    return (s - 6.0) / 2.0;
}

static double X[N * D];      /* plain embeddings                              */
static double B[K * D];      /* the "identity subspace" basis, orthonormalised*/
static double Q[D * D];      /* a secret rotation (orthogonal)                */
static unsigned char Y[N];   /* the protected attribute                       */

/* Build a random orthogonal matrix by Gram-Schmidt on random rows. Using the
 * module's own orthonormaliser here would test it with itself, so this does the
 * arithmetic independently and then checks QᵀQ = I explicitly. */
static void make_rotation(void) {
    double r[D * D];
    for (int i = 0; i < D * D; i++) r[i] = nrand();
    for (int i = 0; i < D; i++) {
        for (int again = 0; again < 2; again++)
            for (int j = 0; j < i; j++) {
                double dot = 0.0;
                for (int t = 0; t < D; t++) dot += r[i * D + t] * r[j * D + t];
                for (int t = 0; t < D; t++) r[i * D + t] -= dot * r[j * D + t];
            }
        double nrm = 0.0;
        for (int t = 0; t < D; t++) nrm += r[i * D + t] * r[i * D + t];
        nrm = sqrt(nrm);
        for (int t = 0; t < D; t++) r[i * D + t] /= nrm;
    }
    memcpy(Q, r, sizeof Q);
}

static void mat_vec(const double *m, const double *v, double *out, int dim) {
    for (int i = 0; i < dim; i++) {
        double s = 0.0;
        for (int j = 0; j < dim; j++) s += m[i * dim + j] * v[j];
        out[i] = s;
    }
}

static void build_corpus(void) {
    for (int i = 0; i < K * D; i++) B[i] = nrand();
    char err[160];
    int rk = ns_orthonormalize(B, D, K, err, sizeof err);
    CHECK(rk == K, "the protected subspace should have full rank");

    make_rotation();
    /* Balanced labels, in RANDOM order.
     *
     * The first version used Y[i] = i % 2. That is balanced and looks harmless,
     * but the embeddings are drawn from a SEQUENTIAL xorshift, so labels become
     * correlated with generation order and any short-lag structure in the
     * generator turns into a signal a linear probe can learn. The symptom was a
     * corpus with no linear encoding scoring 83% "linearly" - which would have
     * overturned the claim this suite exists to make, for a reason that had
     * nothing to do with the mathematics. Shuffle the labels instead. */
    for (int i = 0; i < N; i++) Y[i] = (unsigned char)(i >= N / 2);
    /* Fisher-Yates. The draw must be scaled in DOUBLE before the integer cast:
     * `(unsigned long)(urand()+1.0)*0.5` is 0 for every argument, because a
     * value in (0,1) truncates to zero. That version "shuffled" nothing - it
     * swapped every element with index 0 and left the labels as correlated with
     * row order as before, which is the thing being removed here. */
    for (int i = N - 1; i > 0; i--) {
        const double f = (urand() + 1.0) * 0.5;        /* in (0,1) */
        int j = (int)(f * (double)(i + 1));
        if (j > i) j = i;
        if (j < 0) j = 0;
        const unsigned char t = Y[i]; Y[i] = Y[j]; Y[j] = t;
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < D; j++) X[i * D + j] = nrand();
        /* the attribute is a LINEAR component along the identity direction */
        for (int j = 0; j < D; j++) X[i * D + j] += Y[i] ? 3.0 * B[j] : 0.0;
    }
}

/* The same corpus with the attribute encoded ONLY in the norm.
 *
 * The linear component must be removed FIRST, or the control is confounded: X
 * carries the attribute along B, so normalising X to a target length leaves that
 * component in place and a "linear probe is blind" result would be false. This
 * is the same mistake as leaving an unused term in a conservation model - the
 * test would have passed for the wrong reason. */
static void build_corpus_norm(double *out) {
    double clean[N * D];
    ns_project_out(clean, X, N, B, K, D);      /* strip the linear encoding */

    /* CENTRE, and this is not cosmetic.
     *
     * The first version normalised `clean` straight to a per-class length and
     * the result carried a LINEAR leak, 88% recoverable: an uncentred
     * representation has coordinates whose mean is offset from zero, and
     * scaling rows to different lengths (1.0 vs 2.6) scales that offset
     * differently for each class. The "nonlinear" encoding therefore shows up in
     * the raw coordinates, and a linear probe reads it.
     *
     * That is a real property of real embeddings, not an artifact to hide: they
     * are usually not centred, so norm-based encodings of them leak linearly
     * too. It is also exactly the confound the control must not have, so the
     * corpus is centred over ALL rows (never per class - that would use the
     * label and make the test meaningless). */
    double mean[D] = {0};
    for (int i = 0; i < N; i++)
        for (int j = 0; j < D; j++) mean[j] += clean[i * D + j];
    for (int j = 0; j < D; j++) mean[j] /= (double)N;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < D; j++) clean[i * D + j] -= mean[j];

    for (int i = 0; i < N; i++) {
        double nrm = 0.0;
        for (int j = 0; j < D; j++) nrm += clean[i * D + j] * clean[i * D + j];
        nrm = sqrt(nrm);
        const double target = Y[i] ? 2.6 : 1.0;
        for (int j = 0; j < D; j++)
            out[i * D + j] = clean[i * D + j] * target / (nrm > 0 ? nrm : 1.0);
    }
}

/* ---- 1. the rotation really is an isometry ----------------------------- */

static void test_isometry(void) {
    double err = stiefel_isometry_error(Q, X, N, D);
    CHECK(err < 1e-9, "a Stiefel rotation preserves every inner product");
    /* and it is orthogonal, checked independently of the code under test */
    double worst = 0.0;
    for (int i = 0; i < D; i++)
        for (int j = 0; j < D; j++) {
            double dot = 0.0;
            for (int t = 0; t < D; t++) dot += Q[t * D + i] * Q[t * D + j];
            double want = (i == j) ? 1.0 : 0.0;
            if (fabs(dot - want) > worst) worst = fabs(dot - want);
        }
    CHECK(worst < 1e-12, "the test's own Q is orthogonal (Q^T Q = I)");
}

/* ---- 2. the rotation hides nothing: recover it from known plaintext ----- */

static void test_rotation_recoverable(void) {
    double z[D == 0 ? 1 : N * D];
    char err[160];

    /* What the cloud sees: Q applied to every row. */
    for (int i = 0; i < N; i++) mat_vec(Q, X + i * D, z + i * D, D);

    /* Attacker knows the content of the first D rows (a form header, a footer,
     * a standard clause) and nothing else. */
    double resid = 0.0;
    aios_status rc = stiefel_unknown_rotation_recoverable(X, z, D, D, &resid, err, sizeof err);
    CHECK(rc == AIOS_OK, "recovery of an unknown rotation should succeed");
    CHECK(resid < 1e-6, "with d known plaintexts the rotation is recoverable exactly");

    /* Now use the recovered map to read a row that was NEVER in the known set.
     * This is the step that turns "the map is recoverable" into "the data leaks". */
    double *xtx = calloc(D * D, sizeof *xtx), *xtz = calloc(D, sizeof *xtz);
    double *R = calloc(D * D, sizeof *R);   /* the recovered d x d map */
    CHECK(xtx && xtz && R, "allocation for the attack");
    if (xtx && xtz && R) {
        for (int i = 0; i < D; i++)
            for (int a = 0; a < D; a++) {
                xtz[a] = 0.0;
                for (int b = 0; b < D; b++) xtx[a * D + b] += X[i * D + a] * X[i * D + b];
            }
        for (int c = 0; c < D; c++) {
            for (int a = 0; a < D; a++) {
                xtz[a] = 0.0;
                for (int i = 0; i < D; i++) xtz[a] += X[i * D + a] * z[i * D + c];
            }
            /* solve by explicit Gaussian elimination: independent of the module */
            double *m = malloc(D * (D + 1) * sizeof *m);
            if (!m) continue;
            for (int i = 0; i < D; i++) {
                for (int j = 0; j < D; j++) m[i * (D + 1) + j] = xtx[i * D + j];
                m[i * (D + 1) + D] = xtz[i];
            }
            for (int col = 0; col < D; col++) {
                int piv = col;
                for (int i = col + 1; i < D; i++)
                    if (fabs(m[i * (D + 1) + col]) > fabs(m[piv * (D + 1) + col])) piv = i;
                if (piv != col)
                    for (int j = 0; j <= D; j++) {
                        double t = m[col * (D + 1) + j];
                        m[col * (D + 1) + j] = m[piv * (D + 1) + j];
                        m[piv * (D + 1) + j] = t;
                    }
                const double d0 = m[col * (D + 1) + col];
                for (int j = col; j <= D; j++) m[col * (D + 1) + j] /= d0;
                for (int i = 0; i < D; i++) {
                    if (i == col) continue;
                    const double f = m[i * (D + 1) + col];
                    for (int j = col; j <= D; j++) m[i * (D + 1) + j] -= f * m[col * (D + 1) + j];
                }
            }
            /* column c of R: the map's response to input basis direction c */
            for (int i = 0; i < D; i++) R[c * D + i] = m[i * (D + 1) + D];
            free(m);
        }
        /* apply the recovered map to held-out row N-1 */
        const int held = N - 1;
        double err_max = 0.0;
        for (int t = 0; t < D; t++) {
            double pred = 0.0;
            for (int a = 0; a < D; a++) pred += R[t * D + a] * X[held * D + a];
            const double e = fabs(pred - z[held * D + t]);
            if (e > err_max) err_max = e;
        }
        CHECK(err_max < 1e-6, "a HELD-OUT embedding is readable once the rotation is known");
        free(xtx); free(xtz); free(R);
    }
}

/* ---- 3. projection removes a linear attribute ... ---------------------- */

static void test_projection_removes_linear(void) {
    double proj[N * D];
    ns_corpus raw = { X, Y, N, D }, after;
    char err[160], what[512];
    int lin = -1, quad = -1;

    CHECK(ns_project_out(proj, X, N, B, K, D) == AIOS_OK, "projection runs");
    after = (ns_corpus){ proj, Y, N, D };

    CHECK(ns_probe_linear(&raw, &lin, err, sizeof err) == AIOS_OK, "raw linear probe runs");
    CHECK(lin > 8000, "before projection the attribute is linearly obvious (>80%)");
    CHECK(ns_probe_linear(&after, &lin, err, sizeof err) == AIOS_OK, "projected linear probe runs");
    CHECK(lin < 6000, "after projection a LINEAR probe is near chance");
    CHECK(ns_probe_quadratic(&after, &quad, err, sizeof err) == AIOS_OK, "projected quadratic probe runs");
    /* for a genuinely linear attribute the quadratic probe also fails: there is
       nothing nonlinear left to find. */
    CHECK(quad < 6500, "a linear attribute is gone after projecting its direction");
    CHECK(ns_certify(&after, 1000, what, sizeof what), "this pipeline IS certifiable");
    CHECK(strstr(what, "certified for THIS corpus") != NULL, "and the certificate says so plainly");
}

/* ---- 4. ... and does nothing to one encoded in the norm ---------------- */

static void test_projection_vs_norm(void) {
    double normcorpus[N * D], proj[N * D];
    char err[160], what[512];
    int lin = -1, quad = -1;
    ns_corpus raw, after;

    build_corpus_norm(normcorpus);
    raw = (ns_corpus){ normcorpus, Y, N, D };
    CHECK(ns_project_out(proj, normcorpus, N, B, K, D) == AIOS_OK, "projection of the norm corpus runs");
    after = (ns_corpus){ proj, Y, N, D };

    CHECK(ns_probe_linear(&raw, &lin, err, sizeof err) == AIOS_OK, "norm corpus linear probe runs");
    CHECK(lin < 6500, "a norm-encoded attribute is invisible to a linear probe");

    CHECK(ns_probe_linear(&after, &lin, err, sizeof err) == AIOS_OK, "projected norm corpus linear probe runs");
    CHECK(lin < 6500, "and projection does not change that - it looks private");
    CHECK(ns_probe_quadratic(&after, &quad, err, sizeof err) == AIOS_OK, "projected norm corpus quadratic probe runs");
    CHECK(quad > 8500, "but the QUADRATIC probe recovers it almost perfectly");

    CHECK(!ns_certify(&after, 1000, what, sizeof what),
          "a projection that leaves the norm untouched must NOT be certified");
    CHECK(strstr(what, "REFUSED") != NULL, "and the refusal is explicit");
    CHECK(strstr(what, "quadratic") != NULL, "naming the probe that caught it");
}

/* ---- 5. the vendor pipeline: rotate only ------------------------------- */

static void test_rotate_only_refused(void) {
    double z[N * D];
    ns_corpus rotated;
    char what[512], err[160];
    int lin = -1, quad = -1;

    for (int i = 0; i < N; i++) mat_vec(Q, X + i * D, z + i * D, D);
    rotated = (ns_corpus){ z, Y, N, D };

    /* The proposal's own claim, tested: rotating preserves inner products, so the
     * cloud can reason. It also preserves the attribute's recoverability. */
    CHECK(stiefel_isometry_error(Q, X, N, D) < 1e-9, "attention structure survives the rotation");
    CHECK(ns_probe_linear(&rotated, &lin, err, sizeof err) == AIOS_OK, "rotated linear probe runs");
    CHECK(lin > 8000, "and the cloud can still read the attribute at >80% linear accuracy");
    CHECK(ns_probe_quadratic(&rotated, &quad, err, sizeof err) == AIOS_OK, "rotated quadratic probe runs");
    CHECK(!ns_certify(&rotated, 1000, what, sizeof what),
          "rotation alone is REFUSED: it is a bijection, not a de-identification");
}

/* ---- 6. the auditor fails closed --------------------------------------- */

static void test_fails_closed(void) {
    char what[512], err[160];
    ns_corpus tiny = { X, Y, 2, D };
    ns_corpus nodata = { NULL, Y, N, D };
    ns_corpus raw = { X, Y, N, D };

    CHECK(!ns_certify(&nodata, 1000, what, sizeof what), "no corpus -> refused");
    CHECK(strstr(what, "refused") != NULL, "and says it was refused");
    CHECK(ns_probe_linear(&tiny, &(int){0}, err, sizeof err) == AIOS_ERR_ARG,
          "a corpus too small for a split is an argument error, not a pass");
    CHECK(!ns_certify(&tiny, 1000, what, sizeof what),
          "a corpus the audit cannot split must fail CLOSED, not certify");
    CHECK(strstr(what, "could not run") != NULL || strstr(what, "refused") != NULL,
          "an audit that cannot run says so instead of reporting safe");

    /* the raw corpus is refused too - this is the control that shows the probe
     * is not simply always saying yes */
    CHECK(!ns_certify(&raw, 1000, what, sizeof what), "an unprotected corpus is refused");
}

/* ---- 7. orthonormaliser refuses to over-remove ------------------------- */

static void test_rank_deficient_basis(void) {
    double dep[3 * D];
    char err[160];
    /* two identical rows: the span is 1-dimensional, not 2 */
    for (int j = 0; j < D; j++) { dep[j] = nrand(); dep[2 * D + j] = nrand(); }
    for (int j = 0; j < D; j++) dep[D + j] = dep[j];
    int rk = ns_orthonormalize(dep, D, 3, err, sizeof err);
    CHECK(rk == 2, "a dependent row is dropped and the true rank reported");
    /* the dropped row must be zeroed, not left arbitrary: a projector built from
       it would remove a direction that is not in the protected subspace */
    double nrm = 0.0;
    for (int j = 0; j < D; j++) nrm += dep[2 * D + j] * dep[2 * D + j];
    CHECK(nrm == 0.0, "the dropped row is zeroed, so it cannot over-remove");
}


/* ---- 8. the split must not correlate with row order -------------------- */

/* A corpus sorted by label: every class-0 row first, then every class-1 row.
 *
 * This is not a contrived input - it is what a corpus exported from a records
 * system looks like. The auditor's first version split on `i & 1`, so every
 * training fold contained exactly one class, the readout learned a constant, and
 * the probe reported chance for an attribute that was plainly present. The
 * audit would then have CERTIFIED a leaking pipeline, which is the worst failure
 * this module can produce.
 *
 * The sabotage matrix caught this: reverting the split to `i & 1` caused zero
 * failures, because every other test uses shuffled labels. A test corpus that
 * cannot distinguish the buggy split from the correct one is not testing the
 * split. This is that corpus. */
static void test_split_not_correlated_with_order(void) {
    double x[N * D];
    unsigned char ys[N];
    char err[160], what[512];
    int lin = -1;
    ns_corpus c;

    for (int i = 0; i < N; i++) {
        ys[i] = (unsigned char)(i >= N / 2);           /* sorted, NOT shuffled */
        for (int j = 0; j < D; j++)
            x[i * D + j] = nrand() + (ys[i] ? 3.0 : 0.0);
    }
    c = (ns_corpus){ x, ys, N, D };

    CHECK(ns_probe_linear(&c, &lin, err, sizeof err) == AIOS_OK,
          "probe runs on a label-ordered corpus");
    CHECK(lin > 8000,
          "a label-ordered corpus must not hide a 3-sigma shift behind the split");
    CHECK(!ns_certify(&c, 1000, what, sizeof what),
          "and it must be refused, not certified");
}

/* ---- 9. an alternating-label corpus must not report nonsense ----------- */

/* Labels alternate (0,1,0,1,...) with a real 1-sigma shift in every coordinate.
 *
 * Under the old `i & 1` split this reported an accuracy of ZERO: every even row
 * was class 0 and every odd row class 1, so both folds were single-class, the
 * readout learned a constant, and both scored 0%. max() then returned 0.
 *
 * Zero accuracy is perfectly anti-predictive, not "no information", and a caller
 * writing `lin < 6500` reads it as privacy when it means the opposite. It is a
 * fail-open in the auditor - the one component that must not have one. The
 * stratified split reports 92.5%, which is the truth.
 *
 * Note the signal has to be modest: with a large shift the sorted-corpus test
 * catches the bug instead, but ALTERNATING labels are the case where the old
 * split was maximally wrong and nothing else noticed. */
static void test_alternating_labels(void) {
    double x[N * D];
    unsigned char ys[N];
    char err[160], what[512];
    int lin = -1;
    ns_corpus c;

    for (int i = 0; i < N; i++) {
        ys[i] = (unsigned char)(i % 2);
        for (int j = 0; j < D; j++)
            x[i * D + j] = nrand() + (ys[i] ? 1.0 : 0.0);
    }
    c = (ns_corpus){ x, ys, N, D };

    CHECK(ns_probe_linear(&c, &lin, err, sizeof err) == AIOS_OK,
          "probe runs on an alternating-label corpus");
    CHECK(lin > 2500,
          "accuracy must never come back as ~0% (a single-class-fold artifact)");
    CHECK(lin > 6500,
          "and a real shift must be reported as recoverable, not as privacy");
    CHECK(!ns_certify(&c, 1000, what, sizeof what),
          "so this corpus is refused rather than certified");
}

int main(int argc, char **argv) {
    (void)argc;
    g_argv0 = argv[0];

    build_corpus();
    test_isometry();
    test_rotation_recoverable();
    test_projection_removes_linear();
    test_projection_vs_norm();
    test_rotate_only_refused();
    test_fails_closed();
    test_rank_deficient_basis();
    test_split_not_correlated_with_order();
    test_alternating_labels();

    fprintf(stderr, "-- %s: %d checks, %d failed\n",
            g_argv0, g_checks, g_failed);
    return g_failed ? 1 : 0;
}
