# AI-OS — AI-Native Operating System: Milestone Roadmap

AI-OS is an AI-native operating system built on the Linux kernel with a
rewritten system services layer. AI is a first-class citizen of the OS:
processes, IPC, memory, and scheduling are designed around intents, agents,
and semantic state — not around AI applications bolted onto a classic OS.

Architecture principle: **kernel stays Linux; everything above it is ours.**

## Milestones

| ID | Codename | Scope | Status |
|----|----------|-------|--------|
| M1 | `foundation` | Repo, build system, core types, versioning, coding standard | DONE |
| M2 | `aiproc` | AI-native process model: Intent -> TaskGraph -> lifecycle (INTENT/READY/RUNNING/BLOCKED/DONE/FAILED), cooperative cancellation | DONE (core) |
| M3 | `sembus` | Semantic IPC bus: typed topics, wildcard subscription, JSON payloads | DONE (core) |
| M4 | `agent-runtime` | **Agent runtime + memory subsystem**: agentic loop (observe-plan-act-reflect), pluggable cortex (LLM/scripted), tool registry, sandboxed tool execution, 3-tier memory (working/episodic/semantic) with hashed n-gram embeddings, recall scoring, journal persistence, consolidation, audit log | DONE |
| M5 | `aiosh` | The AI shell: natural-language command front-end on top of agents, jobs, pipes | DONE |
| M5.5 | `bootable-image` | **First bootable, testable OS image**: Linux bzImage + AI-OS initramfs; `aios-init` as PID 1 (mounts, hostname, selftest phase, aiosh bring-up, console respawn loop); full M1-M5 test suite executed *inside the booted OS*; headless QEMU boot test + interactive console test harnesses | DONE |
| M6 | `kernel-bridge` | Kernel coupling: LKM intent device (`aios_intent.ko`: /dev/aios registry + /proc/aios wakeup accounting via kprobe), eBPF wakeup observer (hand-assembled prog on sched:sched_wakeup), cgroup v2 CPU budgets per intent class, sched_setattr policies, seccomp allowlist + Landlock jail in the tool worker | DONE |
| M7 | `autopilot` | **Closed-loop AI scheduler + the OS's own language model**: `aios-governor` daemon (kernel/eBPF/cgroup signals -> pure policy -> live cgroup budget changes, JSONL journal, state file); micro-LM (`microlm.c`, trained at build time by `tools/microlm_gen.py`): intent classifier (bg/urgent/normal) + bigram generator, deterministic, no network; NL scheduling classes in aiosh (background wording spawns IDLE-class jobs), `lm`/`autopilot`/`tune` builtins; 2nd eBPF observer (sched_switch ctx-switch counts); cgroup fix: real cpu.max/cpu.stat in the class groups | DONE |
| M8 | `durable` | Persistent state: ext2 side-disk so memory/jobs/audit survive reboot, boot-replay of the last journal, crash-consistent writes | DONE (0.8.0) |
| M9 | `v1-rc` | Release engineering: hardening pass, complete docs, install/upgrade path, performance pass, final acceptance -> **v1.0** | DONE (1.0.0) |
| M10 | `desktop` | **GUI milestone**: framebuffer desktop (aios-fb core, window manager, aios-desktop shell with NL launcher, apps: terminal/files/agents/memory/autopilot), boot splash, artist-supplied artwork (`desktop/assets/ASSETS.md`), procedural fallbacks for every asset | DONE (1.1.0) |

## M4 acceptance criteria (met by this build)

1. Agent runs a full observe -> plan -> act -> reflect loop to completion.
2. Two cortex backends: `llm` (real model via `z-ai chat`) and `scripted`
   (deterministic, for offline tests/CI).
3. Tools execute in a separate sandboxed worker process with rlimits,
   wall-clock timeout, path jailing; kills are enforced by the parent.
4. Memory survives process restart; recall ranks relevant entries first;
   consolidation dedupes and promotes episodic facts to semantic tier.
5. Every tool call is journaled to an append-only audit log.
6. `make && make test` passes all unit/integration tests.

## M5 acceptance criteria (met by this build)

1. `aiosh` runs interactively, from a script file, and via `aiosh -c`;
   the engine (`src/shell.c`) is a library API (`aios/shell.h`), fully
   testable without a TTY.
2. Line grammar: `;` sequences (stop on first failure), `|` pipelines
   carrying text+JSON between stages, quoting, `key=value` args,
   `{json}` args, and variables (`$name`, `${name}`, `$out`, `$?`,
   `$json`, `$$`).
