# 02 — WHAT IS MISSING (prioritized)

Severity: **S1 = blocks the stated goal or a safety invariant** ·
**S2 = real engineering gap, hurts users/devs** · **S3 = polish/hygiene**.
Each gap: what the project claims → what the code does → smallest honest fix.

---

## Part A — The constitution has partial implementation (M1–M3)

`os_sys_decided.md` opens with: *"Any component, feature, or implementation that
violates this document is considered invalid by definition."*
Measured against that: **the shipped OS still violates it**, because most of
the mechanisms it mandates do not exist yet. M1 (descriptor), M2 (policy) and
M3 (staging) are real now; G3, G5–G9 are not. Each gap below states the claim,
the reality **at audit time**, and what changed since — the audit-time text is
kept verbatim so it is obvious how far the tree has moved and how far it has
not.

### G1 — Policy engine: **IMPLEMENTED (M2, 2026-11-17)** · was S1
- **Claim** (`policy_engine.interface.md`): deterministic, side-effect-free,
  fail-closed evaluator of Tier 0/1/2 laws, host-controlled, non-introspectable.
- **Reality at audit time**: `grep -ri "policy"` over `src/` found only
  autopilot and scheduler policy. There was **no evaluator, no rule
  representation, no verdict type**. Any tool call the shell could parse ran.
- **Now (M2)**: `aios_policy_eval()` in `src/policy.c` — pure, fail-closed,
  compiled-in table, evaluated in the documented order, asking for a verdict on
  every command the shell runs. Tier 0 binds the human. `policy` prints the law
  and `policy check` explains a verdict, because a gate nobody can explain is
  one people route around.
- **Still missing for the claim to be fully true**: capability *issuance* (the
  grant list is compiled in), Tier 1 time-bound overrides, an emergency-mode
  producer, and the sandbox-result comparison (G3/M4). Rollback feasibility is
  reported but nothing can satisfy it until G4/M5.
- **Fix (smallest honest version)**: `src/policy.c` with a fixed, compiled-in
  rule table (`(action_kind, path_class, class) → ALLOW|DENY|NEEDS_HUMAN`) and
  a `policy_eval(action_descriptor*) → verdict` that is pure and unit-tested.
  Fail closed on unknown action kinds. Wire it in `src/shell.c::exec_tool` and
  in `src/sandbox.c` **before** the worker is spawned.

### G2 — Action Descriptor: **IMPLEMENTED (M1, 2026-11-17)** · was S1
- **Claim** (`action_descriptor.schema.md`): every AI action is a canonical,
  bounded, reversible-aware descriptor; the unit of sandboxing, policy, audit,
  rollback. Lifecycle: construct → validate → evaluate → sandbox → diff →
  host decision (promote / confirm / block) → immutable log.
- **Reality at audit time**: there was no descriptor type anywhere. Tool calls
  were `{tool, args}` JSON built ad hoc in `shell.c` and passed straight to the
  worker. Effects were **not declared in advance**, so nothing could compare
  *declared* vs *actual*.
- **Now (M1)**: `aios_action` in `include/aios/action.h`; every command in
  `shell.c` passes through `action_gate()`; the router attaches the descriptor
  and the *caller* stamps origin (typed = HUMAN, routed = AI). Declared effects
  are on the record per tool. Still missing: the **comparison** of declared vs
  actual (that is G3/M4), which is why this fix closes the type's gap, not the
  whole of G2's claim.
- **Fix**: struct `aios_action` (kind, target paths, declared writes, resource
  budget, reversibility, expiry, intent hash), validated from JSON, produced by
  every route in `shroute.c` (TOOL and AGENT paths). The worker returns a
  matching *result* document; the host diffs them.

### G3 — Sandbox Result / diff schema: **IMPLEMENTED WITHOUT THE KERNEL (M4, 2026-11-17)** · was S1
- **Claim** (`sandbox_result.schema.md`): host-generated ground truth of what
  actually happened (all observable effects, resources, diffs); compared
  strictly against the descriptor; a mismatch auto-blocks.
- **Reality at audit time**: `tool-exec` returned `{"ok":true,"result":…}`.
  There was **no effect report, no filesystem diff, no resource accounting, no
  comparison**. `fs.write` wrote the file and reported success.
