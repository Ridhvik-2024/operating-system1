/* apps/nullspace-demo.c — item #17, demonstrated.
 *
 * Prints what the proposal claims, then measures it. Every number below comes
 * from the same code the test suite exercises; nothing here is a mock.
 *
 * The point of the exercise: the proposal's headline is not a small overstatement
 * that needs tuning. Two of its three claims are false for a reason that is a
 * theorem, and the third (the useful one) is not the one being advertised.
 */
#include "aios/nullspace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 200
#define D 8
#define K 3

static unsigned long g_s = 0x9E3779B97F4A7C15UL;
static double urand(void) {
    g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17;
    return ((double)(g_s >> 11) / (double)(1UL << 52)) - 1.0;
}
static double nrand(void) {
    double s = 0.0;
    for (int i = 0; i < 12; i++) s += urand();
    return (s - 6.0) / 2.0;
}

static double X[N * D], B[K * D], Q[D * D];
static unsigned char Y[N];

static int g_checks, g_failed;
static void check(int ok, const char *what) {
    g_checks++;
    printf("   %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

static void make_rotation(void) {
    double r[D * D];
    for (int i = 0; i < D * D; i++) r[i] = nrand();
    for (int i = 0; i < D; i++) {
        for (int pass = 0; pass < 2; pass++)
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

static void mat_vec(const double *m, const double *v, double *out) {
    for (int i = 0; i < D; i++) {
        double s = 0.0;
        for (int j = 0; j < D; j++) s += m[i * D + j] * v[j];
        out[i] = s;
    }
}

/* Gaussian elimination, so the demo's recovery does not borrow the module's
 * solver. If the attack only worked with our own arithmetic, it would prove
 * nothing about an attacker's. */
static int solve(double *a, double *b, double *x, int n) {
    for (int col = 0; col < n; col++) {
        int piv = col;
        for (int i = col + 1; i < n; i++)
            if (fabs(a[i * n + col]) > fabs(a[piv * n + col])) piv = i;
        if (fabs(a[piv * n + col]) < 1e-12) return 0;
        if (piv != col) {
            for (int j = 0; j < n; j++) { double t = a[col * n + j]; a[col * n + j] = a[piv * n + j]; a[piv * n + j] = t; }
            { double t = b[col]; b[col] = b[piv]; b[piv] = t; }
        }
        const double d0 = a[col * n + col];
        for (int j = col; j < n; j++) a[col * n + j] /= d0;
        b[col] /= d0;
        for (int i = 0; i < n; i++) {
            if (i == col) continue;
            const double f = a[i * n + col];
            if (f == 0.0) continue;
            for (int j = col; j < n; j++) a[i * n + j] -= f * a[col * n + j];
            b[i] -= f * b[col];
        }
    }
    for (int i = 0; i < n; i++) x[i] = b[i];
    return 1;
}

int main(void) {
    char err[200], what[600];

    /* ---- the corpus ---------------------------------------------------- */
    for (int i = 0; i < K * D; i++) B[i] = nrand();
    if (ns_orthonormalize(B, D, K, err, sizeof err) != K) {
        printf("could not build the identity subspace: %s\n", err);
        return 1;
    }
    make_rotation();
    /* balanced labels in random order (a label-correlated row order is itself a
     * leak, and would confound every measurement below - see test_nullspace.c) */
    for (int i = 0; i < N; i++) Y[i] = (unsigned char)(i >= N / 2);
    for (int i = N - 1; i > 0; i--) {
        const double f = (urand() + 1.0) * 0.5;
        int j = (int)(f * (double)(i + 1));
        if (j > i) j = i;
        const unsigned char t = Y[i]; Y[i] = Y[j]; Y[j] = t;
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < D; j++) X[i * D + j] = nrand();
        for (int j = 0; j < D; j++) X[i * D + j] += Y[i] ? 3.0 * B[j] : 0.0;
    }

    printf("================================================================\n");
    printf(" item #17, measured\n");
    printf("================================================================\n\n");
    printf(" corpus: %d embeddings of dimension %d; the \"PII\" is a 3-sigma shift\n", N, D);
    printf(" along a 3-dimensional protected subspace (a linear encoding).\n\n");

    /* ---- PART 1: the true half ----------------------------------------- */
    printf("PART 1 - a Stiefel rotation IS an isometry (the proposal is right)\n");
    const double iso = stiefel_isometry_error(Q, X, N, D);
    printf("   max |<Qx,Qy> - <x,y>| over all pairs = %.2e\n", iso);
    check(iso < 1e-9, "attention angles survive the rotation, so the cloud can reason");
    printf("   -> this part of the proposal is correct, and is what makes PART 2\n");
    printf("      possible rather than avoidable.\n\n");

    /* ---- PART 2: the false half ---------------------------------------- */
    printf("PART 2 - the same isometry is a BIJECTION, so it hides nothing\n");
    double z[N * D];
    for (int i = 0; i < N; i++) mat_vec(Q, X + i * D, z + i * D);

    double resid = 0.0;
    int ok = (stiefel_unknown_rotation_recoverable(X, z, D, D, &resid, err, sizeof err) == AIOS_OK);
    printf("   attacker knows the content of %d embeddings (a form header,\n", D);
    printf("   a footer, a standard clause) and sees the cloud's view:\n");
    printf("   residual of the best-fitting linear map = %.2e\n", resid);
    check(ok && resid < 1e-6, "the secret rotation is recoverable exactly from known plaintext");

    /* read a HELD-OUT row with the recovered map: this is the leak, not the theory */
    double *xtx = calloc(D * D, sizeof *xtx), *xtz = calloc(D, sizeof *xtz);
    double *R = calloc(D * D, sizeof *R);
    int read_ok = 0;
    if (xtx && xtz && R) {
        for (int i = 0; i < D; i++)
            for (int a = 0; a < D; a++)
                for (int b = 0; b < D; b++) xtx[a * D + b] += X[i * D + a] * X[i * D + b];
        for (int c = 0; c < D; c++) {
            double *a = malloc(D * D * sizeof *a), *bb = malloc(D * sizeof *bb), *sol = malloc(D * sizeof *sol);
            if (!a || !bb || !sol) { free(a); free(bb); free(sol); break; }
            memcpy(a, xtx, D * D * sizeof *a);
            for (int i = 0; i < D; i++) { bb[i] = 0.0; for (int t = 0; t < D; t++) bb[i] += X[t * D + i] * z[t * D + c]; }
            if (solve(a, bb, sol, D)) { for (int i = 0; i < D; i++) R[c * D + i] = sol[i]; read_ok = 1; }
            free(a); free(bb); free(sol);
        }
    }
    if (read_ok) {
        const int held = N - 1;   /* never in the attacker's known set */
        double worst = 0.0;
        for (int t = 0; t < D; t++) {
            double pred = 0.0;
            for (int a = 0; a < D; a++) pred += R[t * D + a] * X[held * D + a];
            const double e = fabs(pred - z[held * D + t]);
            if (e > worst) worst = e;
        }
        /* RELATIVE error: the embeddings are O(10), so an absolute tolerance
         * says more about their scale than about the attack. The recovered map
         * is exact up to floating-point conditioning. */
        double zscale = 1.0;
        for (int t = 0; t < D; t++)
            if (fabs(z[held * D + t]) > zscale) zscale = fabs(z[held * D + t]);
        printf("   held-out embedding recovered to a relative error of %.2e\n", worst / zscale);
        /* normal equations square the condition number, so ~1e-6 relative is
         * the solver's floor, not a limit on the attack: the map is exact and
         * a held-out document comes back to six significant figures. */
        check(worst / zscale < 1e-5, "a document that was NOT in the known set is read in full");
    }
    free(xtx); free(xtz); free(R);

    int lin = -1, quad = -1;
    ns_corpus rotated = { z, Y, N, D };
    ns_probe_linear(&rotated, &lin, err, sizeof err);
    printf("   attribute recovery from the cloud's view: linear %d.%02d%%\n", lin / 100, lin % 100);
    check(lin > 8000, "so the cloud can read the \"protected\" attribute directly");
    printf("   -> a rotation is not encryption. There is no key here to withhold:\n");
    printf("      the map is linear, low-dimensional, and fully determined by content\n");
    printf("      the attacker already has. \"Un-rotatable by anyone but the phone\"\n");
    printf("      assumes the phone is the only party that can guess a letterhead.\n\n");

    /* ---- PART 3: the useful half --------------------------------------- */
    printf("PART 3 - what CAN be proven: an auditor that refuses on a number\n");

    /* the honest pipeline: project the identity direction out */
    double proj[N * D];
    ns_project_out(proj, X, N, B, K, D);
    ns_corpus after = { proj, Y, N, D };

    ns_probe_linear(&after, &lin, err, sizeof err);
    ns_probe_quadratic(&after, &quad, err, sizeof err);
    char line[300];
    ns_describe("projection (linear PII)", lin, quad, 5000, line, sizeof line);
    printf("   %s\n", line);
    check(ns_certify(&after, 1000, what, sizeof what), "projection of a linearly encoded attribute certifies");
    printf("   -> %s\n", what);

    /* the pipeline that only LOOKS private: attribute encoded in the norm */
    double clean[N * D], normc[N * D];
    ns_project_out(clean, X, N, B, K, D);
    double mean[D] = {0};
    for (int i = 0; i < N; i++) for (int j = 0; j < D; j++) mean[j] += clean[i * D + j];
    for (int j = 0; j < D; j++) mean[j] /= (double)N;
    for (int i = 0; i < N; i++) for (int j = 0; j < D; j++) clean[i * D + j] -= mean[j];
    for (int i = 0; i < N; i++) {
        double nrm = 0.0;
        for (int j = 0; j < D; j++) nrm += clean[i * D + j] * clean[i * D + j];
        nrm = sqrt(nrm);
        const double target = Y[i] ? 2.6 : 1.0;
        for (int j = 0; j < D; j++) normc[i * D + j] = clean[i * D + j] * target / (nrm > 0 ? nrm : 1.0);
    }
    ns_corpus sneaky = { normc, Y, N, D };
    ns_probe_linear(&sneaky, &lin, err, sizeof err);
    ns_probe_quadratic(&sneaky, &quad, err, sizeof err);
    ns_describe("projection (norm PII)", lin, quad, 5000, line, sizeof line);
    printf("   %s\n", line);
    printf("   the linear probe is at chance. A vendor shipping only that probe\n");
    printf("   would report \"no PII recoverable\". The quadratic probe reads it\n");
    printf("   perfectly, because the norm is a nonlinear function of the\n");
    printf("   embedding and projection does not touch it.\n");
    check(lin < 6500, "the linear probe says the attribute is gone");
    check(quad > 9000, "the quadratic probe says it is not");
    check(!ns_certify(&sneaky, 1000, what, sizeof what), "so the auditor REFUSES it");
    printf("   -> %s\n\n", what);

    /* ---- the verdict --------------------------------------------------- */
    printf("================================================================\n");
    printf(" WHAT THIS MILESTONE DELIVERS\n");
    printf("================================================================\n");
    printf(" Built: an auditor that measures identity recovery with a linear AND\n");
    printf(" a quadratic probe, and refuses to certify unless both are at chance.\n");
    printf(" It works on the OUTPUT of any pipeline, including a vendor's, because\n");
    printf(" it measures rather than trusts.\n\n");
    printf(" Not built, and not claimable: I(Z_cloud; PII) = 0. A projection onto\n");
    printf(" an orthogonal complement removes the LINEAR component of an attribute\n");
    printf(" and leaves every nonlinear function of it intact. Mutual information\n");
    printf(" does not reach zero because the variable is still in the residual.\n\n");
    printf(" And the headline that has to be given up: \"zero mathematical\n");
    printf(" possibility of data leakage\". A rotation is a bijection; its inverse\n");
    printf(" needs only n guesses the attacker was going to make anyway. This is\n");
    printf(" not an implementation detail that a better key would fix. It is what\n");
    printf(" a rotation is.\n\n");
    printf(" What is true and useful: the isometry (attention survives), and the\n");
    printf(" audit (a pipeline that only looks private is caught). The proposal's\n");
    printf(" 99.2%% certainty is not supported by its own geometry.\n");

    printf("\n[demo] %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
