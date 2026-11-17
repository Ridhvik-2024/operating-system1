# CONTEXT.md — AI-OS project memory

## RESUME FROM HERE (read this block first)

1. The OS **works**: 10 milestones, **12 test suites**, a bootable QEMU image with
   our own PID 1, cgroups/eBPF/Landlock, autopilot, a 12 KB micro-LM, durable
   ext2 store, and a framebuffer desktop.
2. The **constitution is half-built**: the action descriptor exists and gates
   every command (`M1`, done), but there is still no policy engine, sandbox
   diff, staging, snapshot, rollback, capability or hash-chained audit (G1,
   G3–G9 in `02-WHAT-IS-MISSING.md`). A descriptor is a **choke point, not an
   enforcer**: nothing is *denied* yet, only malformed. Do not describe the OS
   as "safe" or "governed" until G1 exists.
3. The next milestone is **M2, `policy_eval()`** — the fail-closed law table
   that turns the descriptor into a verdict, then staging/diff → promote/
   rollback → audit (M3–M8), then namespaces/overlayfs/seccomp-notify underneath
   (`03-LINUX-INTEGRATION.md`, Phase L1→L2). That is
   **W0-B** of `ROADMAP-TO-WINDOWS.md`, which is the map to desktop parity
   (W0 base → W1 platform → W2 shell → W3 apps+SDK → W4 hardware → W5 users).
4. Before coding: confirm `make && make test` is green using the traps in
   `CONTEXT.agent.md`. **CI now enforces this** (`.github/workflows/ci.yml`:
   build with `-Werror`, tests on 22.04 + 24.04, and the two-boot QEMU durable
   proof) — a red run is the first thing to read, not a local guess. Host suite
   is 12/12; the only SKIPs are Landlock/eBPF/cgroup, which need privileges a
   runner does not grant (the boot job covers them in-OS).
5. Do not edit the constitution documents (`os_sys_decided.md` + the four
   contracts). They change only by an explicit new-major-version decision.

> **This file is the memory.** If you are an AI assistant (Claude, ChatGPT,
> Gemini, or a coding agent) and you have only this message + this file, you
> know the project well enough to continue it.
>
> **Owner:** Ridhvik · **Repo:** `Ridhvik-2024/operating-system1`
> **Snapshot date:** 2026-11-16 · **Version in tree:** `AI-OS 1.1.0-m10` (v1.1 desktop)
> **Status of every claim below:** verified by reading the code and running it
> — see `04-BUILD-VERIFY.md` for the exact commands and results.

---

## 1. What this project is (one paragraph)

**AI-OS** is an AI-native operating system built **on a Linux kernel base**, where
everything above the kernel is rewritten so that AI is a first-class OS citizen
instead of an app. Processes are *intents*, IPC is *semantic*, memory belongs to
the OS (not to an app), and the scheduler is tuned by an AI daemon against
kernel signals. It ships as a **bootable QEMU image** (Linux 6.6.142 + a 103-entry
initramfs + our own PID 1) and also builds as a **host runtime** (a static C11
library + shell) on any Linux box.

Guiding sentence from `source/ROADMAP.md`:

> **kernel stays Linux; everything above it is ours.**

## 2. The one-sentence honest summary

**The OS layer is real and it works; the *safety constitution* is still only on
paper.** Ten milestones of systems code are running and testable; the
"policy engine / action descriptor / staged writes / rollback" spine that
`os_sys_decided.md` calls non-negotiable has **zero implementation** in the tree.
That is the single most important fact to carry forward.

## 3. Two layers in the repo (do not confuse them)

| Layer | Path | What it is |
|---|---|---|
| **Release bundle** (flat) | repo root | What a user downloads: `vmlinuz-virt`, `aios-initramfs.cpio.xz`, `data.img`, `run-qemu.sh`, `boot-test.sh`, boot logs, and the **design/policy documents** (`os_sys_*.md`, `action_descriptor.schema.md`, `policy_engine.interface.md`, `rollback_snapshot.contract.md`, `sandbox_result.schema.md`) |
| **Source tree** | `source/` | The real code: ~12k lines of C11 in `src/ apps/ include/ boot/`, 2k lines of tests. Builds with `make` and `make -f boot/Makefile boot-image` |

The design documents live **only at the root** and are **not referenced by any
code**. Treat them as the constitution, not as a spec that was implemented.

## 4. What actually exists (verified, with file evidence)

### 4.1 Boot path — a real OS boots
- `source/boot/init.c` (548 lines) is **PID 1**: mounts `proc/sys/devtmpfs/tmpfs/cgroup2/tracefs`,
  loads `aios_intent.ko`, creates cgroup classes, starts the governor, runs the
  in-OS selftest, then supervises **two** console front-ends forever
  (`aios-desktop` on the framebuffer, `aiosh` on serial) with crash counters and
  an emergency busybox shell fallback.