3. Command resolution order: builtin -> registered tool -> natural
   language. The intent router (scripted heuristics, or the llm cortex
   through the shared z-ai bridge) picks exactly one of: run a tool,
   start an agent, reply directly.
4. Tool-aware pipe rendering: `fs.read X | count` counts file lines,
   `calc` prints the number, `fs.list` prints names; JSON stays
   available via `$json`.
5. Background agent jobs: `spawn` forks a child with its own memory,
   tool registry and cortex; results land in `<store>/jobs/<id>.json`;
   `jobs`, `wait [id|all]` and `kill id` supervise them.
6. Every line publishes `sh.cmd` on the shell's semantic bus, agent
   runs publish `sh.agent.start`/`sh.agent.done`, spawns publish
   `sh.spawn`; `emit` publishes user events.
7. Memory is a shell service: `remember`/`recall` builtins plus pipe
   injection into `memory.search`.
8. `make && make test` passes; `test_shell` alone runs 100 checks.

## M5.5 acceptance criteria (all verified in this build)

1. `make -f boot/Makefile boot-image` produces a bootable AI-OS image:
   `boot/dl/kernel/boot/vmlinuz-virt` (Linux 6.6, Alpine linux-virt) +
   `boot/build/aios-initramfs.cpio.gz` (~5 MB, statically linked AI-OS
   userland, busybox jail, device nodes, workspace).
2. `aios-init` (boot/init.c) is PID 1: mounts proc/sys/devtmpfs/tmpfs,
   sets hostname, runs the **selftest phase** (all 7 M1-M5 test suites,
   300+ checks, executed inside the booted OS), runs the system
   bring-up script (`/etc/aios/boot.aiosh`) through **aiosh**, prints
   `AIOS-BOOT-OK selftest=PASS`, then respawns an interactive **aiosh**
   console forever (respawn loop, emergency shell fallback). PID 1
   never exits; cmdline flags `aios.mode=test`, `aios.selftest=0`.
3. The bootable OS is genuinely AI-native at boot: the bring-up
   exercises the sandboxed tool worker (`6*7` -> 42), OS-owned memory
   (`remember` at boot is `recall`-able from the interactive shell),
   and publishes the boot event on sembus (`boot.up`).
4. `boot/boot-test.sh` boots the image headless in QEMU (CI mode:
   `aios.mode=test` -> selftest -> AIOS-BOOT-OK -> poweroff), asserts
   the serial log markers and exits 0/1. `boot/console-test.sh` boots
   interactively, drives the aiosh console over serial, and asserts
   live shell answers.
5. The initramfs is packed without host root by `boot/mkcpio.c`
   (cpio newc writer that can emit device nodes) and verified by
   `boot/verify-cpio.py` (header/alignment/entry/ELF checks).
6. Host suite still green: `make && make test` = 7/7 suites.

## M6 acceptance criteria (all verified inside the booted OS)

1. `aios_intent.ko` (boot/module/aios_intent.c) loads at boot; `/dev/aios`
   accepts REGISTER/UNREGISTER/QUERY ioctls; `/proc/aios` lists every
   registered pid with its class and kernel-counted wakeups. aiosh
   registers as INTERACTIVE, every tool-exec worker as BATCH, every
   spawned background job as IDLE (verified: ~47 entries at boot).
2. The kernel counts wakeups per intent class via a kprobe on
   `try_to_wake_up` (sched_switch tracepoint is not module-exported);
   per-class counters are visible in `/proc/aios`.
3. `aios_kernel_status()` (the `kernel` builtin) reports the registry,
   cgroup budgets, current sched policy and the eBPF observer state.
4. eBPF: a 21-insn hand-assembled program (zero external dependencies,
   no libbpf, no clang) counts wakeups per pid in a hash map, attached
   to `sched:sched_wakeup` via perf_event_open. Verifier-approved inside
   the image; observer state is surfaced in `kernel` and asserted by
   `test_kernel`.
5. cgroup v2: init creates `aios/{system,tools,agents}` with cpu.max
   50%/25% budgets; tool workers and background jobs are moved into
   their class groups at spawn time.
6. Sandbox hardening (env-gated by init via `AIOS_SANDBOX_HARD`): the
   tool worker installs a Landlock ruleset (filesystem view limited to
   the jail, ABI v3) and then a seccomp cBPF allowlist (~50 syscalls;
   exec/fork/socket/mount/bpf are SIGSYS). Applied AFTER the Landlock
   setup so hardening cannot lock itself out; jail path is resolved to
   an absolute path before chdir.
