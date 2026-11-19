/* aios/nullspace.h — M11: what a Stiefel rotation does and does not hide.
 *
 * WHAT WAS PROPOSED
 *
 * A local encoder projects token embeddings onto the orthogonal complement of a
 * "personal-identity subspace", applies a secret unitary Stiefel rotation that
 * preserves inner-product attention angles, and sends the result to a cloud LLM.
 * The claimed consequence is I(Z_cloud; PII) = 0, and "zero mathematical
 * possibility of data leakage" — the HTTPS moment for AI.
 *
 * WHAT IS TRUE, AND IS BUILT HERE
 *
 * 1. A rotation IS an isometry. For Q with QᵀQ = I, <Qx, Qy> = <x, y> exactly.
 *    So the attention structure of a document survives rotation untouched and
 *    the cloud model can still reason over it. This part of the proposal is
 *    correct and is implemented exactly (stiefel_isometry_error).
 *
 * 2. A rotation is also a BIJECTION. That is not a side property — it is the
 *    same property. Q is invertible, so the map z -> Qz loses nothing, and
 *    therefore hides nothing. Qᵀ(Qz) = z for anyone who has Q; and anyone who
 *    does not have Q can still recover it from known plaintext, because a
 *    rotation is a LINEAR map with only n(n-1)/2 parameters. If the cloud sees
 *    any n linearly independent embeddings whose content is guessable — the
 *    boilerplate header of a medical form, a page footer, a standard contract
 *    clause — Q is determined outright and every token is readable. This is
 *    implemented (stiefel_unknown_rotation_recoverable) because it is the
 *    single most important thing to know about the proposal, and it is a
 *    theorem, not an opinion.
 *
 * 3. PROJECTION removes a LINEARLY encoded attribute and nothing else. If PII
 *    lives in the span of the protected subspace, P z = 0 and removal is exact
 *    — but only on the embedding the projector was fit to, which sits BEFORE a
 *    nonlinear network, and the cloud gets to apply any nonlinearity it likes.
 *    Any attribute that is a nonlinear function of the embedding (the NORM is
 *    the usual accident: a longer embedding is a different embedding) survives
 *    projection completely. The mutual information does not reach zero; it
 *    cannot, because the residual still contains the variable.
 *
 * WHY THAT MATTERS FOR THIS PROJECT SPECIFICALLY
 *
 * M10 refused a "learned neural conservation critic" because a heuristic that
 * cannot be shown to hold must not be the thing that decides. The same rule
 * forbids this milestone's headline claim. A projection that removes the linear
 * component of an attribute is a real, useful, measurable transformation. It is
 * not I(.; PII) = 0, and stating that it is would be the substitution this
 * project exists to refuse — with a worse failure mode, because a doctor reading
 * "zero mathematical possibility of leakage" would upload a chart.
 *
 * SO WHAT IS THE DELIVERABLE
 *
 * The thing that CAN be proven, and is useful on its own: an AUDITOR. Given a
 * representation and a labelled corpus, it measures (a) how much of the
 * protected attribute a LINEAR probe recovers, (b) how much a QUADRATIC probe
 * recovers, and (c) refuses to certify unless the quadratic gap is closed.
 * A pipeline that only cleans the linear part fails this audit. That is the
 * check that separates a real privacy transform from one that looks real, and no
 * vendor can argue with it, because it is arithmetic over their own outputs.
 *
 * TYPES: the probe mathematics is floating point, because embeddings are
 * floating point and pretending otherwise would be theatre. The DECISION
 * INTERFACE is integer basis points, so that a certificate is comparable,
 * reproducible and testable exactly. The exactness claim applies to the
 * arithmetic this module performs on its inputs, not to the accuracy of the
 * embedding pipeline that produced them.
 */
#ifndef AIOS_NULLSPACE_H
#define AIOS_NULLSPACE_H

#include <stdbool.h>
#include <stddef.h>

#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_MAX_DIM     128   /* embedding dimension this module will handle   */
#define NS_MAX_BASIS   128   /* largest protected subspace                   */
#define NS_MAX_SAMPLES 4096  /* rows in one corpus                           */

