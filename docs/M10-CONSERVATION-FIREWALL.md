# M10 — Conservation-law firewall for agent chains

Run it:

```sh
./firewall-demo.sh            # the model, then six attack scenarios
./firewall-demo.sh --model    # just the model — what a human signs off on
make -C source test           # 19 suites, includes test_stoich (83 checks)
```

---

## Read this paragraph first

The proposal this implements asks for a **"learned neural conservation critic"**.
That specific component is not here, and I think it should not be, in the role of
*deciding*. A learned critic is a heuristic: it cannot be shown to hold, it drifts
with its training distribution, and it fails silently on inputs outside that
distribution. **Something that cannot be shown to hold is not a conservation law.**
Calling it one is precisely the substitution this project keeps refusing to make —
a safety claim that is really "it worked on the examples we tried".

What is here instead is exact, deterministic integer arithmetic. Every verdict is
a vector comparison you can reproduce by hand, and the same chain always produces
the same sentence. That is what makes it a law rather than a policy.

The chemistry framing survives all of this and is genuinely useful: the map from
"tool calls" to "reactions" is what turns a sequence into something with structure
you can reason about. That part is real, and it is the interesting part.

## What genuinely maps, and what I quietly dropped

**Kept, and load-bearing:**

- **Stoichiometric matrix and conservation laws.** Each tool call becomes a
  reaction with signed deltas over species. The conservation laws are the integer
  basis of the left null space of the stoichiometry matrix. Computed exactly, in
  canonical form (smallest integers, first non-zero positive), so two people
  reviewing the same model see the same string.
- **Cumulative state, not per-call checks.** This is the whole point. A per-call
  check cannot see the drip attack; a check on the cumulative state can.
- **Deficiency δ = n − ℓ − s, computed and reported.** Not because we integrate
  dynamics — we do not — but because a number the proposal leans on should be
  computed rather than asserted, and because δ < 0 is impossible and is a
  genuinely good bug detector. It caught a real one here (below).

**Dropped, with reasons:**

- **Feinberg's deficiency-zero *theorem* as a security guarantee.** The theorem is
  about the long-run behaviour of continuous-time mass-action kinetics: for δ = 0
  and weakly reversible networks, the system admits a complex-balanced equilibrium
  and cannot have a cyclic trajectory. An agent chain is a *discrete, adversarial,
  finite-horizon* sequence, not a well-mixed reactor relaxing to equilibrium. The
  theorem does not transfer, and claiming its guarantees would be borrowing
  rigour that does not apply. What transfers is the **structural** part —
  complexes, linkage classes, rank, deficiency, weak reversibility — and that is
  what is computed and reported.
- **Energy barriers.** "Crosses an unauthorized energy barrier" needs an energy
  function over states; nothing in the proposal defines one, and inventing one
  would be inventing the security property. What the code has instead is an
  **authorized envelope**: a box in species space. That is a weaker, honest,
  checkable statement — "the chain may end here, not there".
- **The neural critic as a decider** — see above. If one is ever added it must
  *propose* the species mapping, and the exact check must still decide.

## The model, in full

This is the artifact. Everything else is arithmetic over it.

```
species:
  jail_bytes         start 8192   envelope [0, 8192]
  trash_bytes        start 0      envelope [0, 8192]
  destroyed_bytes    start 0      envelope [0, 0]   FORBIDDEN TO GROW
  usd:checking       start 25000  envelope [0, 25000]
  usd:savings        start 0      envelope [0, 25000]
  usd:external       start 0      envelope [0, 0]   FORBIDDEN TO GROW
  backup_bytes       start 0      envelope [0, 8192]
  capabilities       start 3      envelope [0, 3]
  permissions_used   start 0      envelope [0, 3]
reactions:
  read               (no quantity change)
  delete_to_trash    -1 jail_bytes  +1 trash_bytes
  purge              -1 trash_bytes +1 destroyed_bytes
  backup             -1 jail_bytes  +1 backup_bytes
  wire_out           -1 usd:checking +1 usd:external
  transfer           -1 usd:checking +1 usd:savings
  spend_capability   -1 capabilities +1 permissions_used
conservation laws (3):
  jail_bytes + trash_bytes + destroyed_bytes + backup_bytes
  usd:checking + usd:savings + usd:external
  capabilities + permissions_used
reaction network: complexes=10 linkage=4 rank=6 deficiency=0 weakly_reversible=no
```

Every species defaults to an envelope of **exactly its initial value** — "may not
move". Authorising movement is an explicit act. The opposite default (permit
anything not forbidden) is how every one of these systems gets owned.

Two refusals, and they are deliberately different claims:

| check | claim | can an operator widen it away? |
|---|---|---|
| envelope | "the chain left the box somebody drew" | **yes** — that is the point |
| forbidden | "this quantity is irreversible and no widening authorizes it" | **no** |

`usd:external` and `destroyed_bytes` are both. That is why the forbidden check runs
*first*: otherwise a person reads "outside the authorized envelope" and reaches for
the envelope.

## The scenarios the demo runs

1. **Tidying up** — read, delete-to-trash, back up ⇒ **allowed**. A reversible
   delete is the ordinary case; a firewall that refuses it is just a thing that
   says no.