- **Now (M4)**: the worker returns `effects:[{path,op,bytes}]`, `complete` and
  `usage`; the host parses them, compares against the declaration in both
  directions, and fails closed on anything undeclared, incomplete or malformed —
  discarding the staging directory so a lie cannot be laundered into a diff.
- **And the part that matters**: the card assumed the worker's report was
  evidence. It is testimony. A swapped `tool-exec` that ignores staging and lies
  in its `effects` array passes every one of those checks while the file sits in
  the jail. So the parent also **witnesses** the jail — hashing each declared
  path before and after — and refuses with `JAIL_MUTATED` when the world moved.
- **Still missing**: this is a periodic check by a process the worker lives
  inside, not a kernel invariant. The read-only mount + real diff (L2) is what
  turns it into one. The witness covers declared paths only; an undeclared write
  to an unwatched path is caught by testimony or not at all.
- **Fix**: worker snapshots the jail's mtime/size map before and after (or
  runs the tool with the jail on an overlay upper dir) and returns
  `effects:[{path,op,bytes_before,bytes_after}]` + resource usage; host compares
  with the descriptor's declared writes and blocks on undeclared effects.

### G4 — Staged writes, diffs, tombstones, promotion: **IMPLEMENTED (M3, 2026-11-17)** · was S1
- **Claim**: all changes go through staging; diffs mandatory; deletes become
  recoverable tombstones; bulk changes require snapshots; the AI never writes
  reality directly.
- **Reality at audit time**: `fs.write` (`src/tool-exec.c`) did
  `open(O_CREAT|O_TRUNC)` + write **inside the real jail directory**. A write was
  immediate, irreversible and invisible to any diff. `rm`-equivalent did not
  exist. Worse: the agent loop called `tool_exec()` directly, so even the
  descriptor gate did not apply to it.
- **Now (M3)**: `src/stage.c`. An AI write is redirected into
  `<store>/staging/<intent>-<seq>/upper/`; the worker refuses to write at all if
  the origin is AI and no staging dir was supplied, so there is no silent
  fall-through to reality. `staged` shows the diff, `promote` applies it,
  `discard` throws it away. Deletes copy to `trash/` before unlinking and
  `stage_undelete()` reverses them. The agent loop now goes through
  `agent_guard`, which builds a real descriptor and asks policy before every
  step.
- **Still missing**: overlayfs (this is copy-on-write with renames), a content
  diff (per-file counts only), rollback of a promoted change (that is M5), and
  any binding between "the diff you were shown" and "the promote you typed"
  (M6/M7).
- **Fix**: overlay. Either (a) inside the image: `mount -t overlay` with
  `lowerdir=/workspace-ro`, `upperdir=/data/staging/<intent-id>` and only
  `pivot`/commit on promotion; or (b) host runtime: copy-on-write temp dir +
  three-way diff. Tombstone = move into `staging/.trash/<ts>-<name>` and only
  `unlink` on user confirmation.

### G5 — Snapshots and rollback: MISSING · **S1**
- **Claim**: rollback is *physics* — always available, cannot be disabled,
  host-controlled, revokes AI authority and hardware access, restores a
  previous valid state; micro / session / emergency rollback.
- **Reality**: `grep -ri rollback src include` → **0 hits**. The only
  "snapshot" in the tree is the autopilot's sensor sample (`aios_ap_snapshot`),
  an unrelated meaning. There is no snapshot store, no restore path, no
  "rollback" command in the shell.
- **Fix (Linux parts)**: `dm-snapshot`, a btrfs subvolume per session, or
  (`data.img`) an ext2 copy — plus a `rollback [micro|session|all]` builtin that
  is implemented in PID 1, not in the shell, so the AI-facing surface cannot
  disable it. Minimum viable: snapshot the store dir + the staging upper dir
  before each agent run; restore = swap dirs + relaunch agents.

### G6 — Authority model is not enforced · **S1**
- **Claim**: strict ordering **Human > Host control plane > Shared VM > AI >
  Sandbox**, enforced structurally; the AI may not escalate, delegate upward or
  retain authority after revocation.
- **Reality**: every AI-OS process runs as the **same uid with full root**
  inside the initramfs. `aiosh`, `tool-exec`, `aios-governor` and the desktop
  are peers; the shell that takes AI/natural-language input can spawn tools,
  jobs and agents with no capability boundary that distinguishes "this came
  from the human" from "this came from a language model". There is **no host
  control plane** process either — PID 1 lives inside the same trust domain.
