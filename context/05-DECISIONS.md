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

## D31 — 2026-11-18 — The origin is a parameter, and the guard is where the effect is
M7 could have been a flag threaded through a few call sites. It is instead three
load-bearing choices, each of which was found by *doing* the milestone rather than
reading the card:
1. `sh_exec_origin()` has **no default**. `ORIGIN_UNSPECIFIED` is refused, and
   `sh_exec()` exists as an explicit name for the human case. A default would have
   been `ORIGIN_HUMAN` (the enum value is 0), so every caller that forgot would have
   silently claimed to be a person. It happened once already: a fresh `sh_shell` is
   `calloc`'d, so `last_origin` read 0 = HUMAN before any line ran.
2. The human-only builtin gate is in **`run_command`**, at dispatch, not in the
   router — `promote`/`discard`/`rollback` are builtins that never touch the
   router, which is exactly the hole the acceptance test names.
3. `stage_promote()` takes an `aios_origin` and refuses anything but HUMAN **in its
   own body**. Every layer above it already refuses; that is a claim about callers,
   and callers get rewritten. The test asserts the jail's tree digest is unchanged
   after the refusal, because a promote that renamed half the files and then
   returned false would pass a return-value test.
**Also corrected:** `pctx.rollback_available` was hardcoded `0` since M2, so every
destructive AI action reported "not undoable yet (no snapshot store)" for a whole
milestone after M5 shipped one. A stale truth is a lie with a good excuse. It now
asks the store, through one function shared with `policy check`.
**Not fixed, and now visible:** policy rule 4c ("AI write outside the jail is
NEEDS_HUMAN") is dead code — step 2d denies any outside path for every origin
first. The behaviour is correct (nobody writes outside the jail); the *rule* is
unreachable, and the M7 acceptance sentence that relies on it cannot be true as
written. Recorded rather than papered over; the fix is D32-territory (make the jail
rule origin-aware, which is a policy change, not a plumbing one).
**Reverse if:** a third origin appears (e.g. a signed system service). Then the
enum and the "not UNSPECIFIED" check become a table, and `stage_promote`'s
condition becomes "the origin is on the list of origins that may accept", not
`!= ORIGIN_AI`.

## D32 — 2026-11-18 — Silence is a discard, and nobody can answer for you
The card says "no timeout auto-promote — timeout = discard". Implemented as a
stronger property: **there is no code path from "no answer" to promote at all.**
`ui_present()` returns DISCARD when the asker is NULL, when the asker exists but
`fgets` hits EOF, when the input is unrecognised, and after the look-at-it-again
loop is exhausted. "Please" is not "promote" even though it starts with `p`.
The console installs an asker **only for human-origin runs**. If `--as-ai` could
reach the prompt, "only a person can accept it" would be a sentence with nothing
behind it: a model that can answer its own confirmation dialog has not been
mediated, it has been given a form to fill in.
**Cost:** a non-interactive run (a script, `-c`, the boot proof) can never promote
a held action. That is the intended reading of "a human must decide": a batch job
that could promote would be a batch job that can approve itself.
**Reverse if:** never for the "no answer = discard" half. The "install an asker"
half reverses if a genuinely authenticated human channel exists (a signed tty, a
physical presence token) — then the asker is installed for *that*, not for a uid.

## D33 — 2026-11-18 — Two "only writable place" rules cannot both be true
M3 says a machine's write lands in `<store>/staging/<id>/upper`, which is
deliberately outside the jail. M6's Landlock sandbox says the worker's filesystem
view is the jail and nothing else. Both shipped. Both are right. Neither was ever
run against the other, because the host tests ran with `AIOS_SANDBOX_HARD` unset
and the image sets it — so the image was the only place the contradiction lived.
**Symptom:** under the sandbox, every honest AI write died with SIGSYS before
touching a file. The shell pre-notes declared writes, so `staged` still showed a
row and `promote` answered "promoted 0 files into the jail" with status 0. A
person was told a change was waiting that had never existed, in the one place
whose entire purpose is to be trustworthy about what did and did not happen.
**Why nobody noticed:** the in-image selftest suite had *already* been failing at
`the agent's write is reported as staged` — an M3 assertion — and the boot job
only checked `AIOS-SELFTEST: PASS`, which was false, so the run stopped there.
The M7 origin test is what narrowed it to one line.
**Fix:** three parts, and each was necessary.
1. `aios_hardening_apply()` takes a second writable root (`extra_rw`) and adds
   ONE rule for it — the staging directory the parent names, not the store it
   lives in. `test_kernel` asserts the store's other directories stay refused.
2. Hardening moved from before the request is read to after it is parsed, because
   the staging path is *in* the request. Locking before knowing what to lock out
   is how the sandbox came to forbid the one directory M3 writes to.
3. `tool-exec` no longer `realpath()`s the staging dir. The parent already passes
   an absolute, resolved path (`stage_begin` guarantees it), and re-resolving it
   under the lock performed a readlink that the seccomp filter killed — the
   SIGSYS that started all this. The containment check that matters (the joined
   path stays under the staging root) never needed the filesystem.
**Also:** `fopen` under seccomp needed `__NR_open` (glibc uses it, not `openat`),
and Landlock needed `MAKE_REG`/`MAKE_DIR` — `WRITE_FILE` overwrites an existing
file and does NOT create a new one. Two allowlists that were each one entry short
in a way that only showed up when a file did not already exist.
**Cost:** an AI write now has two things to get right instead of one. In exchange,
the failure mode changed from "silent, and lies about it" to "either works or
says NOTHING WAS STAGED".
**Reverse if:** never for the rule itself. If staging ever moves inside the jail
(the overlayfs design in the M3 card), the second root disappears — and the test
that proves the store is still refused should be deleted with it, not before.

## D34 — 2026-11-18 — A review that shows less than it holds is not a review
M3 through M7 treated the staging area as if at most one change were ever
waiting. That was never asserted anywhere — it was assumed, in three places at
once, and the assumption was wrong in each of them.

**The three defects, found by asking "what does the human see?" rather than "does
the code run":**
1. `sh_latest_stage()` answered "which change do you mean?" by picking the newest
   session by mtime, ties broken by readdir order. Two pending changes were
   indistinguishable from one, and `promote` / `discard` with no argument took
   whichever won. A human who reviewed one change and typed `promote` had no way
   to know that a second existed, let alone which one was applied.
2. A staged file could exist that NO NOTE NAMED. The shell pre-noted the effects
   it declared, so a second AI write folded into an already-live session landed
   its bytes in `upper/` described by nothing. `stage_diff_compute()` iterates
   over notes, so those bytes were invisible to `staged` — and `promote` moves
   notes, so they were never applied. The human read "1 file" while two were
   written, and one was stranded.
3. The worst one: `stage_live` was SHELL state, not COMMAND state. A typed AI
   write left its session live, and the next line — an unrelated agent run —
   inherited it. The agent's guardian staged its own step into the human's
   pending change, and when the run finished with the jail unchanged the
   after-hook discarded "the run's" staging, which by then was the person's
   change. **A change a human had been shown, and had not decided on, was thrown
   away by an agent that never knew it existed.** The reproduction is four lines:
   `machine "fs.write path=REVIEWED.txt content=one"`, `staged`,
   `machine "summarize the workspace"`, `staged`.

**Fix, one rule per defect, all in the fail-closed direction:**
1. `stage_pending_scan()` enumerates every OPEN and PROMOTING session; `staged`
   prints all of them, each with the `[id]` a human types. `stage_resolve()`
   accepts an id, a path, or `latest` — and `latest` resolves ONLY when exactly
   one change is pending. With two, it is an error that names both. A promote is
   not a place to guess.
2. `stage_unnoted()` walks `upper/` and compares it against the manifest, and
   both `stage_promote()` and `stage_promote_locked()` refuse when they disagree.
   The old contract — "the manifest drives the move, unlisted files are ignored"
   — was defended by a test (`test_locked_promote_ignores_unlisted_files`) whose
   last assertion was that the unlisted file did NOT land. That was half a
   protection: ignoring a file is indistinguishable, from the human's side, from
   never having staged it, and the diff did not mention it either. The promote
   SUCCEEDED and left bytes behind that nobody was shown. That test's conclusion
   is now the opposite, and the reversal is stated in the test.
3. `sh_exec_origin()` clears the live-session pointer, so nothing is live until
   the command being run stages something. No command can act on a session it did
   not open. The cost is stated rather than hidden: two AI writes in two commands
   are now two changes, not one — which is exactly what the pending list is for.

**Why this was not visible before:** all three are only observable with *more than
one* pending change, and every test that staged anything promoted or discarded it
before doing the next thing. The suite was a sequence of single-change stories.
The new checks are the first ones that ask what happens when a person is looking
at one change while another arrives.

**Cost.** One walk of `upper/` per promote and per `staged`. Promotion is a human
action, bounded by `AIOS_STAGE_MAX_FILES`, and the alternative is a review tool
that can omit. Cheap at any price this milestone would pay.

**Reverse if:** the manifest stops being the only source of truth about a staging
session — e.g. if upper/ becomes a real overlay and the kernel can enumerate it.
Even then the refusal must stay: "the diff showed everything" is the property,
not "the manifest names everything".

## D35 — 2026-11-19 — A promote proves a record names the change, not that these are the bytes you saw
**Status:** open — deliberately not closed in M8
**Decision.** The M8 promote gate refuses in two cases: the audit chain does not
verify, and no record names this change. Both are needed, and the second is the
load-bearing one — a chain can verify perfectly while containing nothing about the
change in front of you (proved by deleting the stage record: `audit verify` says
"chained and intact", and the promote must still be refused).

What the gate still cannot do: **bind the diff a person read to the bytes that get
applied.** `promote` reads `upper/` at the moment it runs. Anyone who can write the
store can change `upper/` between `staged` and `promote`, and the record will still
verify, because the record names the change and not its content.

So the honest claim is: *a record names this change, so an approval can be shown to
have happened.* The stronger claim — *these are the bytes you approved* — needs a
content hash in the manifest, recomputed and compared at promote.
**Why not now.** It changes the manifest format and every writer of it, and doing it
inside a UX milestone would mean shipping the hash without the review of the layer
that owns the format.
**Cost.** A person with store write access can get a different change applied than
the one they read. In a one-uid process where the audit writer is itself audited,
that is one of a family of gaps rather than an exotic one.
**Reverse if:** never; this is closed by adding the hash, not by deciding against it.

## D36 — 2026-11-19 — The four bugs M8 found, and why each was invisible
**Status:** fixed
**Decision.** M8 was scoped as UX — a list, a diff, two words — and it kept
uncovering defects underneath, because a front-end is the first code that has to
*ask* the engine questions the tests never asked. Four, in the order they bite:

1. **`ui_present()` collapsed every non-PROMOTE answer into DISCARD.** Correct
   while there were three answers; a lie the moment a front-end could say "asked,
   not yet answered". `UI_CHOICE_DEFER` was swallowed, so the desktop's hold
   branch never ran and every hold silently became a refusal. *A function that
   rewrites its caller's answer must be told about every answer there is.*
2. **The desktop could not answer a hold.** `desktop_ask()` returned DISCARD
   synchronously (it must not block — the shell is on the desktop's own thread),
   so `action_gate()` discarded the action *before* `hold_open()` drew the window.
   A person read a proposal, pressed `P`, got "re-run the command to apply it",
   and nothing happened. The question was real; the answer went nowhere. Fixed by
   deferring and resuming (`sh_deferred()` / `sh_resume()`), not by re-running:
   a re-run takes a new descriptor id and applies whatever the store looks like
   *now* rather than what the person was shown.
3. **`write one` was a heap overflow.** In the intent router the content length
   was computed as `last_sp - (t + 6)` after only checking that a space existed.
   For two words, the only space is at index 5 — before `t+6` — so the length
   underflowed to `SIZE_MAX` and `memcpy` wrote a 10-byte string into a 0-byte
   allocation. **Reachable by typing two words at the prompt**, and the same heap
   corruption showed up in the test suite as `double free or corruption`. Found
   only because a new test happened to stage a file with a two-word phrase.
4. **A builtin's return code was rendered as an `aios_status`.** `sh_exec_origin`
   fell back to `aios_status_str(AIOS_ERR)` when a builtin set no message, but
   `AIOS_ERR` is `-1` while a builtin returns `1` — so the fallback returned
   `"unknown"` and the front-end printed `error: error`. A shell's `0/1` and the
   library's status enum are different vocabularies, and the boundary is exactly
   where they were being confused.

**Cost.** Each fix is small; the cost is the class of bug. All four needed a
front-end or a new scenario to surface, and none was caught by 1406 passing
checks. The tests were a sequence of single-change, single-answer, happy-path
stories.
**Reverse if:** never — but note the pattern: the next front-end will find the
next one, and the answer is more scenarios, not more mocks.

## D37 — 2026-11-19 — `promote all` is a word, and it stops at the first refusal
**Status:** decided
**Decision.** `promote all` applies every pending change, and is spelled as a
whole word matched before id resolution — an id is hex digits and a dash, so
`all` can never be a legal id or a prefix of one. Two consequences, both
deliberate:
- It runs `sh_promote_gate()` **per change**, not once. A chain that verifies says
  nothing about whether each individual change is on the record.
- It **stops at the first refusal.** Applying the rest would apply changes in an
  order the person did not choose and cannot see. The output always states how far
  it got, and a partial run exits non-zero: `0 changes applied; stopped at the
  first refusal -- the rest were not touched`.

A session that promotes zero files is reported as `nothing staged to apply
(effects declared, no bytes)` and is **not** counted — `2 changes applied` over a
jail that gained nothing is a number that flatters the command.

**Why a word and not a bare `promote`.** D34 is the record of what happens when
the one command that makes a change real is allowed to guess which change you
meant. `all` has to be typed.
**Cost.** No way to promote a chosen subset in one line; it is `promote <id>`
repeatedly, which also means each is gated and counted separately.
**Reverse if:** the pending list grows past what a person can read, at which
point a selection syntax with explicit ids is the answer — not a wildcard.
