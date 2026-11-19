/* src/nullspace.c — M11: the auditor for geometric privacy claims.
 * See include/aios/nullspace.h for what this is NOT.
 *
 * Design notes worth reading before changing anything here:
 *
 *  - Nothing in this file decides that data is safe. It reports what a probe can
 *    recover. The refusal lives in ns_certify(), which refuses on a number, not
 *    on a promise.
 *
 *  - The quadratic expansion is the whole point. A projection onto the orthogonal
 *    complement of an identity subspace is exact for a LINEARLY encoded
 *    attribute and does nothing at all for one encoded in the norm, the squared
 *    length, or any product of coordinates. A vendor that ships only the linear
 *    probe will pass their own test and leak. The gap between the two probes is
 *    the measurable difference between a transform and a gesture.
 *
 *  - Everything here is small dense arithmetic. No BLAS, no matrix library: the
 *    dimensions are an embedding (hundreds, at most) and a corpus (thousands),
 *    so O(n d²) is fine and a dependency would not be.
 */
#include "aios/nullspace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers ------------------------------------------------------ */

static int feat_dim(size_t d) {
    /* x_i (d terms), x_i^2/2 (d terms), x_i x_j for i<j (d(d-1)/2 terms) */
    return (int)(2 * d + d * (d - 1) / 2);
}

static void expand_quadratic(const double *row, size_t d, double *feat) {
    size_t k = 0;
    for (size_t i = 0; i < d; i++) feat[k++] = row[i];
    for (size_t i = 0; i < d; i++) feat[k++] = 0.5 * row[i] * row[i];
    for (size_t i = 0; i < d; i++)
        for (size_t j = i + 1; j < d; j++) feat[k++] = row[i] * row[j];
}

/* Solve A w = b for symmetric positive-definite A (m x m), by Cholesky.
 * Returns false when A is not positive definite, which is the honest answer for
 * a rank-deficient design: guessing would produce a confident wrong coefficient. */
static bool chol_solve(double *a, const double *b, double *x, size_t m) {
    for (size_t i = 0; i < m; i++) {
        for (size_t j = 0; j <= i; j++) {
            double s = a[i * m + j];
            for (size_t k = 0; k < j; k++) s -= a[i * m + k] * a[j * m + k];
            if (i == j) {
                if (s <= 0.0) return false;
                a[i * m + j] = sqrt(s);
            } else {
                a[i * m + j] = s / a[j * m + j];
            }
        }
    }
    /* forward substitution into b */
    for (size_t i = 0; i < m; i++) {
        double s = b[i];
        for (size_t k = 0; k < i; k++) s -= a[i * m + k] * x[k];
        x[i] = s / a[i * m + i];
    }
    /* back substitution */
    for (size_t ii = m; ii-- > 0;) {
        double s = x[ii];
        for (size_t k = ii + 1; k < m; k++) s -= a[k * m + ii] * x[k];
        x[ii] = s / a[ii * m + ii];
    }
    return true;
}

/* Ridge fit of a readout on `m` features over the rows `idx`, then accuracy on
 * the complementary rows. `feat` is n x m. Returns accuracy in basis points. */