7. `test_kernel` (suite 8/8) covers registry ioctls, per-policy
   sched_setattr round-trips (in forked children), cgroup membership,
   seccomp kill/allow behaviour, Landlock jail enforcement and eBPF
   counting — with honest SKIPs on hosts lacking a facility.
8. `make && make test` = 8/8 on the host; `boot-test.sh` and
   `console-test.sh` pass inside the image (selftest: 8 suites, AIOS-
   BOOT-OK selftest=PASS, eBPF observer active, kernel bridge live).

## M7 acceptance criteria (all verified inside the booted OS)

1. `aios-governor` (apps/aios-governor.c) runs as an OS daemon, started
   by init (cmdline `aios.autopilot=off|static|balanced|aggressive`,
   default balanced). Each tick it gathers three scheduler-pressure
   signals — per-class wakeup deltas (/proc/aios, M6), per-class
   ctx-switch deltas (new eBPF sched_switch observer), per-class CPU
   usage (cgroup cpu.stat) — feeds the PURE policy in `autopilot.c`,
   applies budget changes by writing cpu.max, and reports to
   /var/lib/aios/autopilot-state.json + a JSONL journal.
2. The policy is explainable and unit-tested: a class pinned at its cap
   with kernel-visible runnable work gets budget LENT from a quiet
   class (total caps never grow); borrowed budgets return to base after
   3 quiet ticks (hysteresis); off/static modes never adjust.
3. The micro-LM (`src/microlm.c`, weights generated by
   `tools/microlm_gen.py` — 18/18 selftest phrases) gives the OS its own
   very small language model: a naive-Bayes intent classifier
   (bg/urgent/normal) and a deterministic bigram generator. Zero
   network, zero runtime training, ~12 KB of weights. Bigger LLMs plug
   into the same seam later (`cortex llm`) without OS changes.
4. LLM-first boot: init exports AIOS_SHELL_CORTEX=micro; the bring-up
   script routes through the micro-LM (`lm classify` -> class=bg),
   speaks OS-flavoured prose (`lm gen`), and natural language with
   background/idle wording spawns a real IDLE-class background job
   (`later, summarize the workspace notes` -> `[job 1] spawned`).
5. aiosh gains `lm`, `autopilot`, `tune` builtins and the `micro`
   cortex mode; `kernel` reports both eBPF observers + governor state.
6. Second eBPF observer: the same 22-insn hand-assembled counter
   template attached to sched:sched_switch, counting per-pid switch-outs
   (descheduling pressure); active in the image, surfaced in `kernel`.
7. cgroup fix (inherited M6 bug): init now moves PID 1 into
   aios/system (leaf) and enables +cpu on the process-free aios group —
   cpu.max/cpu.stat actually exist in the class groups now; budgets are
   live-read and enforced (tools=50%, agents=25% verified in-image).
8. `test_autopilot` (suite 9/9) covers the classifier, generator
   determinism, policy decisions (lend/hold/return/aggressive), cpu.max
   math, conf io, and — best-effort — /proc/aios totals, live cgroup
   round-trips and eBPF ctx counting.
9. Hard size cap honoured: the m7 initramfs is 5,901,655 bytes —
   43,313 bytes UNDER the m6 image (5,944,968). Achieved with
   -Os + -ffunction-sections/-fdata-sections + -Wl,--gc-sections and
   stripped binaries; the boot Makefile now FAILS the build if any
   future image exceeds the m6 cap.
10. `make && make test` = 9/9 on the host; boot-test.sh asserts the m7
    banner, 9-suite selftest, governor liveness and micro-LM markers;
    console-test.sh drives `lm classify`, `lm gen` and `autopilot` live.

## M8 acceptance criteria (all verified inside the booted OS)

1. **Real ext2 side disk, no host tools needed**: `tools/mkfs_ext2.py`
   builds a deterministic 8 MiB ext2 (rev 1, 1 KiB blocks, single group,
   filetype, `/aios` pre-created) with zero dependencies; the kernel's
   own ext2 driver mounts it (modules `virtio_blk` + `mbcache` + `ext2`
   shipped in the initramfs, loaded by init).
2. Crash-consistent store layer (`src/durable.c`): atomic replacement =
   temp file + fsync + rename + directory fsync; journal appends =
   O_APPEND + fsync; the replay reader drops a torn tail (a line without
   its newline = crash mid-append). Unit-tested including the torn-tail
   case and path-escape rejection.
