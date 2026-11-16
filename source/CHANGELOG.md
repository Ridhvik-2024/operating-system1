# AI-OS — Changelog

All notable changes to AI-OS. The OS is milestone-driven; each release
is one milestone (see ROADMAP.md).

## 1.1.0-m10 — M10 `desktop` (the GUI release) — 2026-11-16

The AI-native desktop. The OS now has a graphical face, and the shell
you talk to in it is the same aiosh engine as the serial console:

- **Kernel evidence first**: the spike picked `bochs-display` over
  `virtio-gpu` — virtio-gpu's fbdev uploads pixels via DRM commands
  that never reach the QEMU surface under `-display none`, while
  bochs-display is a plain PCI-BAR framebuffer (guest mmap writes are
  visible immediately). 18 gzipped modules (~845 KB) ship in the
  image; `aios-init` gunzips + insmods them at boot in a spike-proven
  order (fb helpers -> fb core -> DRM -> bochs -> evdev).
- **aios-fb** (`src/fb.c`, `include/aios/fb.h`): double buffer,
  damage-rect union + row-wise flush, alpha blending, 8x16 glyphs from
  a build-time DejaVu Sans Mono table (`tools/gen_font.py`), the
  `.aiosimg` image format (magic + dims + raw RGBA) with the build
  pipeline `tools/img2aiosimg.py`, and procedural fallbacks for every
  ASSETS.md asset — the OS never boots without a face.
- **Window core** (`src/win.c`, `include/aios/win.h`): z-ordered
  windows with title bars/close boxes/focus, hit testing, cursor
  sprite, damage-driven full-screen compositing (flush stays
  damage-bounded).
- **Input** (`src/input.c`, `include/aios/input.h`): raw evdev — US
  keymap with shift/ctrl/alt/caps/numlock, absolute tablet pointer,
  arrow/F-key codes for the line editor; kernel autorepeat filtered;
  unmapped keys never emit events (a raw evdev code is not ASCII).
- **aios-desktop** (`apps/aios-desktop.c`): the graphical shell —
  wallpaper, launcher bar whose input box speaks natural language to
  the same aiosh engine (micro-LM cortex, `AIOS_SHELL_CORTEX`), five
  app windows (terminal REPL, files browser over /workspace, agents
  jobs list, memory remember/recall, autopilot live state + budget
  bars), sembus toasts, CI marker `AIOS-DESKTOP-OK`.
- **Boot integration**: splash with phase updates on /dev/fb0; init
  supervises the desktop (graphics) AND aiosh (serial) forever;
  five desktop crashes fall back to text; `aios.gui=0` skips the GPU
  stack entirely (headless CI unchanged).
- **Size cap raise (documented)**: 1,642,932 -> 2,605,888 bytes — the
  GPU/input module stack ships once; cap pinned in
  `boot/.m10-size-cap`, history printed by the guard.
- **Tests**: `test_fb` suite (89 checks, host + guest); 11/11 host
  suites; two-boot durable regression green; new `boot/boot-gui-test.sh`
  boots with a GPU + keyboard, screendumps through the QEMU monitor,
  asserts pixels (gradient, accent bar, five title bars, cursor) and
  types `6*7` through the virtio keyboard — the GUI answers `42`
  (screenshots in `download/m10-boot-shots/`).