- The shipped initramfs holds **103 entries**: `/init`, 4 AI-OS binaries in
  `/sbin`, 31 busybox applets in `/bin`, 4 kernel modules, 16–18 GPU/input
  modules, 11 test binaries, `/workspace`, `/etc/aios/boot.aiosh`.
- Boot flags: `aios.mode=test`, `aios.selftest=0`, `aios.gui=0`,
  `aios.durable=0`, `aios.autopilot=off|static|balanced|aggressive`.
- QEMU is run with **`-nic none`** — *the booted OS has no network at all.*

### 4.2 Kernel bridge (M6) — the OS talks to the kernel
- `source/boot/module/aios_intent.c` — our own LKM: `/dev/aios` intent registry
  (INTERACTIVE / BATCH / IDLE) + `/proc/aios` with per-class wakeups counted by
  a **kprobe** on `try_to_wake_up`.
- `source/src/ebpf.c` — **two hand-assembled eBPF programs (no libbpf, no clang)**
  attached to `sched:sched_wakeup` and `sched:sched_switch`.
- `source/src/ksched.c` — `sched_setattr(2)` policies per class + cgroup v2
  `cpu.max` budgets (`aios/tools` 50 %, `aios/agents` 25 %, `aios/system` unlimited).
- `source/src/hardening.c` — raw-syscall **Landlock ABI v3** ruleset + inline
  **seccomp cBPF allowlist (~54 syscalls)**; `execve/fork/socket/mount/bpf` → `KILL_PROCESS`.

### 4.3 Autopilot (M7) — closed-loop AI scheduler
- `source/apps/aios-governor.c` + `source/src/autopilot.c`: pure, unit-tested
  policy that **lends CPU budget** between classes using wakeup deltas,
  ctx-switch deltas and cgroup `cpu.stat`, with 3-tick hysteresis, live
  `cpu.max` writes and a JSONL journal.
- **Finding:** PID 1 starts the governor but **never respawns it** — its comment
  says "reaped other children (governor, tool workers) — fine". If the governor
  dies, the closed loop is silently gone for the rest of the boot.

### 4.4 The OS's own language model (M7)
- `source/src/microlm.c` + `microlm_weights.h` (generated by
  `tools/microlm_gen.py`): naive-Bayes intent classifier (bg/urgent/normal)
  + bigram generator, ~12 KB, deterministic, **zero network**.
- This is the **only intelligence that actually runs inside the booted image**.
- The "big LLM" path (`cortex llm`) shells out to **`z-ai` CLI via `execlp`**
  (`src/cortex.c:192`) — that binary is **not in the image** and there is no
  network, so inside the booted OS `AIOS_SHELL_CORTEX=llm` cannot work.

### 4.5 Durable state (M8)
- 8 MiB ext2 side disk (`data.img`, built by `tools/mkfs_ext2.py` with zero host
  tools), mounted by init as `/data`, exported as `AIOS_STORE=/data/aios/store`.
- `source/src/durable.c`: atomic replace = temp + fsync + rename + dir fsync;
  journals = `O_APPEND` + fsync; replay **drops a torn tail**.
- Persisted: memory journal, audit log, job history, autopilot conf/state/journal,
  boot counter. `boot-test.sh` proves it with **two boots on one disk**; the
  checked-in logs show `restored 2 files, previous session: jobs=1 mem=9 audit=6
  autopilot=1` and `this is boot #2`.

### 4.6 AI shell (M5) — `aiosh`
- `source/src/shell.c` (1 682 lines) is a small language: `;` sequences,
  `|` pipelines with text+JSON, quoting, `key=value` and `{json}` args, `$var`
  `${var}` `$out` `$?` `$json`.
- 24 builtins (`help version history remember recall emit set unset vars count
  grep head tail cortex jail store jobs wait kill spawn lm autopilot tune dur kernel`),
  8 tools (`calc fs.read fs.write fs.list memory.search proc.info selftest.sleep
  selftest.spin`), and a **natural-language router** (`src/shroute.c`) that
  decides: run one tool / start an agent / reply directly.
- Background work is a **forked child** with its own memory + registry
  (not the `aiproc` thread model — see gap G2).

### 4.7 Agent runtime + OS-owned memory (M4)
- `source/src/agent.c`: observe → plan → act → reflect, pluggable cortex
  (`scripted` for CI, `llm` for real models).
- `source/src/mem.c`: 3 tiers (working / episodic / semantic), 256-dim embedding
  from **hashed 3- and 4-grams**, recall = `0.62·cosine + 0.23·recency +
  0.15·usefulness`, consolidation dedupes `cos > 0.93` and promotes the top 8.
