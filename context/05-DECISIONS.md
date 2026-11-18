# 05 — DECISIONS (log + open conflicts)

Append-only. Newest at the bottom. Each entry: date · decision · why · cost ·
what would reverse it.

---

## D1 — 2026-11-16 — Linux kernel stays; the layer above it is ours
**Status:** inherited (already the project's spine, `ROADMAP.md`)
Kernel = stock Alpine `linux-virt` 6.6.142; everything above (init, shell,
scheduler policy, memory, IPC, desktop) is our C11. No kernel patches; at most
one out-of-tree LKM (`aios_intent.ko`).
**Cost:** we cannot fix kernel-level behaviours; we depend on Linux features.
**Reverse if:** we need a facility that cannot be expressed with namespaces,
cgroups, LSM, seccomp or eBPF.

## D2 — 2026-11-16 — The design docs are the constitution, not a spec of the code
`os_sys_decided.md` + the four contracts define what must be true. The C code
does not implement them yet (see G1–G9). We keep the docs at the root as law and
treat implementation as a roadmap **toward** them, not as evidence for them.
**Cost:** honest but uncomfortable — the shipped OS is currently "invalid by
definition" under its own constitution.
**Reverse if:** we decide to weaken the constitution. That would be a new major
version per `os_sys_decided.md` itself.

## D3 — 2026-11-16 — Fix the build blocker instead of documenting it
`struct sched_param` redefinition is fixed in-tree (`src/ksched.c`,
`boot/probe.c`) rather than worked around with `-D` flags.
**Why:** the flags differ per toolchain and would silently change on the next
machine; declaring `struct sched_attr` is exactly the ABI we use.
**Cost:** we now own that struct definition (15 lines) — acceptable, it is a
stable kernel UAPI.

## D4 — 2026-11-16 — Commit the generated font table (proposal, not yet done)
`build/font8x16.h` should be committed like `microlm_weights.h`.
**Why:** otherwise `make` requires Pillow + DejaVu, which is exactly the kind of
build-time magic this project otherwise refuses.
**Cost:** ~40 KB in the repo; must be regenerated when the font changes.
**Alternative:** ship the PNG atlas in-process, or a hand-written 8x8 font for
`fb.c` and keep DejaVu only for the desktop.

## D5 — 2026-11-16 — The mediator is the next milestone (M11)
Not "networking", not "more apps", not "bigger models". Sequence:
descriptor → policy → staging(+diff) → promote/rollback → hash-chained audit,
then put namespaces/overlay/seccomp-notify underneath it (Phases L1→L2 in
`03-LINUX-INTEGRATION.md`).
**Why:** every S1 gap in `02-WHAT-IS-MISSING.md` is a piece of the same object;
building them together is cheaper than building them apart, and it is the only
way the constitution becomes true.
**Cost:** delays visible features (Mesh, cockpit, more desktop apps).

## D6 — 2026-11-16 — Do not adopt systemd/dbus/udev or a distro userland
**Why:** each would become a second control plane or an un-auditable attack
surface; PID 1 is 548 lines and needs to stay that small.
**Cost:** we hand-write device bring-up and module loading.

## D7 — 2026-11-16 — Sembus transport should change, its API should not
Keep `sembus_create/sub/pub`, swap the in-process list for a unix
`SOCK_SEQPACKET` broker owned by PID 1 (later `AF_VSOCK` for the host).
**Why:** the "semantic IPC" claim is currently false across processes; changing
the transport is invisible to every caller.
**Cost:** a broker in PID 1 (must not become a general service bus).

---

## Open conflicts to resolve (need a decision before building)

### C1 — Who owns the "shared VM"?
`os_sys_decided.md` describes a **host control plane** outside a **shared VM**
that both human and AI use, with a sandbox as a third, disposable place.
The code implements a *single* trust domain: PID 1, shell, desktop, governor and
tool workers are all root in one initramfs.
**Options:** (a) keep one image and fake the layers with namespaces, uids and a
PID-1 request socket (smallest); (b) real two-level: host process outside QEMU
that owns snapshots/rollback and starts the VM (matches the document, much more
work, needs host-side code in a second language or a second binary);
(c) redraw the document to match the single-VM reality.
**Recommendation:** (a) now, (b) when snapshots must survive a VM wipe.

### C2 — Is the AI allowed to write files at all in v1?
The constitution says every change is staged, diffed, and promoted by the host;
today `fs.write` writes directly into the jail.
**Options:** (a) `fs.write` becomes *stage-only* and needs `promote` (strict,
slower, matches law); (b) writes stay direct inside the jail and only escapes
are mediated (pragmatic, violates §11 of the constitution); (c) writes go to an
overlay whose commit is automatic at job end with a diff shown and a 30 s undo
window (middle: "rollback is physics" without a prompt).
**Recommendation:** (c) then (a) for anything outside `/workspace`.

### C3 — What is the model story?
`cortex llm` = `execlp("z-ai")`, which does not exist in the image and could not
reach a network (`-nic none`). So the only real intelligence is the 12 KB
micro-LM.
**Options:** (a) air-gapped appliance + local GGUF over vsock; (b) mediated
network via a `netd` in PID 1; (c) keep micro-LM only and market it honestly.
**Recommendation:** decide explicitly and write it in `os_sys_decided.md` §9
(Brain Modes) — right now that section promises API mode that cannot boot.

### C4 — aarch64 / Raspberry Pi: when?
`ai_con.md` says the Pi is the eventual hardware; every `.ko` in the image is
x86_64 and the GPU path is `bochs-display`.
**Options:** (a) do it now (blocks feature work for weeks); (b) after the
mediator, as a second build target; (c) drop the Pi target.
**Recommendation:** (b) — the mediator is kernel-agnostic and the Pi becomes a
build-target exercise, not an architecture change.

### C5 — Single user with two trust levels, or real multi-user?
`os_sys_open_decisions.md` §13 keeps multi-user open. But even single-user needs
a uid boundary between human-originated and AI-originated actions (G6).
**Recommendation:** implement the two-level boundary now (human uid vs `aios`
uid via user namespace); multi-user later reuses it.

---

## D8 — 2026-11-17 — Target is *capability parity*, not Win32 compatibility
`context/ROADMAP-TO-WINDOWS.md` defines parity as five layers (platform, shell,
apps, app platform, ops/security) and explicitly excludes `.exe`/Win32, registry
semantics and Office file fidelity.
**Why:** binary compatibility would dictate the whole architecture and poison
every AI-native decision; capability parity is what a user actually experiences.
**Cost:** no existing Windows software; everything in the suite is ours.
**Reverse if:** never for Win32. File *interchange* (ODF/CSV/Markdown/PDF) is a
separate, smaller commitment that we do keep.

## D9 — 2026-11-17 — Roadmap order: base → platform → shell → apps → hardware
Storage/persistence (W1) before hardware (W4); the mediator (W0-B) before the app
platform (W3-A); identity (W1-C) before networking (W1-D); text/Unicode (W2-A)
before the app suite (W3-B).
**Why:** each of these is a hard dependency that would otherwise force a rewrite
(the 8 MiB ext2 disk cannot carry firmware blobs; a capability manifest with no
enforcer is theatre).
**Cost:** the flashy things (browser, audio, hardware) land late.
**Reverse if:** a sponsor needs hardware support first — then W4-A/W4-B move up
and W1-D slips, at the cost of a non-persistent system for longer.

## D10 — 2026-11-17 — Trust fields are host-stamped, never parsed
`aios_action_from_json()` leaves `origin` as `ORIGIN_UNSPECIFIED` (making the
descriptor invalid until host code stamps it) and only ever *shortens* expiry,
clamping anything longer to now + 30 s. A descriptor claiming
`"origin":"human"` is refused for having no host stamp.
**Why:** origin is the field the audit log trusts. If a model could set it, the
log would attribute machine actions to humans and M6's hash chain would
faithfully record a lie. Same for expiry: input may reduce authority, never
extend it.
**Cost:** every caller must stamp origin; forgetting is a hard failure rather
than a silent default (deliberate — a default here would be a vulnerability).
**Reverse if:** never. If a second trust domain appears (a real second user),
add an origin value; do not add a parse path.

## D11 — 2026-11-17 — Effects are declared per tool in one table, not inferred
`aios_action_declare_tool_effects()` is the single place mapping a tool to the
resources it touches, and it sets `effects_complete` because it enumerated them.
**Why:** if each call site decided its own effects, the claims would drift and
the M4 declared-vs-actual diff would be meaningless. One table is auditable by
reading it.
**Fail closed on the failure path too:** `effects_complete` is set to 1 *only*
when the enumeration succeeded (`st == AIOS_OK`). If any single effect cannot be
declared — a path with a `..` component, say — it stays 0, and the shell refuses
the whole action rather than running it with a partial claim.
**Cost:** a new tool must be added to that function or its effects are unknown
(which is a mismatch the diff will flag, not a silent pass).
**Learned the hard way (M2):** "declared nothing" and "declares nothing truthfully"
are the same value of `n_declared`, so a policy that inspects only declared
effects sees a clean, empty set for an action that *could not be declared at
all*. M1's refusal at the call site is what makes that safe; policy's step 2
independently inspects the path the action carries, so two independent things
must be wrong before such an action passes. Never collapse those two cases into
one boolean.
**Reverse if:** a tool ever needs per-invocation effects that cannot be derived
from its args — then extend the table, do not move the logic out.

## D12 — 2026-11-17 — The law is compiled in; there is no law file
`src/policy.c` holds a `const` table. No file, no env var, no config, no
override verb (`policy` is read-only; there is deliberately no `policy allow`).
**Why:** a constitution that a dropped file or an exported variable can edit is
not a constitution. The only way to change what is permitted is to change the
source and ship an image — which means the change is reviewed, attributed and
in the audit trail by construction.
**Cost:** no per-site policy tuning, no hotfix for a bad rule, no enterprise
"custom rules" story. Every change is a release.
**Reverse if:** a genuine multi-tenant product needs per-tenant law — then it
must become *signed, versioned policy bundles with the signature check in PID 1*,
never a writable file. Do not start with "just read /etc/policy.conf".

## D13 — 2026-11-17 — Tier 0 binds the human; tier 1 does not
The audit log, the law itself and PID 1's socket are refused for **every**
origin, including a human who typed the command. Everything else distinguishes
human from machine: delete, bulk change and >1 write stop at a human when the
machine proposes them, and are allowed when a person asks.
**Why:** the first rule is what keeps the system auditable at all — if anyone
can edit the log, no other guarantee can be checked. The second is what makes
origin tagging mean something: if a human's explicit command were gated the same
way, the gate would be theatre people learn to click through.
**Cost:** a human cannot repair a corrupt audit log from inside the shell. That
is intended: repair is an out-of-band, physical act.
**Reverse if:** never for tier 0. For tier 1, if a deployment needs a policy that
binds humans too, add a rule with a documented reason — do not widen tier 0.

## D14 — 2026-11-17 — Purity is a parameter, not a promise
`aios_policy_eval()` takes `now_ms`; it may not read the clock. Making that true
required splitting `aios_action_is_valid_at()` out of `aios_action_is_valid()`.
**Why:** "deterministic" and "side-effect free" are testable properties only if
every input is an argument. A pure function that calls an impure one is a
function that is usually reproducible, which is worse than one that admits it.
**Cost:** every caller passes a clock; tests pass 0 to mean "no clock".
**Reverse if:** never. If a rule genuinely needs wall time (it does not today),
pass it in.

## D15 — 2026-11-17 — The agent loop asks permission, per step
`agent_cfg.guard` is required, and the loop calls it before every `tool_exec`.
`src/agent.c` may not run a tool on its own authority.
**Why:** it did. The agent called `tool_exec()` directly, so every action ran
with no descriptor, no verdict and no staging — and the agent is the component
most likely to act unsupervised. M1 and M2 had both shipped believing the shell
was the only way in. It was not. "Every action is mediated" is false if any
component has a `tool_exec()` pointer.
**Cost:** every step builds and validates a descriptor (a few microseconds), and
a refused step is reported back into the planner's history rather than aborting
the run — so a refusal is visible and retryable, not fatal.
**Reverse if:** never. If the guard becomes expensive, make the descriptor
cheaper; do not let a caller opt out. A NULL guard is legitimate only in tests.

## D16 — 2026-11-17 — `promote` takes its instructions from the manifest, not the directory
`stage_promote_locked()` is what a human-triggered promote calls. It loads every
note first, then moves only files named by notes, and refuses anything whose
`realpath()` escapes `upper/`.
**Why:** a directory listing is attacker-controlled the moment a human can write
into the store. Moving "whatever is in upper/" would let someone plant a file or
a symlink and have the policy engine never see it — a bypass that does not look
like one. The manifest is written by code paths that build descriptors.
**Cost:** two promote functions (`stage_promote()` trusts, `stage_promote_locked()`
does not) and one more thing to keep in step. Worth it: the trusting one is
never reachable from a shell builtin.
**Reverse if:** never. If the two converge, delete the trusting one.

## D17 — 2026-11-17 — A refusal must be visible, and a size cap must be tracked
Two smaller rules this milestone earned:
**(a)** When the guardian refused every agent step (a missing intent hash), the
only symptom was an agent reporting success having done nothing. Fail-closed is
right, silent is not: the refusal now goes into the planner's history, the
audit log and the returned message.
**(b)** `source/boot/.m10-size-cap` is no longer in `.gitignore`. It was
regenerated on every fresh checkout as "first build + 128 KB", so it could never
fail in CI — each run re-pinned it to whatever the build had just produced. The
cap is a tracked number now, and raising it is a reviewed commit.
**Reverse if:** never for either. A guard whose failure mode is silence, or
whose threshold is set by the thing it measures, is decoration.

## D18 — 2026-11-17 — A worker's self-report is testimony, not evidence
`effects` is compared against the declaration, and separately the **parent**
witnesses the jail (hash of each declared path before/after). A worker that lies
in its report is caught by the second check; a worker that is merely sloppy is
caught by the first.
**Why:** the M4 card treats the worker's effect document as the ground truth.
It is not — it is written by the same component that might be wrong. I built the
adversary to prove the check worked and it proved the opposite: a swapped
`tool-exec` that ignores staging, writes into the jail and reports only the
declared file passes every rule, because both halves of the comparison come from
the same liar. M3's whole guarantee was being enforced by the component we trust
least. Testimony is what we **ask**; the witness is what we **check**.
**Cost:** a bounded read (hash) of each declared write path, twice per staged
command. Fine at 1 MiB; if it ever bites, hash metadata and sample content
rather than dropping the check.
**Reverse if:** never. When L2 lands and the jail is genuinely read-only to the
worker, the witness becomes redundant *evidence* rather than the only defence —
and a redundant check on a trust boundary is worth its microseconds.

## D19 — 2026-11-17 — A test that skips is a test that reports success
Two fixtures in this milestone failed to compile and the suites still printed
PASS, because the skip path incremented nothing. Both now `CHECK(built == 0)`.
**Why:** a test suite whose failure mode is "quietly does less" trains everyone
to trust a green tick that means nothing. The same instinct is what makes people
call an OS "mediated" when one call site was never wired (M3's finding).
**Cost:** fixtures must compile on every platform that runs the suite, or the
suite fails everywhere. That is the correct trade for a security test.
**Reverse if:** never. If a fixture is genuinely platform-unavailable, skip it
with an explicit `SKIP()` that prints — visible in the log, not silent.

## D20 — 2026-11-17 — One limit constant, sized to the largest legitimate payload
`AIOS_TOOL_REQ_MAX` (1.2 MB) is shared by the parent and the worker and is
larger than `FS_WRITE_MAX` (1 MiB).
**Why:** the request cap was 64 KiB in two independent places while the write cap
was 1 MiB, so the documented write limit was unreachable and a legitimate 100 KB
write failed as "request too large" with no indication which limit it hit. Two
constants describing one boundary will always drift; the drift will always land
on the path nobody tests with a big input.
**Cost:** the worker may now buffer 1.2 MB of input.
**Reverse if:** the payload limit changes — change the one constant, and check
that it is still larger than the largest thing a tool is allowed to produce.

## D21 — 2026-11-17 — A limit the parent knows is enforced by the parent
`AIOS_FS_WRITE_MAX` is checked in `tools.c` before a request is built, as well as
by the worker.
**Why:** the worker was the only enforcer, so refusing a 1.1 MB write meant
serialising 1.1 MB, forking, piping it across, and only then hearing "too big".
On a developer machine that is a few milliseconds. Under QEMU's TCG on a boot with
other work running it exceeded the call's own 10 s deadline, so a clear refusal
arrived as a timeout — indistinguishable from a hang, with no explanation. The
limit is known on both sides; the cheap side should refuse.
**Cost:** the rule lives in two places, so the words must match. They are one
constant and a test that compares the message.
**Reverse if:** the payload can be produced incrementally (a streamed write), in
which case the worker must enforce it as it goes and the parent's pre-check
becomes an optimisation rather than the guarantee.

## D22 — 2026-11-17 — Tests must run where the product runs, or they are decoration
Every test failure in this milestone that mattered in the image was one that had
passed a hundred times on the host: a fixture needing a compiler that the image
does not have, fixtures linked against a libc the initramfs does not have, and a
reliance on `/proc/self/exe` before `/proc` is mounted. There is now a documented
way to run the whole suite inside the real image (`unshare -r` + `chroot` +
extracted initramfs), and the boot job is the authority.
**Why:** the four CI cycles this cost were not caused by hard bugs. They were
caused by a green tick from an environment where the code was never going to run.
The same instinct that made M3's guarantee decorative for the agent path.
**Cost:** the extracted-rootfs run is slower and manual. Worth it for anything
that touches process spawning, paths, signals or the loader.
**Reverse if:** never. Add the run to CI as a scripted step when it is cheap
enough; do not drop it because it is inconvenient.

## D23 — 2026-11-17 — A filesystem boundary breaks every rename, so stage next to the target
The restore path staged its copies inside the store and then renamed them onto the
jail. It passed every host test, because on a host the store and the jail are two
directories on one filesystem. In the image the jail is on the ext2 side disk and
the store is in the initramfs, and every one of four renames answered `EXDEV`:
*"cannot move the current jail aside: Invalid cross-device link"*.
**Why:** `rename()` is not a copy. It is a directory-entry move, and it is only
defined within one filesystem. The boot job found this because it is the only place
the two trees are ever on different media — and the message reads like a permission
problem, one step away from the real cause.
**What it cost:** four separate renames, each hiding the next. The staging dir
inside the jail (`<jail>/.restore`) was the worst: it is a *child*, so it moves
with the parent and the subsequent rename finds nothing, reporting "No such file
or directory" about a path that obviously exists.
**Fix:** every temporary and backup path is a **sibling** of the thing it protects,
built by `sibling_path()` rather than `snprintf` so a path build cannot silently
truncate. State preserved during a restore is `<jail>.old` and `<store>.replaced…`.
**Test:** `test_cross_filesystem()` puts the store on `/dev/shm` and the jail in
the source tree — two real filesystems, no root needed — and reproduces the CI
failure exactly when the fix is reverted.
**Reverse if:** never. Any place that renames between two trees must first ask
whether they can be on different filesystems. In this OS the answer is yes.

## D24 — 2026-11-17 — Coarse clocks turn "unique" ids into collisions
Snapshot ids were `<milliseconds>-<pid>`. On a desktop that is unique. In the
image the clock ticks coarsely and a take of a small jail finishes well inside one
tick, so two takes produced the same id, the same temp directory, and the second
could not rename onto the first. The reported failure was three groups downstream
("an ambiguous id is not guessed"), because there was only one snapshot left to be
ambiguous about.
**Why:** uniqueness was assumed from the resolution of the machine the code was
written on. A virtual clock is not a wall clock.
**Fix:** `<ms>-<pid>-<seq>`, where seq is the first value with no existing
directory. This is what `snapshot.h` already promised; the code had dropped it.
**Test:** `test_ids_are_unique()` asserts the *property* — eight takes, eight
distinct ids, eight directories — rather than the format. It cannot fail on a fine
host clock, which is exactly why the bug was invisible there.
**Reverse if:** never. Any identifier that must be unique must be made unique by
checking, not by hoping the clock is good enough.

## D25 — 2026-11-17 — The component that takes AI input must not hold rollback power
`aiosh` can be talked into things by an agent; that is what a shell for an AI is.
So the ability to put the machine back is served by PID 1 over a unix socket, and
the shell asks.
**Why:** a rollback owned by the shell is a rollback an agent can reach through
the shell. The boundary that matters is not "can the AI type `rollback`" — it is
"does the code that executes AI input own the restore path". It does not.
**Cost:** a protocol, a server, a client, and a documented answer to "who may
ask". Every snapshot now costs a connect().
**Honest limit:** `SO_PEERCRED` is checked before the request is read, but today
every process runs as the same uid, so the check cannot tell an agent from a
human. The header says so. It becomes a real boundary when agents get their own
uid (Phase 3), and the check will already be in the right place.
**Reverse if:** never for the ownership. The protocol could be replaced by a
different transport; the property to keep is that the restore path is not reached
from AI input.

## D26 — 2026-11-17 — A stale socket and an absent supervisor must not look alike
The shell falls back to an in-process snapshot when no supervisor answers, which
is what makes the host tests work. The first version of that fallback made "PID 1
owns rollback" true only when the shell happened to start second: the socket file
did not exist yet, the shell connected to nothing, and quietly did the work itself.
**Why:** a fallback that cannot distinguish "not there" from "not up yet" turns a
guarantee into a race, and the race is invisible because both paths succeed.
**Fix:** if the socket's directory exists but the socket does not, the client waits
up to ~1 s for it. If the directory does not exist at all, there is no supervisor
on this machine and the fallback is correct.
**Test:** the in-image check starts the supervisor and the shell *concurrently* and
asserts the snapshot came from the supervisor's jail, not the shell's.
**Reverse if:** the supervisor ever becomes optional on a booted image, in which
case absence should be an error, not a fallback.

## D27 — 2026-11-17 — A chain that only checks itself is a chain the attacker can rewrite
`audit_verify()` reads the file and recomputes every link. That catches a changed
record, a removed middle record, a reordering and a forged append. It cannot catch
one thing: the whole file being rewritten consistently, or the last record being
removed cleanly. Nothing outside the file says how many records there should be.
**Why it matters:** "tamper-evident" is a phrase that invites overclaiming. The
honest statement is "partial tampering is evident, wholesale rewriting is not",
and the difference is a value committed somewhere the writer cannot reach.
**What the code does about it:** `audit verify` prints the head hash, and
`audit_head()` exists so a later session can pin that value somewhere else (the
next boot's first record, the supervisor, a host signature). Until one of those
exists, the head is printed and not checked, and the output says so.
**Card divergence:** the M6 card said a deleted last line should be reported as a
torn tail. It cannot be, and asserting it would be a lie: a cleanly removed final
record leaves a well-formed, shorter, internally consistent file. `test_audit.c`
asserts the honest answer (undetectable from the file alone) with the reason
written down, so the gap is recorded rather than papered over.
**Reverse if:** never. Add the off-file commitment; do not weaken the wording.

## D28 — 2026-11-17 — An amnesiac log verifies perfectly
`trim_torn_tail()` scanned backwards in a way that never examined the final byte,
so every well-formed log looked torn and was truncated to zero before each append.
The result: one record, always `seq 1`, always chaining to genesis — and
`verify()` reported "1 records, chained and intact". Self-consistent, tested, and
useless.
**Why:** the tests asserted that the chain VERIFIED, never that it GREW. A
verifier's opinion about a log it has only ever seen one record of is not evidence.
**Fix:** check the last byte first (that is the whole definition of torn), and the
test now asserts `records == 100` after 100 appends, plus that all 100 hashes
differ. The in-image proof asserts the count rises across a reboot (6 → 14).
**Reverse if:** never. Any "chain is intact" claim must be paired with a claim
about its length.

## D29 — 2026-11-17 — A promise in a header is a requirement on the code
`audit.h` said a caller cannot supply its own `hash` or `prev`. The code appended
its own fields *after* the caller's, so a body containing `{"hash":"deadbeef"}`
produced a record with two hash fields, kept the caller's `prev`, and failed to
verify — a record this module had just written, unable to pass its own check.
**Why:** relying on a JSON parser's duplicate-key behaviour to discard a forged
field is not a defence; it is an accident that happens to work until the parser
changes.
**Fix:** `strip_field()` removes `hash`, `prev` and `seq` from the body before the
real ones are computed, and the test asserts the caller's values appear nowhere in
the written line.
**Reverse if:** never. When the header and the code disagree, one of them is a
bug; it is never "documentation".

## D30 — 2026-11-17 — A failed record is reported, not swallowed
M4's `audit_log()` ignored every error from `fopen`/`fwrite`/`fsync`. A full disk
meant actions happened and left no trace, silently.
**Why:** for a log whose purpose is to make suppression visible, "no record" and
"the record was lost" must not look alike afterwards.
**Cost:** a failed record now prints to stderr. The tool call still completes:
refusing to work because the log is unwritable trades a missing record for a broken
machine, and the log's own "busy" error tells a caller which happened.
**Reverse if:** the record becomes a precondition for the effect (it should, once
the write is in PID 1 and can be made transactional with the action).