- **Fixed for everyone**: `boot/mkcpio.c` no longer uses `ftell(stdout)`
  (glibc 2.41 makes it fail on pipes), which since the sandbox refresh
  emitted a broken 512-byte trailer ("Initramfs unpacking failed:
  broken padding" warning in every boot since).

## 1.0.0 — M9 `v1-rc` (the v1.0 release) — 2026-11-16

Release engineering. No new OS subsystems — this milestone makes the
existing ones installable, measurable, reviewed and shippable:

- **Version 1.0.0** across the runtime (`AIOS_VERSION`), the bootable
  image banner (`AI-OS 1.0.0 (v1.0)`), `os-release` and every test
  marker. The shell's `version` builtin reports `milestone: v1.0`.
- **Install path**: `make install` / `make uninstall` (`PREFIX`,
  `DESTDIR` aware) — binaries, headers and `libaios.a`; the sandbox
  finds the `tool-exec` worker next to the installed `aiosh` via
  `/proc/self/exe`, no environment setup needed. `tools/install.sh`
  wraps the whole flow (build -> 10-suite test -> install -> verify).
- **Upgrade path**: `UPGRADE.md` — binaries are stateless, the
  OS-owned store (`/data/aios` in the booted OS, `./.aios-store` on
  the host) survives binary replacement; boot users keep `data.img`.
- **Hardening pass**: `docs/HARDENING.md` (threat model, sandbox layer
  map, claim->test matrix). Build hardening: `-fstack-protector-strong`,
  `-D_FORTIFY_SOURCE=2`, `-Wformat -Wformat-security` on the host
  build; the tree is now **warning-free** under the full flag set
  (guarded `_GNU_SOURCE` defines, `warn_unused_result` handled in
  tests, provably-bounded path joins in `test_durable`). Boot image
  keeps `-Os` + section GC to honour the size cap — documented
  tradeoff.
- **Performance pass**: `docs/PERFORMANCE.md` with measured numbers
  (build, per-suite test times, shell line latency, NL routing, boot
  timeline), reproducible via `tools/perf-run.sh`.
- **Size cap tightened**: the boot Makefile cap is now the m8 shipped
  image (1,642,932 bytes); the v1.0 image builds **1,611,872 bytes —
  31,060 under the cap** (the guard held despite a gcc-14-rebuilt LKM:
  the dead M5.5-era `sbin/probe` was removed from the image). The cap
  auto-fails any future bloat.
- **Docs completion**: `INSTALL.md`, `UPGRADE.md`, `CHANGELOG.md`,
  `docs/HARDENING.md`, `docs/PERFORMANCE.md`, `docs/ACCEPTANCE-v1.0.md`;
  README and BOOT-GUIDE updated to v1.0.
- **Final acceptance**: 10/10 host suites; two-boot durable CI proof
  PASS; live console test PASS. Numbers in `docs/ACCEPTANCE-v1.0.md`.

## 0.8.0-m8 — `durable` — persistent state

- ext2 side disk (8 MiB, `tools/mkfs_ext2.py` builds it with zero host
  tools) mounted by init as `/data`; kernel modules `virtio_blk`,
  `mbcache`, `ext2` shipped and loaded by init.
- Crash-consistent store layer `src/durable.c`: atomic replace =
  temp + fsync + rename + dir fsync; journals = O_APPEND + fsync;
  replay drops a torn tail.
- Memory, audit, job history, autopilot conf/state/journal persist;
  boot replay restores the previous session and the OS counts its own
  boots (`boots.jsonl`); `dur` / `dur sync` builtins;
  `aios.durable=0` reverts to RAM-only.
- Two-boot CI proof (`boot-test.sh` runs two boots on one disk).
- Initramfs moved gzip -> xz: 1.64 MB with three new kernel modules —
  72% under the m7 cap.

## 0.7.0-m7 — `autopilot` — closed-loop AI scheduler + the OS's own LM

- `aios-governor` daemon: reads per-class wakeups (/proc/aios), ctx
  switches (2nd eBPF observer, sched_switch) and cgroup cpu.stat;
  pure policy lends CPU budget between cgroup class groups
  (lend-or-hold invariant, 3-tick hysteresis), writes cpu.max live and
  journals every decision.
- micro-LM: build-time trained statistical model (~12 KB weights,
  zero network): naive-Bayes intent classifier (bg/urgent/normal) +
  bigram generator; `lm` builtin; `AIOS_SHELL_CORTEX=micro`.
- Natural-language scheduling classes in aiosh: background wording
  spawns IDLE-class jobs; urgent wording stays interactive.
- cgroup v2 fix: real cpu.max/cpu.stat in the class groups.
- Initramfs 5,901,655 bytes — under the m6 image, enforced by a hard
  size guard.

## 0.6.0-m6 — `kernel-bridge`

- `aios_intent.ko`: /dev/aios intent registry (INTERACTIVE/BATCH/IDLE)
  + /proc/aios with kprobe-counted per-class wakeups.
- eBPF wakeup observer: 21-insn hand-assembled program (no libbpf, no
  clang) on sched:sched_wakeup.
- cgroup v2 CPU budgets per intent class (tools <=50%, agents <=25%),
  sched_setattr policies.
- seccomp allowlist + Landlock jail inside the tool worker
  (`AIOS_SANDBOX_HARD`).

## 0.5.5-m5.5 — `bootable-image`

- aios-init as PID 1 (mounts, hostname, selftest phase, bring-up,
  console respawn loop), no-root cpio packer with device nodes,
  headless QEMU boot test + interactive console test.

## 0.5.0-m5 — `aiosh`

- The AI shell: pipelines, variables, quoting, JSON args, tool stages;
  scripted/LLM intent routing (builtin -> tool -> natural language);
  background jobs (spawn/jobs/wait/kill) as separate supervised
  processes with their own memory/registry/cortex; sembus events for
  every line.

## 0.4.0-m4 — `agent-runtime`

- observe->plan->act->reflect loop; pluggable cortex (llm / scripted);
  tool registry + sandboxed `tool-exec` worker (rlimits, timeout,
  jail); 3-tier OS-owned memory (working/episodic/semantic, hashed
  n-gram embeddings, salience recall, consolidation); append-only
  audit log; no-duplicate-call loop discipline.

## 0.3.0-m3 — `sembus`

- Typed semantic IPC bus: topics, wildcard subscriptions, JSON
  payloads.

## 0.2.0-m2 — `aiproc`

- AI-native process model: Intent -> TaskGraph -> lifecycle
  (INTENT/READY/RUNNING/BLOCKED/DONE/FAILED), cooperative
  cancellation.

## 0.1.0-m1 — `foundation`

- Repo, build system, core types, JSON, status codes, versioning,
  coding standard.
