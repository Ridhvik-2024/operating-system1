# M11 — Stiefel rotation and null-space projection: what is true, what is not

Item #17 asked for a local encoder that projects token embeddings off a
"personal-identity subspace", applies a secret unitary Stiefel rotation, and sends
the result to a cloud model — with the claimed guarantee `I(Z_cloud; PII) = 0` and
"zero mathematical possibility of data leakage", rated 99.2% likely to work.

This milestone builds the part of that which is provable, and measures the part
that is not. It is a shorter document than M10's because the finding is shorter:
one of the three claims is true, one is a theorem-level error, and one is
conditional on something the proposal does not do.

The verdict up front:

| claim | status |
|---|---|
| a Stiefel rotation preserves inner products, so attention survives | **true**, implemented and checked to 1e-14 |
| the rotation keeps the content secret | **false** — it is a bijection, recoverable from `n` known plaintexts |
| projection removes PII, so `I(Z; PII) = 0` | **false as stated** — it removes the *linear* component only |
| an encoder can pass a strong audit | **buildable**, and that is what ships |

---

## 1. The isometry is real

For `Q` with `QᵀQ = I`, `<Qx, Qy> = xᵀQᵀQy = <x, y>`. Every attention score,
every cosine angle, every relative position encoded as a dot product is
unchanged. `stiefel_isometry_error()` compares **all** pairs, not a sample,
because the claim is about all inner products:

```
max |<Qx,Qy> - <x,y>| over 200x8 embeddings and all pairs = 1.07e-14
```

This is the part of the proposal that is simply correct, and it is worth having:
the cloud model's arithmetic over the manifold is exactly what it would have been
over the plaintext.

## 2. But that same property is why it cannot hide anything

A rotation is orthogonal, therefore invertible, therefore **bijective**. The map
`z ↦ Qz` loses no information whatsoever, which is not a side effect of the
isometry — it is the same fact. Two consequences, both implemented:

**(a) The inverse is the transpose.** Anyone holding `Q` reads the data. The
proposal's answer is that only the phone holds `Q`.

**(b) `Q` leaks, because a rotation is a low-dimensional linear map.**
`Q ∈ O(n)` has only `n(n−1)/2` free parameters, and the cloud sees `Qx` for every
token. Given `n` embeddings whose *content* the attacker can guess — the
letterhead of a medical form, a page footer, a standard contract clause, a
disclaimer every bank uses — the attacker solves `min_R ‖XR − Z‖` and recovers `Q`
outright. `stiefel_unknown_rotation_recoverable()` does this, and the demo uses
the recovered map to read **a held-out document that was never in the known set**:

```
residual of the best-fitting linear map = 2.4e-16
held-out embedding recovered to a relative error of 1.06e-06
attribute recovery from the cloud's view: linear 96.00%
```

This is the part that has to be said plainly. "Un-rotatable by anyone but the
phone" is not a key-management problem that better engineering will fix. A
rotation is not encryption, and no choice of `Q` makes one. Encryption needs a
component the attacker cannot invert even with known plaintext; an orthogonal
matrix is exactly a component they can.

Note what the proposal's own framing gives away: it advertises that the rotation
*preserves inner products*. Preservation of the whole geometry is preservation of
the whole information content. The security claim and the utility claim are the
same sentence read in two directions.

## 3. Projection is conditional, and the condition is not met in practice

If the protected attribute is **linearly** encoded in the embedding — that is,
PII `= wᵀz` for some direction `w` in the protected subspace — then projecting
onto the orthogonal complement gives `P z` with `wᵀP z = 0`, exactly. That is a
real, exact, useful removal. The test suite verifies it end to end.

The problem is everything after the word "linearly":

- **Nonlinear encodings survive completely.** If the attribute is a function of
  `‖z‖`, or of `z_i z_j`, or of any coordinate product, then `P z` still
  determines it. There is no linear direction to remove, so there is nothing for
  the projector to remove. The demo measures this: a corpus whose attribute is
  encoded *only* in the norm scores **50.00% linearly** — indistinguishable from
  chance, i.e. "we removed it" — and **100.00% quadratically**.

- **The projection is fitted before a nonlinear network.** The cloud receives the
  output of its own embedding stack, not the vector the phone projected. Even a
  perfectly linear `P` at the phone's layer says nothing about what the cloud's
  layer reconstructs. A transform applied at one layer is not a guarantee about
  another.

- **"Zero mutual information" is a claim about a random variable, and the
  variable is still in the residual.** If `Y` is recoverable from `Z` by *any*
  function — and a quadratic probe is a function — then `I(Z; Y) > 0`. Full stop.
  The proposal's equation is not a strong version of a true statement; it is a
  false statement about the thing being shipped.

### What can be proven instead, and does ship

An **auditor**. `ns_certify()` runs two probes on the *outputs* of whatever
pipeline the caller used, and certifies only when both are at chance:

- **linear probe** — ridge readout on the raw residual, two-fold, taking the
  better orientation;