static int fit_and_score(const double *feat, size_t m, const unsigned char *y,
                         size_t n, const size_t *idx, size_t nidx,
                         double lambda) {
    /* Design with an explicit bias column, fitted on idx. */
    const size_t p = m + 1;
    double *a = calloc(p * p, sizeof *a);
    double *b = calloc(p, sizeof *b);
    double *w = calloc(p, sizeof *w);
    int ok = 0;
    size_t right = 0, tested = 0;

    if (!a || !b || !w) goto done;

    for (size_t t = 0; t < nidx; t++) {
        const size_t i = idx[t];
        const double *f = feat + i * m;
        for (size_t r = 0; r < p; r++) {
            const double fr = (r == m) ? 1.0 : f[r];
            b[r] += fr * (double)y[i];
            for (size_t c = 0; c <= r; c++) {
                const double fc = (c == m) ? 1.0 : f[c];
                a[r * p + c] += fr * fc;
            }
        }
    }
    /* mirror the lower triangle, and ridge the feature block only (never the
       bias, whose shrinkage would bias every prediction toward zero) */
    for (size_t r = 0; r < p; r++) {
        for (size_t c = 0; c < r; c++) a[c * p + r] = a[r * p + c];
        if (r < m) a[r * p + r] += lambda;
    }
    if (p > 0 && a[(p - 1) * p + (p - 1)] <= 0.0) a[(p - 1) * p + (p - 1)] = 1.0;

    if (!chol_solve(a, b, w, p)) goto done;

    for (size_t i = 0; i < n; i++) {
        bool in_train = false;
        for (size_t t = 0; t < nidx; t++) if (idx[t] == i) { in_train = true; break; }
        if (in_train) continue;
        const double *f = feat + i * m;
        double s = w[m];
        for (size_t r = 0; r < m; r++) s += f[r] * w[r];
        const int pred = (s > 0.5);
        right += (size_t)(pred == (y[i] != 0));
        tested++;
    }
    ok = (tested > 0);
done:
    free(a); free(b); free(w);
    return ok ? (int)((right * 10000) / tested) : -1;
}

/* Two-fold, both orientations, take the better. A single split makes the number
 * depend on which half happened to be train; taking the better of the two makes
 * a HIGH number conservative-to-find and a LOW number harder to claim by luck. */