- **Fix**: split the trust domain. (1) PID 1 keeps absolute authority and
  exposes a tiny request protocol (unix socket, fixed message set: `spawn`,
  `sandbox-run`, `promote`, `snapshot`, `rollback`, `kill`). (2) AI work runs
  as a **different uid/gid** in its own **user namespace**, with `no_new_privs`.
  (3) Human-originated lines are tagged at the TTY (the shell knows whether the
  bytes came from the console or from a model) and only human-tagged descriptors
  may ever get `promote`.

### G7 — Input arbitration: MISSING · **S2**
- **Claim**: human input always preempts AI input, mediated by a host arbiter;
  the AI never injects input; "cursor fights" are a critical failure.
- **Reality**: the desktop (`apps/aios-desktop.c`) is the only writer of the
  framebuffer + input; there is no AI-originated GUI input path at all yet, so
  the rule is *vacuously* true. Nothing exists to arbitrate, and nothing exists
  to stop a future AI input path from being added without one.
- **Fix**: when GUI automation lands, route it through `input.c` as an
  `AI_INPUT` source with (a) a hard rate limit, (b) instant drop on any human
  evdev event, (c) a visible "AI is driving" indicator, (d) a cooldown.

### G8 — Immutable audit / tamper evidence: PARTIAL · **S1→S2**
- **Claim**: every action is logged immutably; vetoes logged; the host verifies.
- **Reality**: `src/tools.c:140 audit_log()` appends JSONL to
  `<store>/audit.log`. Append-only by convention only: no hash chain, no
  signature, no `fsync` policy documented, and the file is writable by the same
  uid as everything else. The AI-facing process can, in principle, edit it.
- **Fix**: hash chain (`prev_hash` per line, sha256 via a tiny built-in
  implementation or `fs-verity` + IMA on the image), written through the PID-1
  request protocol, plus a `audit verify` builtin that walks the chain.

### G9 — Consent / capability gates: MISSING · **S1**
- **Claim**: hardware access needs explicit, time-bound, revocable consent;
  irreversible or bulk actions need confirmation; preferences require consent.