3. Memory, audit and job results persist **by construction**: with the
   disk mounted, init points `AIOS_STORE` at `/data/aios/store`, where
   mem.c and tools.c already fsync their journals; job spawn/exit
   records are appended to `store/jobs.jsonl` with fsync.
4. Boot replay: init restores `/data/aios/var/*` into `/var/lib/aios`
   (autopilot conf/state/journal), reports the previous session's
   counts (`jobs=1 mem=9 audit=6 autopilot=1` style), logs the boot
   number from the append-only `boots.jsonl`, and the governor prints
   its replayed journal tail.
5. Job history survives reboots: a fresh shell replays `jobs.jsonl`
   (done/failed states, final texts from the per-job result documents,
   id sequence continues) — `jobs` on boot 2 shows session 1's work.
6. Autopilot state survives: conf (via `tune`), per-tick state JSON and
   the adjustment journal are mirrored to the disk every tick; the
   governor on boot 2 continues from the restored journal.
7. `dur` builtin: durable-store status (root, boot count, memory,
   audit, job and autopilot journal counts, whether the shell's store
   is on the disk) + `dur sync`; `aios.durable=0` cmdline disables the
   disk and the OS behaves exactly like m7 (RAM-only).
8. Reboot proof in CI: boot-test.sh now runs TWO boots on the same
   disk — phase 1 writes state and powers off cleanly, phase 2 must
   report the previous session's jobs/memory/audit/autopilot counts,
   boot #2, and the governor's replayed journal, or the test fails.
9. Hard size cap honoured against the m7 image: the m8 initramfs is
   1,642,820 bytes — 4,258,835 bytes UNDER the m7 cap (5,901,655).
   The initramfs moved from gzip to xz (kernel RD_XZ=y), which absorbed
   the three new kernel modules and the durable layer and still shrank
   the image to 28% of its m7 size. The boot Makefile fails the build
   if any future image exceeds the m7 cap.
10. `make && make test` = 10/10 on the host (suite 10 = `test_durable`:
    atomicity, torn-tail replay, mirror/restore, escape rejection,
    job-journal replay through a real shell, plus a live write to the
    real side disk when run inside the OS); boot-test.sh asserts the
    two-phase reboot proof; console-test.sh drives `dur` live.

## The road to v1.0 — travelled

**v1.0 shipped (M9, 1.0.0):** version bump everywhere; install path
(`make install`, `tools/install.sh`); upgrade path (`UPGRADE.md`);
hardening pass (warning-free build with stack-protector + FORTIFY,
`docs/HARDENING.md`); performance pass (`docs/PERFORMANCE.md`,
`tools/perf-run.sh`); size ratchet tightened to the m8 image and held
(1,611,872 bytes, 31,060 under); full acceptance run recorded in
`docs/ACCEPTANCE-v1.0.md` (10/10 host suites, two-boot durable proof,
live console test).

After v1.0 comes the GUI milestone:

- **M10 `desktop` — DONE (1.1.0-m10)**. The spike chose the graphics
  path by boot evidence: virtio-gpu's scanout never reaches the QEMU
  surface under `-display none` (its fbdev uploads resources through
  the DRM command queue), while `bochs-display` is a plain PCI-BAR
  framebuffer — guest mmap writes are displayed directly. So the OS
  ships an 18-module gzipped stack (fb core + helpers, DRM, TTM,
  bochs.ko, evdev + virtio_input, ~845 KB) that aios-init gunzips and
  insmods at boot, then draws a splash on /dev/fb0. `aios-fb` (double
  buffer, damage rects, 8x16 glyph blits from a build-time DejaVu
  table, PNG->raw-RGBA pipeline with procedural fallbacks for all 14
  ASSETS.md slots) underlies the window core (z-order, focus, title
  bars, cursor, damage-driven compositor) and `aios-desktop` (5 app
  windows, NL launcher bar on the same micro-cortex aiosh, sembus
  toasts). init supervises TWO console front-ends forever: the desktop
  on the framebuffer and aiosh on serial; `aios.gui=0` skips the GPU
  stack entirely; five desktop crashes fall back to text. The
  documented size-cap raise: 1,642,932 -> 2,605,888 (first M10 image
  + 128 KB headroom), pinned in `boot/.m10-size-cap`.

Post-M10 backlog (not scheduled): `mesh` (TCP transport for the
semantic bus — agents across VMs), `cockpit` (live mission-control
dashboard).