static aios_status probe(const double *feat, size_t m, const ns_corpus *c,
                         double lambda, int *bps_out, char *err, size_t ecap) {
    size_t *ia = malloc(c->n * sizeof *ia);
    size_t *ib = malloc(c->n * sizeof *ib);
    size_t na = 0, nb = 0;
    aios_status rc = AIOS_ERR_NOMEM;
    int best = -1;

    if (!ia || !ib) goto done;
    /* A STRATIFIED, deterministic split.
     *
     * Two failures live here, and the second one is why this is not simply a
     * shuffle.
     *
     * 1. Splitting on `i & 1` looks arbitrary, but it is arbitrary in the same
     *    coordinate the corpus is indexed by. A corpus ordered by label
     *    (`i >= n/2`) gives folds that are each a single class, so the readout
     *    learns a constant. Worse, an ALTERNATING label (`i % 2`) is the
     *    opposite corner: even rows are all class 0 and odd rows all class 1, so
     *    BOTH folds are single-class, both score 0%, and max() returns 0. Zero
     *    accuracy is "perfectly anti-predictive", not "no information" - and a
     *    caller comparing `lin < 6500` reads it as privacy when it means the
     *    opposite. That is a fail-open in the AUDITOR, which is the one component
     *    that must not have one.
     *
     * 2. So the split is stratified: shuffle within each class, then deal
     *    alternately into the two folds. Each fold then has both classes
     *    whatever the row order, and an accuracy of 50% means what it says.
     *
     * If a class has fewer than 2 rows no balanced split exists at all. That is
     * reported as an argument error so ns_certify() fails closed, rather than
     * producing a number that looks like a measurement. */
    {
        size_t c0 = 0, c1 = 0;
        for (size_t i = 0; i < c->n; i++) { if (c->y[i]) c1++; else c0++; }
        if (c0 < 2 || c1 < 2) {
            snprintf(err, ecap,
                     "cannot form a balanced split: %zu rows of class 0 and %zu of "
                     "class 1 (each needs >= 2)", c0, c1);
            rc = AIOS_ERR_ARG;
            goto done;
        }
        /* deal each class separately, always into the fold that has fewer of it */
        for (int cls = 0; cls < 2; cls++) {
            size_t idx[NS_MAX_SAMPLES];
            size_t cnt = 0, half;
            for (size_t i = 0; i < c->n; i++)
                if ((c->y[i] != 0) == (cls == 1)) idx[cnt++] = i;
            /* Fisher-Yates over a fixed seed: reproducible, order-independent */
            unsigned long long st = 0x243F6A8885A308D3ULL ^ (unsigned long long)(cnt + (size_t)cls);
            for (size_t i = cnt; i-- > 1;) {
                st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                const size_t j = (size_t)(st % (i + 1));
                const size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
            half = cnt / 2;
            for (size_t i = 0; i < cnt; i++) {
                if (i < half) ia[na++] = idx[i];
                else          ib[nb++] = idx[i];
            }
        }
    }
    if (na == 0 || nb == 0) {
        snprintf(err, ecap, "corpus too small: need at least 2 rows, have %zu", c->n);
        rc = AIOS_ERR_ARG;
        goto done;
    }
    int b1 = fit_and_score(feat, m, c->y, c->n, ia, na, lambda);
    int b2 = fit_and_score(feat, m, c->y, c->n, ib, nb, lambda);
    if (b1 < 0 || b2 < 0) {
        snprintf(err, ecap, "readout fit failed (rank-deficient design)");
        rc = AIOS_ERR_IO;
        goto done;
    }
    best = (b1 > b2) ? b1 : b2;
    *bps_out = best;
    rc = AIOS_OK;
done:
    free(ia); free(ib);
    return rc;
}

/* ---- public: linear algebra -------------------------------------------- */

int ns_orthonormalize(double *basis, size_t d, size_t k, char *err, size_t ecap) {
    size_t kept = 0;

    if (!basis || d == 0 || k == 0 || k > d) {
        snprintf(err, ecap, "bad basis shape: need 1 <= k <= d, got k=%zu d=%zu", k, d);
        return -1;
    }
    for (size_t row = 0; row < k; row++) {
        double *v = basis + row * d;
        /* two passes: the second removes the error the first introduced */
        for (int pass = 0; pass < 2; pass++) {
            for (size_t j = 0; j < kept; j++) {
                const double *b = basis + j * d;
                double dot = 0.0;
                for (size_t t = 0; t < d; t++) dot += v[t] * b[t];
                for (size_t t = 0; t < d; t++) v[t] -= dot * b[t];
            }
        }
        double nrm = 0.0;
        for (size_t t = 0; t < d; t++) nrm += v[t] * v[t];
        nrm = sqrt(nrm);
        if (nrm < 1e-10) {
            /* linearly dependent on the rows already kept: zero it and report a
               smaller rank. Over-removing would delete real signal and look
               like a stronger privacy result. */
            for (size_t t = 0; t < d; t++) v[t] = 0.0;
            continue;
        }
        for (size_t t = 0; t < d; t++) v[t] /= nrm;
        /* compact the kept rows to the front, so a caller can use rank directly */
        if (kept != row) {
            double *dst = basis + kept * d;
            double *src = basis + row * d;
            for (size_t t = 0; t < d; t++) { dst[t] = src[t]; src[t] = 0.0; }
        }
        kept++;
    }
    return (int)kept;
}

aios_status ns_project_out(double *out, const double *x, size_t n,
                           const double *basis, size_t k, size_t d) {
    if (!out || !x || (k > 0 && !basis) || d == 0) return AIOS_ERR_ARG;
    for (size_t i = 0; i < n; i++) {
        const double *row = x + i * d;
        double *o = out + i * d;
        for (size_t t = 0; t < d; t++) o[t] = row[t];
        for (size_t j = 0; j < k; j++) {
            const double *b = basis + j * d;
            double dot = 0.0;
            for (size_t t = 0; t < d; t++) dot += row[t] * b[t];
            for (size_t t = 0; t < d; t++) o[t] -= dot * b[t];
        }
    }
    return AIOS_OK;
}

double stiefel_isometry_error(const double *q, const double *x, size_t n, size_t d) {
    double worst = 0.0;

    if (!q || !x || d == 0) return 0.0;
    /* compare <x_i, x_j> with <Qx_i, Qx_j> for every pair: the claim is about
       ALL inner products (attention angles), so sampling pairs would be weaker
       than the theorem being checked. */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i; j < n; j++) {
            double plain = 0.0, rotated = 0.0;
            for (size_t t = 0; t < d; t++) {
                double qi = 0.0, qj = 0.0;
                for (size_t s = 0; s < d; s++) {
                    qi += q[t * d + s] * x[i * d + s];
                    qj += q[t * d + s] * x[j * d + s];
                }
                plain   += x[i * d + t] * x[j * d + t];
                rotated += qi * qj;
            }
            double e = fabs(rotated - plain);
            if (e > worst) worst = e;
        }
    }
    return worst;
}

