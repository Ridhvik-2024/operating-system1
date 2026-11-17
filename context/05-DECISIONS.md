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