- Every tool call is appended to `audit.log` (plain append, no hash chain).

### 4.8 Sandboxed tool worker (M4/M6)
- `source/src/sandbox.c` spawns `source/src/tool-exec.c` per call: pipes,
  `RLIMIT_CPU/AS/NOFILE/NPROC/FSIZE`, parent-side SIGTERM→SIGKILL deadline,
  `chdir(jail)`, `..`/absolute/symlink-final rejection, `PR_SET_NO_NEW_PRIVS`,
  then (when `AIOS_SANDBOX_HARD=1`, which init sets) Landlock + seccomp.

### 4.9 Desktop (M10) — the OS has a face
- `source/src/fb.c` double-buffered framebuffer with damage rects + 8×16 glyphs
  from a DejaVu table generated at build time; `win.c` z-order/focus/title/close/
  drag/damage-driven compositor; `input.c` raw evdev keyboard + absolute tablet.
- `source/apps/aios-desktop.c` (944 lines): 5 windows — terminal, files, agents,
  memory, autopilot — plus a **natural-language launcher bar** on the same
  micro-LM cortex as `aiosh`, and sembus toasts.
- Every asset has a **compiled-in procedural fallback**, so the OS boots without artwork.
- Graphics target chosen by boot evidence: **bochs-display** (PCI BAR framebuffer),
  because virtio-gpu's scanout never reached the QEMU surface under `-display none`.