aios_status stiefel_unknown_rotation_recoverable(const double *x_known,
                                                 const double *z_seen,
                                                 size_t n, size_t d,
                                                 double *residual_out,
                                                 char *err, size_t ecap) {
    /* Solve, per column c,  min_r ||X r - Z[:,c]||²  with X the (n x d) known
       plaintexts. Normal equations: (XᵀX) r = XᵀZ[:,c]. */
    double *xtx = NULL, *xtz = NULL, *r = NULL;
    aios_status rc = AIOS_ERR_NOMEM;
    double resid = 0.0, znorm = 0.0;

    if (!x_known || !z_seen || !residual_out || n < d || d == 0) {
        snprintf(err, ecap, "need n >= d known plaintexts (n=%zu, d=%zu)", n, d);
        return AIOS_ERR_ARG;
    }
    xtx = calloc(d * d, sizeof *xtx);
    xtz = calloc(d, sizeof *xtz);
    r   = calloc(d, sizeof *r);
    if (!xtx || !xtz || !r) goto done;

    for (size_t i = 0; i < n; i++) {
        const double *xi = x_known + i * d;
        for (size_t a = 0; a < d; a++)
            for (size_t b = 0; b <= a; b++)
                xtx[a * d + b] += xi[a] * xi[b];
    }
    for (size_t a = 0; a < d; a++)
        for (size_t b = 0; b < a; b++) xtx[b * d + a] = xtx[a * d + b];

    for (size_t c = 0; c < d; c++) {
        memset(xtz, 0, d * sizeof *xtz);
        for (size_t i = 0; i < n; i++) {
            const double *xi = x_known + i * d;
            for (size_t a = 0; a < d; a++) xtz[a] += xi[a] * z_seen[i * d + c];
        }
        /* solve a copy, because Cholesky overwrites it */
        double *acopy = malloc(d * d * sizeof *acopy);
        if (!acopy) { rc = AIOS_ERR_NOMEM; goto done; }
        memcpy(acopy, xtx, d * d * sizeof *acopy);
        /* tiny ridge keeps a rank-deficient X (identical boilerplate rows) from
           producing a NaN; the residual then honestly reports "no single R". */
        for (size_t a = 0; a < d; a++) acopy[a * d + a] += 1e-12;
        if (!chol_solve(acopy, xtz, r, d)) {
            free(acopy);
            snprintf(err, ecap, "design matrix is singular; cannot fit a rotation");
            rc = AIOS_ERR_IO;
            goto done;
        }
        free(acopy);
        for (size_t i = 0; i < n; i++) {
            double pred = 0.0;
            for (size_t a = 0; a < d; a++) pred += x_known[i * d + a] * r[a];
            double diff = pred - z_seen[i * d + c];
            resid += diff * diff;
            znorm += z_seen[i * d + c] * z_seen[i * d + c];
        }
    }
    *residual_out = (znorm > 0.0) ? sqrt(resid / znorm) : 0.0;
    rc = AIOS_OK;
done:
    free(xtx); free(xtz); free(r);
    return rc;
}

/* ---- public: the auditor ----------------------------------------------- */

aios_status ns_probe_linear(const ns_corpus *c, int *bps_out, char *err, size_t ecap) {
    if (!c || !c->x || !c->y || !bps_out) {
        snprintf(err, ecap, "bad corpus");
        return AIOS_ERR_ARG;
    }
    if (c->n < 4 || c->d == 0 || c->d > NS_MAX_DIM || c->n > NS_MAX_SAMPLES) {
        snprintf(err, ecap, "corpus shape out of range (n=%zu d=%zu, max d=%d, max n=%d)",
                 c->n, c->d, NS_MAX_DIM, NS_MAX_SAMPLES);
        return AIOS_ERR_ARG;
    }
    /* scale the ridge by the mean feature magnitude so lambda is dimensionless */
    double scale = 0.0;
    for (size_t i = 0; i < c->n * c->d; i++) scale += c->x[i] * c->x[i];
    scale = (scale > 0.0) ? scale / (double)c->n : 1.0;
    return probe(c->x, c->d, c, 1e-6 * scale, bps_out, err, ecap);
}