/* ---- floating point linear algebra (small, dense, row-major) ------------- */

/* Gram-Schmidt with re-orthogonalisation, producing an orthonormal basis.
 * `basis` is d x k row-major; on return basisᵀbasis = I to within rounding.
 * Returns the RANK actually found: a span of linearly dependent columns yields
 * k' < k and the remaining rows are zeroed, because a projector built from
 * dependent columns is not a projector and silently over-removes. */
int ns_orthonormalize(double *basis, size_t d, size_t k, char *err, size_t ecap);

/* Project every row of `x` (n x d) onto the orthogonal complement of the span of
 * `basis` (k x d, assumed orthonormal). out[i] = x[i] - sum_j (x[i].b[j]) b[j]. */
aios_status ns_project_out(double *out, const double *x, size_t n,
                           const double *basis, size_t k, size_t d);

/* The greatest |<Qx, Qy> - <x, y>| over the given rows. For an orthonormal Q
 * this is floating-point noise (~1e-15), and the test asserts exactly that. */
double stiefel_isometry_error(const double *q, const double *x, size_t n, size_t d);

/* Can a secret rotation be recovered from n known-plaintext embeddings?
 *
 * Solves the least-squares problem  min_R ||X R - Z||  for the (n x d) observed
 * cloud inputs Z and guessed plaintexts X, then reports the residual normalised
 * by ||Z||. A small residual means some linear R explains the cloud's view of
 * content the attacker already knows — and one such R exposes every other token.
 * Written with normal equations; ill-conditioning is reported, never hidden. */
aios_status stiefel_unknown_rotation_recoverable(const double *x_known,
                                                const double *z_seen,
                                                size_t n, size_t d,
                                                double *residual_out,
                                                char *err, size_t ecap);

/* ---- the auditor -------------------------------------------------------- */

/* A labelled corpus: embeddings plus a binary protected attribute. */
typedef struct {
    const double *x;       /* n x d row-major                                  */
    const unsigned char *y;/* n labels, 0 or 1                                 */
    size_t n;
    size_t d;
} ns_corpus;

/* Accuracy of the best LINEAR readout for the label, as basis points (0..10000).
 *
 * Fitted by ridge-regularised least squares on one half of the corpus and scored
 * on the other, in both directions, taking the better (so the number is not an
 * accident of the split orientation). Chance level is 5000. A value near 5000
 * means "this representation does not linearly encode the attribute" and
 * nothing more. */
aios_status ns_probe_linear(const ns_corpus *c, int *bps_out, char *err, size_t ecap);

/* Accuracy of the best QUADRATIC readout, same protocol.
 *
 * This is the number that decides whether a projection was privacy or theatre.
 * It expands each row into its second-order monomials — x_i, x_i²/2, x_i x_j —
 * and fits a linear readout on that. An attribute encoded in the norm, or in any
 * product of coordinates, is invisible to ns_probe_linear and fully visible here.
 * Unlike the plain probe, coefficients are NOT zeroed out for the protected
 * subspace: if the attribute is absent from the residual this probe cannot find
 * it either, which is what makes a low number meaningful. */
aios_status ns_probe_quadratic(const ns_corpus *c, int *bps_out, char *err, size_t ecap);

/* Is a transformed corpus safe to send?
 *
 * Applied to the OUTPUTS of whatever transform the caller ran (projection,
 * rotation, noise, or a vendor's pipeline). Returns true only when BOTH probes
 * fail to recover the attribute: linear and quadratic accuracy each within
 * `tolerance_bps` of chance. `what` receives the reason, always, pass or fail,
 * because a certificate that does not say what it measured is not a certificate.
 *
 * Note what this is NOT: it is an empirical statement about this corpus. It is
 * not a proof of zero mutual information, and the header of this file says so
 * where the next person will read it. */
bool ns_certify(const ns_corpus *transformed, int tolerance_bps,
                char *what, size_t wcap);

/* Human-readable one-line summary, for the demo and the audit record. */
void ns_describe(const char *transform, int lin_bps, int quad_bps, int chance_bps,
                 char *out, size_t ocap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_NULLSPACE_H */