- **quadratic probe** — the same readout on all second-order monomials, so an
  attribute hidden in the norm or in a product is visible.

The gap between those two numbers *is* the difference between a transform and a
gesture, and it is measured on the vendor's own output, so it cannot be argued
with. A pipeline that cleans only the linear component reports beautiful linear
privacy and fails the audit:

```
projection (linear PII)    linear 47.00%  quadratic 50.00%  -> certified
projection (norm PII)      linear 52.00%  quadratic 100.00% -> REFUSED:
   "the transform removed a direction, not the information"
```

That refusal is the deliverable. It is not the milestone the proposal asked for,
because the milestone the proposal asked for cannot be built.

---

## The bugs this found, in its own tests

Worth recording, because they are the reason the auditor's numbers can be trusted
— and because two of them were **fail-open**, the worst failure mode for a
component whose whole job is to say no.

1. **The split correlated with row order.** The first two-fold split used `i & 1`.
   On a corpus indexed by label, each fold is a single class, the readout learns a
   constant, and the probe reports chance for an attribute that is plainly
   present. The auditor would have *certified a leaking pipeline*.
2. **And it could report 0%.** Worse, with *alternating* labels both folds are
   single-class, both score 0%, and `max()` returns 0. An accuracy of zero is
   perfectly anti-predictive, not "no information", and a caller writing
   `lin < 6500` reads it as privacy when it means the opposite. Fixed with a
   **stratified** split, and a refusal when a class has fewer than two rows.
3. **The label shuffle did not shuffle.** `(unsigned long)(urand()+1.0)*0.5`
   truncates a value in (0,1) to zero, so every iteration swapped with index 0.
   The generator stayed correlated with the labels and confounded the very
   measurement the test existed for.
4. **An uncentred corpus makes a "nonlinear" encoding leak linearly.** Normalising
   rows to different lengths scales their common offset differently per class, so
   an attribute encoded purely in the norm showed up in the raw coordinates at
   88%. This is a real property of real embeddings — they are usually not centred
   — and it is also exactly the confound the control must not have.

The sabotage matrix, with each sabotage applied faithfully and recompiled:

| change | result |
|---|---|
| quadratic probe returns the linear number | **3 failures** |
| certify ignores the quadratic number | **2 failures** |
| audit fails **open** when it cannot run | **4 failures** |
| original `i & 1` split restored | **2 failures** |
| orthonormaliser keeps linearly dependent rows | **2 failures** |
| rotation recovery always reports "unrecoverable" | **1 failure** |

Two sabotages reported 0 failures on the first attempt. Both times the *sabotage*
was wrong, not the test: the first replacement was silently overwritten by later
lines, and the second was a corner the suite genuinely did not cover (fixed by
adding the alternating-label corpus). A sabotage matrix that is never red is
testing the saboteur.

---

## Verification

- `make test` — **20 suites, 0 failed** (`test_nullspace` = 46 checks)
- `test_nullspace` clean under `-Werror` and under ASan + UBSan
- `./nullspace-demo.sh` — 8 checks, exit 0
- `test_nullspace` is **linked** into the image build (that link is what catches a
  missing source in `boot/Makefile`'s own `LIB_SRCS` — the trap that cost M10 a CI
  cycle) but **not installed**, because it asserts nothing about the booted
  machine. It is in `LINK_ONLY_TESTS`, alongside `test_stoich`.

## What is not done

- **Nothing is wired into an encoding path.** This is an auditor and a set of
  checks, not a component in a pipeline. There is no encoder in this OS that
  produces embeddings to audit.
- **The probes are empirical, over one corpus.** `ns_certify()` says "this corpus,
  these two probe families". A determined nonlinear encoding (a trained MLP over
  the residual) could still recover the attribute where the quadratic probe does
  not. The honest claim is that the quadratic probe closes the gap that projection
  classically leaves open, not that it closes all gaps.
- **`n` known plaintexts is an assumption.** The recovery attack needs `d` guesses.
  For a medical form or a legal boilerplate header that is a reasonable assumption;
  for high-entropy random content it is not. The demo should not be read as "all
  rotations break always", but as "a rotation provides no security under known
  plaintext, and forms have known plaintext".
- The probe arithmetic is floating point, so the *decision* interface is integer
  basis points but the numbers behind it are not exact. The interface is exact;
  the measurement is not.

## The honest summary

M10 refused a "learned neural critic" because a heuristic that cannot be shown to
hold must not be the thing that decides. The same rule has to apply to a claim of
zero leakage. What ships here is the checkable part: attention is preserved, and a
privacy transform can be **audited** rather than believed. What does not ship is
`I(Z; PII) = 0`, and it should not be claimed by anyone, because a rotation is a
bijection and an orthogonal projection removes a direction rather than the
information.

The proposal rates itself 99.2% likely to work. The geometry says the headline
guarantee is not 0.8% away from working — it is a category error about what an
invertible linear map is.