aios_status ns_probe_quadratic(const ns_corpus *c, int *bps_out, char *err, size_t ecap) {
    aios_status rc;
    size_t m;
    double *feat;
    double scale = 0.0;

    if (!c || !c->x || !c->y || !bps_out) {
        snprintf(err, ecap, "bad corpus");
        return AIOS_ERR_ARG;
    }
    if (c->n < 4 || c->d == 0 || c->d > NS_MAX_DIM || c->n > NS_MAX_SAMPLES) {
        snprintf(err, ecap, "corpus shape out of range (n=%zu d=%zu, max d=%d, max n=%d)",
                 c->n, c->d, NS_MAX_DIM, NS_MAX_SAMPLES);
        return AIOS_ERR_ARG;
    }
    m = (size_t)feat_dim(c->d);
    feat = malloc(c->n * m * sizeof *feat);
    if (!feat) { snprintf(err, ecap, "no memory for %zu x %zu features", c->n, m); return AIOS_ERR_NOMEM; }

    for (size_t i = 0; i < c->n; i++)
        expand_quadratic(c->x + i * c->d, c->d, feat + i * m);

    for (size_t i = 0; i < c->n * m; i++) scale += feat[i] * feat[i];
    scale = (scale > 0.0) ? scale / (double)(c->n * m) : 1.0;

    rc = probe(feat, m, c, 1e-4 * scale, bps_out, err, ecap);
    free(feat);
    return rc;
}

bool ns_certify(const ns_corpus *transformed, int tolerance_bps,
                char *what, size_t wcap) {
    int lin = -1, quad = -1, chance = 5000;
    char e1[160] = {0}, e2[160] = {0};
    bool safe = false;

    if (!transformed) {
        snprintf(what, wcap, "refused: no corpus");
        return false;
    }
    if (ns_probe_linear(transformed, &lin, e1, sizeof e1) != AIOS_OK ||
        ns_probe_quadratic(transformed, &quad, e2, sizeof e2) != AIOS_OK) {
        /* Fail CLOSED. An audit that cannot run must not report "safe"; that is
           the same mistake as a signature check that treats an IO error as OK. */
        snprintf(what, wcap, "refused: audit could not run (%s)",
                 e1[0] ? e1 : (e2[0] ? e2 : "unknown"));
        return false;
    }

    /* compare distance from chance in either direction: a probe that recovers
       the attribute INVERTED has recovered it */
    const int dlin  = lin  - chance;
    const int dquad = quad - chance;
    const int alin  = dlin  < 0 ? -dlin  : dlin;
    const int aquad = dquad < 0 ? -dquad : dquad;

    safe = (alin <= tolerance_bps) && (aquad <= tolerance_bps);

    if (safe) {
        snprintf(what, wcap,
                 "certified for THIS corpus: linear %d.%02d%%, quadratic %d.%02d%%, "
                 "both within %d bps of chance",
                 lin / 100, lin % 100, quad / 100, quad % 100, tolerance_bps);
    } else if (alin <= tolerance_bps && aquad > tolerance_bps) {
        snprintf(what, wcap,
                 "REFUSED: linear probe is at chance (%d.%02d%%) but the quadratic "
                 "probe recovers the attribute at %d.%02d%% - the transform removed a "
                 "direction, not the information",
                 lin / 100, lin % 100, quad / 100, quad % 100);
    } else {
        snprintf(what, wcap,
                 "REFUSED: the attribute is recoverable (linear %d.%02d%%, "
                 "quadratic %d.%02d%%, chance 50%%)",
                 lin / 100, lin % 100, quad / 100, quad % 100);
    }
    return safe;
}

void ns_describe(const char *transform, int lin_bps, int quad_bps, int chance_bps,
                 char *out, size_t ocap) {
    snprintf(out, ocap,
             "%-26s linear %2d.%02d%%  quadratic %2d.%02d%%  (chance %d.%02d%%)",
             transform ? transform : "?", lin_bps / 100, lin_bps % 100,
             quad_bps / 100, quad_bps % 100,
             chance_bps / 100, chance_bps % 100);
}