### 4.10 Tests
- 12 suites, **579 `CHECK`s** (measured): `action agent aiproc autopilot durable fb json
  kernel mem sandbox sembus shell`. In-tree boot logs record **11 passed, 0 failed**
  (they predate M1's `test_action`)
  (including `test_fb` 89 checks). The same suites run **inside the booted OS**.

## 5. What is missing — the short list

Full analysis with severity and file-level detail: **`02-WHAT-IS-MISSING.md`**.

**A. The constitution has no implementation (highest severity).**
Policy engine · Action Descriptor · Sandbox-Result/diff schema · staged
filesystem writes with tombstones and promotion · snapshots & rollback ·
authority model (human > host > VM > AI > sandbox) · input arbitration ·
immutable (hash-chained) audit · consent/capability gates.

**B. OS engineering gaps.**
No cross-process IPC (sembus is a per-process object) · `aiproc` unused by the
shell · no networking at all · no user/identity separation · rootfs is a RAM
initramfs with no overlay/persistence apart from the 8 MiB data disk · no
locking on the shared store · no A/B or signed updates · no observability
(dmesg/log/trace UI) · no CI · x86_64 only (the original plan wanted a
Raspberry Pi) · no LICENSE, no `.gitignore` (added), 54 MB of binaries in git.

## 6. Where we are going: adopt Linux parts instead of reinventing them

The user's direction: **"we will eventually add Linux to this, use bits and
parts of Linux in this AI operating system."** The repo already assumes that
(Linux kernel stays). The next move is to pick *which* Linux subsystems become
our mechanisms:

| Need | Linux part to adopt (instead of building) |
|---|---|
| Real sandbox isolation | `namespaces` (pid/net/mnt/user) + `pivot_root`, or **microVM** |
| Real rollback | **overlayfs** upper/lower + **dm-snapshot** / btrfs subvolumes |
| Staged writes & diffs | **overlayfs** upper dir + **fanotify/inotify** |
| Mediated syscalls (AI asks, OS decides) | **seccomp user-notify** (`SECCOMP_RET_USER_NOTIF`) |
| Policy enforcement that cannot be bypassed | **Landlock** + **BPF LSM** |
| Cross-process semantic bus | **unix sockets** + `SCM_RIGHTS`, `netlink`, `memfd`, **vsock** |
| Better scheduler signal | **PSI** (`/proc/pressure/*`) + cgroup v2 (already used) |
| Host ↔ VM control plane | **virtio-vsock**, **virtio-fs**, KVM |
| Binary trust | **fs-verity** / IMA, **module signing**, measured boot |
| Immutable base image | **erofs/squashfs** + overlay; **A/B slots** |
| Update mechanism | signed image replacement, A/B switch |

Phased plan, with acceptance criteria per phase: **`03-LINUX-INTEGRATION.md`**.

## 7. Decision rules that must survive every change

From `os_sys_improvements.md` §13 and the constitution — quote these, do not
paraphrase them into something weaker:

1. **Capability may scale. Authority may not.**
2. Safety is enforced by **architecture** (isolation, mediation, rollback), never by prompts.
3. Rollback is **physics**, not an error path; it cannot be disabled.
4. There is exactly **one visible computer**; the sandbox is disposable and
   produces **results only** (no state, no memory, no learning).
5. Human input **always** preempts AI input; the AI never injects input directly.
6. The AI **plans, never enforces**; a veto is accepted, never negotiated.
7. Binaries are **stateless**; the OS owns state in the durable store.
8. **Zero external dependencies** in the runtime (C11 + pthreads only); build
   must stay warning-free; the initramfs **size cap** must hold.

## 8. Current focus and next three actions

**Focus:** close the gap between the constitution and the code, using Linux
primitives as the mechanisms — without regressing the working boot.

1. **Unblock the build** (done in this snapshot, see §9) and make
   `make && make test` green on a clean machine (font header problem).
2. **Wire the safety spine** at the cheapest possible point of entry: one
   `Action Descriptor` → policy evaluation → staged write in an overlay upper
   dir → diff → promote/rollback. This is the smallest thing that makes the
   constitution true.
3. **Make processes real**: put the semantic bus on a unix socket (+ `SCM_RIGHTS`),
   give every agent its own pid/namespace and a cgroup, and let PID 1 supervise
   the governor.

## 9. Changes made in this snapshot (2026-11-16)

- **Fixed a hard build failure** on modern toolchains: `src/ksched.c` and
  `boot/probe.c` included both `<linux/sched/types.h>` and glibc's `<sched.h>`,
  which **redefine `struct sched_param`** on glibc ≥ 2.36 (gcc 12/Debian 12).
  `make` failed with `error: redefinition of 'struct sched_param'`. Both files
  now declare `struct sched_attr` directly (`probe.c` also got `_GNU_SOURCE`,
  it was using `syscall(2)` undeclared).
- **Restored the "warning-free" claim** from `docs/HARDENING.md`: 3
  `-Wsign-compare` warnings in `src/shell.c` (loop counters) and one
  `-Wformat-truncation` in `src/ebpf.c` (verifier-log tail) are fixed.
  Verified: **26/26 translation units compile with 0 warnings** under the v1.0
  flag set, and the suite runs — **9 pass**; `test_fb` fails only because a
  stub font table was used (real font needs Pillow), `test_kernel` fails on
  Landlock because this sandbox forbids it (the guest enforces it — see
  `boot-test2.log`).
- **Added `.gitignore`** for build artifacts, `source/boot/dl/` fetches and
  runtime stores.
- **Added this `context/` folder.** No runtime behaviour was changed.

## 10. How to update this memory (protocol)

1. Change code → run `make && make test` → update `04-BUILD-VERIFY.md` with the
   new numbers **and the command you ran**.
2. Add/close a gap → move the line in `02-WHAT-IS-MISSING.md`, never delete it;
   record the decision in `05-DECISIONS.md` with date + rationale + what it costs.
3. Bump `Version in tree` at the top of this file and the snapshot date.
4. Keep `CONTEXT.md` short. Detail belongs in the numbered files.

## 11. Glossary

`aiproc` intent-driven process · `sembus` semantic message bus · `aiosh` the AI
shell · `cortex` the planner backend (scripted/micro/llm) · `tool-exec` the
sandboxed worker · `intent class` INTERACTIVE/BATCH/IDLE ·
`micro-LM` the OS's own 12 KB model · `durable store` `/data/aios` on ext2 ·
`the constitution` = `os_sys_decided.md` + the four contract `.md`s at the root.

---

## Files in this folder

| File | For whom | Why open it |
|---|---|---|
| `CONTEXT.md` | everyone | the memory (this file) |
| `CONTEXT.agent.md` | coding agents (me, Cursor, Copilot, Claude Code) | commands, traps, invariants, how to add a builtin/tool |
| `CONTEXT.chatgpt.md` | ChatGPT (self-contained paste) | strategy, gap analysis, planning |
| `RULES.md` | **anyone writing code** | universal + AI-OS rules, build traps, definition of done |
| `01-WHAT-IS-HERE.md` | you | the inventory, subsystem by subsystem, with evidence |
| `02-WHAT-IS-MISSING.md` | you | prioritized gap list (G1…G22) + severity + fix sketch |
| `03-LINUX-INTEGRATION.md` | you | which Linux parts to adopt, in what order, with acceptance criteria |
| `04-BUILD-VERIFY.md` | you | exact commands, what passed, what failed and why |
| `05-DECISIONS.md` | you | decision log + constitution-vs-code conflicts and how to resolve them |
| `ROADMAP-TO-WINDOWS.md` | you | **the window to v5.0: W0–W6 milestones + phases + exit tests** |
| `todo/00-MASTER.md` | you | the near-term phase plan + index + progress tracker (W0 of the roadmap) |
| `todo/03-GUI-TASKS.md` | ChatGPT | desktop/app/UI cards (notepad, files, launcher, resize…) |
| `todo/04-SMALL-TASKS.md` | ChatGPT | tiny ≤1 h cards |
| `prompts/` | everyone | named, copy-paste prompts (`00`…`09`) |