- **Reality**: no confirmation prompt exists anywhere. `fs.write` from an agent
  happens silently. There is no capability model (no "grant read-workspace for
  10 minutes").
- **Fix**: a per-session capability set owned by PID 1 (path prefixes, syscall
  classes, GUI, audio/mic if ever), granted at the console, time-bound, dropped
  on rollback; `aiosh` refuses descriptor execution without a matching grant.

### G10 — Cabinet / multi-agent reasoning, mode awareness: MISSING · **S3**
`os_sys_creating_ai_prompt.md` mandates a "cabinet" of ministers (Defense,
Infrastructure, Finance, Science, Recovery, Privacy) with absolute vetoes, and
mandatory disclosure of the AI's current brain mode. None of it exists in code;
the model has no way to observe its own mode beyond `cortex` printing a string.
This is prompt-layer work, and it should follow G1 (a veto needs an enforcer).

---

## Part B — OS engineering gaps

### G11 — No cross-process IPC despite "semantic IPC" being a headline · **S2**
`sembus` is an **in-process object** (`src/sembus.c`: `struct sembus { sub *head; }`,
`calloc` on create). The shell creates one per process (`shell.c:1554`), so the
desktop, the governor, the tool workers and each spawned job **cannot see each
other's events**. `emit boot.up` is published into a bus nobody else holds.
- **Fix (Linux)**: `unix` `SOCK_SEQPACKET` + `SCM_RIGHTS`, or `memfd` + a small
  broker in PID 1. Keep the API identical (`sembus_pub/sub`) and swap the
  transport, so nothing above changes. Later: `vsock` for host↔VM (`mesh`).

### G12 — `aiproc` (the AI-native process model) is dead code · **S2**
`src/aiproc.c` implements an intent-driven process with a semantic lifecycle on
**pthreads**, but nothing outside `tests/test_aiproc.c` uses it: agents and
jobs are `fork()`ed children (`shell.c:1300`) and `agent` runs inline in the
shell process. So the "process is an intent, not a PID" claim exists as a
library, not as the OS's process model.
- **Fix**: make every unit of work an intent record owned by PID 1
  (`/proc`-like view `/run/aios/intents.json`), with a real pid, a cgroup class,
  a memory namespace and a lifecycle; `aiproc` becomes the *client* of that.

### G13 — No networking (by policy, but now a wall) · **S2**
QEMU is launched with `-nic none`; the seccomp allowlist kills `socket()`; there
is no TCP/IP code, no DNS, no HTTP client. Every "big LLM" path therefore needs
an out-of-band `z-ai` CLI that **is not in the image**, so the OS cannot use a
network model even if one were reachable.
- **Fix**: decide the network story explicitly (see `03-LINUX-INTEGRATION.md`
  Phase 4): either (a) an offline-only appliance with a local GGUF model behind
  the `cortex llm` seam, or (b) an explicit, audited `net.*` tool family with a
  proxy through PID 1, per-domain allowlists and a visible indicator. Do not
  bolt a socket into the shell.

### G14 — No user/identity separation · **S2**
Single root uid; no login, no `getpwnam`, no per-user store, no quota.
`os_sys_open_decisions.md` §13 keeps multi-user open, but *single-user* still
needs a uid boundary between "the human's shell" and "AI work", which is the
same mechanism as G6.

### G15 — The root filesystem is a RAM initramfs · **S2**
Everything except `/data` is unpacked into initramfs RAM at boot. Sessions
cannot persist changes outside the 8 MiB ext2 disk; there is no `/usr` on disk,
no package story, no writable overlay, no A/B slots. The 8 MiB disk holding
memory + audit + jobs will fill under real use (no rotation policy).
- **Fix (Linux)**: read-only base image (erofs/squashfs) + **overlayfs** upper on
  a real data partition, `/data` grown, log rotation, A/B slots for updates.

### G16 — Shared store has no locking · **S2**
The shell, background jobs, tool workers and the governor all append to the
same `memory.journal` / `audit.log` / `jobs.jsonl` under `/data`. Appends are
`O_APPEND` (safe-ish for small writes) but there is **no `flock`**, and
`mem_consolidate()` rewrites the journal via atomic rename while other processes
may be recalling from it. Concurrent agents are the advertised feature, so this
will bite.
- **Fix**: `flock(LOCK_EX)` around journal rewrite, `O_APPEND` + a per-record
  sequence and a reader that tolerates duplicates, or one writer process
  (PID 1) that everyone talks to.

### G17 — The governor is unsupervised · **S1 (cheap) / S2**
`init.c:439-449` forks the governor and never tracks it; the reap loop says
`/* reaped other children (governor, tool workers) — fine */`. If the governor
dies, budgets silently stay wherever they were and `autopilot` still prints a
state file. The milestone's whole claim ("closed loop") becomes false without a
log line.
- **Fix**: 20 lines in PID 1: track `gov_pid`, respawn with backoff, log
  `[init] governor died (rc=N), respawning`, and surface `governor: alive` in
  the `autopilot` builtin.

### G18 — Every boot runs the full test suite · **S2**
`init.c` runs all 11 suites before the console comes up (visible in the boot
logs). It is excellent evidence, but it is production boot cost and a
self-test cannot be a security control. Keep it behind `aios.selftest=1`
(explicitly opt-in) for release images, and keep the CI harness using it.

### G19 — No observability surface · **S2**
No `dmesg` equivalent for OS events, no `log` builtin, no way to read the
audit log from the shell (you must `dur` it and count lines), no trace view of
what the governor did and why beyond one journal line. `docs/` promises
debuggability without a mechanism.
- **Fix**: a ring-buffer log in PID 1 + `log` / `audit` / `trace` builtins; the
  desktop already has windows to show them.

### G20 — Desktop is a shell, not a desktop · **S2/S3**
Five fixed windows, fixed sizes, no resize, no minimize, no taskbar, no
clipboard, no copy/paste, no file editing in the Files window, no app spawning,
no multi-monitor, US keymap only, no on-screen keyboard, assets are procedural
fallbacks (no artwork has landed). `F1`-style discoverability is absent (the
launcher is a text box).
- Not urgent, but do not call it a "desktop environment" in user-facing copy yet.

### G21 — Engineering hygiene · **S2**
- **No `.gitignore`** until this snapshot: `make` litters `src/*.o`, `src/*.d`
  into the source dir (verified — 25 untracked artifacts).
- **~52 MB of binaries in git**: `linux-virt-dev.apk` 18.5 MB (a build-time
  download for the LKM), duplicate `vmlinuz-virt` 10.8 MB (twice), GPU modules,
  `data.img` 8 MiB. These belong in releases / `boot/dl` (fetched by
  `boot/fetch.sh`) with release artifacts, not in history.
- **No LICENSE** — for an OS with a constitution, this blocks contributors.
- **No CI** (`.github/` absent) though `boot-test.sh` and
  `boot-gui-test.sh` are exactly CI-shaped.
- **Docs/code drift**: CHANGELOG says `11/11 host suites`, README/INSTALL say
  `10 suites` / `10 passed`; `docs/ACCEPTANCE-M10.md` cites
  `scripts/m10-spike.sh` and `scripts/m10-bochs-probe.sh`, and CHANGELOG cites
  `download/m10-boot-shots/` — **none of those paths exist** in the tree.
- **Stale handoff**: `ai_con.md` describes a Rust `ai_os_core/` crate
  (`lib.rs`, `action_descriptor.rs`) that is not in this repo. Keep it as
  history or fold it into `context/`, but do not let a new contributor follow it.

### G22 — Platform: x86_64 + QEMU only · **S2**
`console=ttyS0`, `virtio_blk`/`bochs-display` module stack, no device tree, no
GPU other than the bochs/virtio path, no bare-metal install path.
`ai_con.md` says the real target was a **Raspberry Pi** — aarch64 support is a
prerequisite for that and is currently zero (all modules are x86_64 `.ko`).

### G23 — Build reproducibility · **S2**
- `tools/gen_font.py` and `tools/img2aiosimg.py` need **Pillow**, and
  `font8x16.h` is a **build prerequisite for `fb.c/win.c/input.c` but is not
  checked in** — so `make` on a machine without Pillow fails at
  `build/font8x16.h` (verified). `microlm_weights.h` *is* checked in, which is
  the right pattern; mirror it for the font (commit the table, keep the
  generator).
- `boot/dl/` is committed but `.gitignore`d going forward — fine, but
  `fetch.sh` must be the only source of truth.
- No pinned toolchain/kernel version check; the LKM builds against whatever
  headers `fetch.sh` produced.

---

## Summary table

| # | Gap | Sev | Blocker for |
|---|---|---|---|
| G1 | Policy engine | S1 | the constitution |
| G2 | Action Descriptor | S1 | policy, sandbox, rollback, audit |
| G3 | Sandbox result + diff | S1 | promotion decisions |
| G4 | Staged writes / tombstones | S1 | "AI never writes reality" |
| G5 | Snapshots + rollback | S1 | "rollback is physics" |
| G6 | Authority model / trust split | S1 | human override |
| G8 | Tamper-evident audit | S1 | auditability |
| G9 | Consent / capability gates | S1 | hardware + irreversible actions |
| G7 | Input arbitration | S2 | safe GUI automation (not yet built) |
| G11 | Cross-process sembus | S2 | "semantic IPC" claim, desktop↔shell |
| G12 | `aiproc` unused | S2 | "process = intent" claim |
| G13 | Networking story | S2 | any real model |
| G14 | Identity separation | S2 | trust split |
| G15 | Persistent rootfs / overlay | S2 | daily use |
| G16 | Store locking | S2 | concurrent agents |
| G17 | Governor supervision | S2 | "closed loop" claim |
| G18 | Selftest on every boot | S2 | boot time |
| G19 | Observability | S2 | debuggability |
| G20 | Desktop completeness | S3 | product feel |
| G21 | Hygiene (gitignore/license/CI/docs) | S2 | contributors |
| G22 | aarch64 / bare metal | S2 | Raspberry Pi target |
| G23 | Build reproducibility (font) | S2 | clean `make` |

**The one-line answer to "what are we missing":**
a **mediator**. Today the OS is a very good AI *workload host* — intents,
sandboxes, budgets, journals. What it is not yet is an *authority system*: there
is no component that sits between a proposed action and reality, holds the
policy, stages the change, proves what happened, and can put the world back.
Every S1 gap above is a piece of that mediator.