2. **The same agent, one step further** — add `purge` ⇒ **refused**, because
   `destroyed_bytes` cannot grow. Every individual call was legal.
3. **Exfiltration that conserves** — `wire_out` breaks *no* conservation law: the
   money leaves `usd:checking` and arrives in `usd:external`, both in the model.
   What stops it is the separately-stated forbidden rule. This is the clearest
   demonstration that **conservation is not authorisation**.
4. **The drip** — 8 × 512 bytes into trash is allowed; 20 × 512 is refused *at
   step 17*, the exact step the cumulative state leaves the box. Per-call limits
   cannot see this; nothing about step 17 is special in isolation.
5. **Capability laundering** — spending the 3 capabilities you hold is allowed;
   spending a 4th is refused.
6. **Determinism** — the same chain twice gives the same verdict and the same
   sentence.

## Three bugs the chemistry found

These are the reason the rigorous part was worth doing. Each was invisible while
the demo still "worked".

**1. A complex is a whole vector, not one side of it.** The first version compared
consumed sides for reaction sources and produced sides for targets, so `A → B` and
`B → A` never recognised each other's complexes. `A ⇄ B` reported *2 linkage
classes and a deficiency of −1*. **A negative deficiency is impossible** — that
inequality is what caught it. Nothing else would have: the network still computed
plausible-looking numbers.

**2. A single sloppy term silently destroyed a conservation law.** `backup` was
declared as `+1 backup_bytes` with nothing consumed — "a copy comes into
existence". That looks harmless and it destroyed `jail + trash + destroyed`, the
law that makes the file protections meaningful. The firewall still refused the
money attack (a different species group), so the demo passed and the property was
gone. Fix: `backup` moves bytes (`-1 jail, +1 backup`) — a backup that takes no
space is not a backup — and the constructor now **verifies** that a law protecting
the file group still exists rather than assuming one does.

**3. `add_rx(net, name, species_a, da, species_b, db)` takes a species index, not a
sentinel.** Passing `-1` as `species_b` means "one term", so a two-term "copy" was
silently a one-term *creation*. That is how bug 2 got in.

## What was sabotaged, and what survived

Every guard was removed and the suite re-run:

| sabotage | result |
|---|---|
| forbidden-quantity check removed | **2 failures** |
| cumulative state reset each step | **10 failures** |
| file-law guard at construction always true | **1 failure** |
| refused step commits its partial state | **1 failure** |
| conservation-law comparison removed | **still passes** |

That last row is not a gap in the tests — it is a limit of the mechanism, and it
is written into the test file. When a reaction breaks a law, **the law is gone
from the computed set**, so the law check can never fire on it. The law check
guards "the state cannot drift off a law that holds", not "reactions cannot
destroy laws". The second is the model author's job, and the only defence is
`fw_laws_conserve_bytes()` at construction — which *is* tested and *does* fail
when removed. Anyone who reads this to judge whether the law check is
load-bearing should know exactly which of the two properties they are getting.

## Verification

- `make test` — **19 suites, 1590 checks, 0 failed** (1507 before this)
- `test_stoich` — 83 checks, clean under AddressSanitizer
- every new file clean under `-Werror` with `-Wshadow -Wformat-security`
- `./firewall-demo.sh` — 10 scenario checks, exit 0
- CI run `44347019770` — build, test (22.04), test (24.04) and boot all pass

### Where `test_stoich` runs, and why it is not in the image

`test_stoich` is built and **statically linked** by `boot/Makefile` but not
installed into the initramfs. The distinction is deliberate: the link is the
check that catches "the image's own `LIB_SRCS` does not know about the new
source", which is exactly how this milestone first failed CI. The 35,024 bytes
it costs in the compressed image (measured by building both ways) are what keep
it out, and the suite has no business in there anyway — it is integer arithmetic
over tables it builds itself. It reads no `/dev`, opens no socket, and asserts
nothing about the booted machine. Running it inside the initramfs would re-test
the host build with the same compiler and libc.

The first attempt at this exclusion silently did nothing:
`$(filter-out $(LINK_ONLY_TESTS),$(TEST_BINS))` filtered `test_stoich` against a
list of `boot/build/test_stoich`, and `filter-out` compares whole words, so the
pattern matched nothing while the Makefile read as if the suite were excluded.
The image size not moving was the only clue. It is fixed by spelling the built
path once in `LINK_ONLY_BINS`.

The size guard is toolchain-dependent — the same tree is 2,741,492 bytes here
and roughly 200 KB larger on CI's static glibc — so the cap is pinned from the
larger toolchain. Do not compare a local delta against a CI delta: xz is not
additive across toolchains, and one draft of the Makefile comment did exactly
that and produced a confident, wrong number.

## What is not done

- **It is not wired into `sh_exec()`.** This is the engine and its proof, not the
  enforcement point. Hooking it into the shell's `action_gate()` is the next step
  and is a change to the gate, not to this file.
- The species mapping is authored. The mathematics is only as honest as the model.
- Values are `long long`: exact for counts and integer cents, and float money
  would already have lost the property.
- No overlayfs, one uid, and the audit writer is still one of the components it
  audits — the standing gaps from D35 and G6 apply here too.
