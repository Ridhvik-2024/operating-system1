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
