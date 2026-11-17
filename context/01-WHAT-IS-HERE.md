# 01 — WHAT IS HERE (inventory with evidence)

Everything below was read from the code in this tree on 2026-11-16. Where a
line number is given it is `file:line` inside `source/`. Numbers like "11
suites" are taken from the checked-in boot logs (`boot-test1.log`,
`boot-test2.log`) **and** independently re-run where possible
(`04-BUILD-VERIFY.md`).

## 0. Size and shape

| Item | Value |
|---|---|
| Runtime code (`src/` + `include/aios/` + `apps/`) | 10 243 lines of C11 |
| Boot code (`boot/init.c`, `mkcpio.c`, `probe.c`, LKM) | 1 205 lines |
| Tests | 2 053 lines, 11 suites, ~378 checks |
| External runtime dependencies | **none** (C11 + pthreads + libm) |
| Build-time deps | `cc`, `make`; `python3` + **Pillow** for font/PNG; `xz`; `qemu-system-x86_64` to run |
| Tracked repo size | 54 MB, of which ~52 MB is committed binaries |
| Shipped initramfs | 2 476 384 bytes (xz), 103 entries, cap pinned in `boot/.m10-size-cap` |
| Kernel | `vmlinuz-virt` = Linux **6.6.142** (Alpine `linux-virt`), `RD_XZ=y` |

## 1. Boot and init (M5.5 → M10)

`source/boot/init.c` — PID 1, 548 lines, phase order:

1. banner (`AI-OS 1.1.0-m10`), `signal(SIGCHLD, SIG_DFL)`
2. mounts: `proc`, `sysfs`, `devtmpfs`, `tmpfs` (`/tmp`), **`cgroup2`**
   (`/sys/fs/cgroup`), `tracefs` — `init.c:389-394`
3. **durable phase**: `insmod virtio_blk → mbcache → ext2`, mount `/data` from
   `/dev/vda` (or `sda`), replay previous session, print boot number — `init.c:263-350`
4. env contract: `AIOS_STORE=/data/aios/store`, `AIOS_DUR=1`,
   `AIOS_TOOL_EXEC=/sbin/tool-exec`, `AIOS_SANDBOX_HARD=1`,
   `AIOS_SHELL_CORTEX=micro` — `init.c:342-432`
5. **kernel bridge**: `insmod /lib/modules/aios_intent.ko`, then
   `aios_cgroup_boot()` creates `aios/{system,tools,agents}` — `init.c:413-423`
6. **GUI phase**: gunzip + `insmod` 18 modules in a proven order, splash on
   `/dev/fb0`, degrade cleanly to text if `/dev/fb0` never appears — `init.c:159-262`
7. **governor**: `fork` + `execv("/sbin/aios-governor")` — `init.c:439-449`
8. **selftest phase**: runs all suites inside the booted OS, prints
   `AIOS-SELFTEST: PASS` / `AIOS-BOOT-OK selftest=PASS`, powers off in CI mode
9. bring-up script `/etc/aios/boot.aiosh` executed through `aiosh`
10. **supervision loop** (`init.c:494-548`): respawns `aios-desktop` (graphics)
    and `aiosh` (serial) forever, 5-crash fallback to text / emergency busybox shell

Cmdline knobs: `aios.mode=test`, `aios.selftest=0`, `aios.gui=0`,
`aios.durable=0`, `aios.autopilot=off|static|balanced|aggressive`.

Initramfs contents (verified by unpacking `aios-initramfs.cpio.xz`):
`/init`, `/sbin/{aiosh,aios-desktop,aios-memctl,aios-governor,tool-exec}`,
`/bin/busybox` + 31 applets, `/lib/modules` (4 .ko + 16–18 GPU .ko.gz),
`/usr/lib/aios/tests/*` (11 binaries), `/usr/bin/aios-selftest`,
`/etc/aios/boot.aiosh`, `/etc/os-release`, `/workspace` (3 sample files), `/dev` nodes.

## 2. Kernel bridge (M6)

| Piece | File | What it does |
|---|---|---|
| Intent device LKM | `boot/module/aios_intent.c` (341 l) | `/dev/aios` ioctls REGISTER/UNREGISTER/QUERY; `/proc/aios` prints pid, class, wakeups; **kprobe on `try_to_wake_up`** counts per-class wakeups; built out-of-tree against Alpine headers by `module/build.sh` |
| eBPF observers | `src/ebpf.c` (248 l) | 21–22 insn programs hand-assembled in C (no libbpf/clang), attached via `perf_event_open` to `sched:sched_wakeup` and `sched:sched_switch`, per-pid hash map |
| Scheduler intents | `src/ksched.c` (391 l) | INTERACTIVE→`SCHED_OTHER,nice -5`; BATCH→`SCHED_BATCH,nice 5`; IDLE→`SCHED_IDLE`, all via `syscall(__NR_sched_setattr)`; cgroup v2 class groups + `cpu.max` set/get/give-back |
| MAC hardening | `src/hardening.c` (214 l) | inline cBPF seccomp allowlist (~54 syscalls) and raw Landlock ABI v3 ruleset; both applied inside `tool-exec` when `AIOS_SANDBOX_HARD=1` |

Live evidence from `boot-test2.log`:
```
aios: intent device ready — /dev/aios, /proc/aios (kernel bridge 0.6.0)
cgroup v2: aios/tools  cpu.max = 50000 100000
cgroup v2: aios/agents cpu.max = 25000 100000
eBPF: sched_wakeup observer active; this pid wakeups=22
eBPF: sched_switch observer active; this pid ctx-switches=22
```

## 3. Autopilot — closed-loop AI scheduler (M7)

- Policy is a **pure function** in `src/autopilot.c` (`aios_ap_tick`), unit
  tested by `tests/test_autopilot.c` (61 checks on the host).
- Inputs per class: wakeup deltas (`/proc/aios`), ctx-switch deltas (eBPF),
  CPU usage (`cpu.stat`).
- Outputs: new `cpu.max` percentages for `tools`/`agents`, **lend-or-hold
  invariant** (total caps never grow), 3-tick hysteresis, human-readable reason
  journalled to `/var/lib/aios/autopilot.jsonl` and mirrored to `/data/aios`.
- Modes: `off`, `static`, `balanced` (default), `aggressive`.

## 4. The OS's own language model (M7)

`src/microlm.c` (225 l) + `src/microlm_weights.h` (generated, `MICROLM_WEIGHTS_VERSION "m7.1"`):
- naive-Bayes classifier over ~200 corpus words → `bg` / `urgent` / `normal`
- bigram generator for short OS-voice sentences
- deterministic, no network, ~12 KB of weights, baked by `tools/microlm_gen.py`
- Used by: the `lm` builtin, the natural-language router (`shroute.c`) to pick a
  scheduling class, and the desktop launcher bar.

## 5. Durable state (M8)

`src/durable.c` (245 l) — crash-consistent primitives:
`aios_dur_atomic_write` = temp + `fsync` + `rename` + parent-dir `fsync`;
`aios_dur_append` = `O_APPEND` + `fsync`; replay drops a torn last line.

Store layout on the 8 MiB ext2 side disk (`data.img`):
```
/data/aios/store/memory.journal    3-tier memory, JSONL
/data/aios/store/audit.log         every tool call, append-only
/data/aios/store/jobs.jsonl        spawn/exit records
/data/aios/store/jobs/<id>.json    per-job result documents
/data/aios/var/                    mirror of /var/lib/aios
/data/aios/autopilot/              governor conf/state/journal
/data/aios/boots.jsonl             one line per boot
```
Proof: `boot-test.sh` boots **twice on the same disk**; logged result —
`restored 2 files, previous session: jobs=1 mem=9 audit=6 autopilot=1`,
`this is boot #2 for this data disk`.

## 6. Shell — `aiosh` (M5)

`src/shell.c` (1 682 l) is the biggest file and the main user surface.

**Grammar**: `pipeline (';' pipeline)*` (stop on first failure),
`command ('|' command)*` (text + JSON flow), quoting (`"…"`, `'…'` no
expansion), `key=value`, `{json}`, variables `$var ${var} $out $? $json $$`.

**24 builtins**: `help version history exit quit echo count grep head tail
remember recall emit set unset vars kernel autopilot tune lm dur cortex jail
store agent spawn jobs wait kill`.

**8 tools** (`src/tools.c`): `calc`, `fs.read`, `fs.write`, `fs.list`
(sandboxed), `memory.search`, `proc.info` (native), `selftest.sleep`,
`selftest.spin` (test-only).

**Intent routing** (`src/shroute.c`, 409 l): resolution order is
`builtin → registered tool → natural language`. The router returns exactly one
of: `TOOL` (single tool + args), `AGENT` (goal for the runtime, with a
scheduling class hint), `REPLY` (answer directly). Scripted heuristics by
default; the same seam accepts `micro` and `llm`.

**Jobs**: `spawn GOAL` → `fork()` + child with its own memory/registry/cortex
(`shell.c:1290-1330`), result to `<store>/jobs/<id>.json`, `jobs/wait/kill`
supervise, events `sh.spawn` on sembus, journal to `jobs.jsonl`.

## 7. Agent runtime + memory (M4)

- `src/agent.c` (282 l): observe → plan → act → reflect loop; observation is a
  JSON document (step, goal, max_steps, tool registry, last result, history);
  no-duplicate-call discipline; tool calls go through the sandbox.
- `src/cortex.c` (415 l): two backends —
  `cortex_scripted` (deterministic, CI/offline; walks list → read → read → summarize)
  and `cortex_llm` (**`execlp("z-ai", "z-ai", "chat", …)`**, `cortex.c:192`).
- `src/mem.c` (446 l): tiers WORKING/EPISODIC/SEMANTIC; 256-dim hashed
  3/4-gram embeddings (FNV-1a); recall `0.62·cos + 0.23·recency + 0.15·usefulness`;
  consolidation dedupes `cos > 0.93`, promotes top 8 (score ≥ 0.30), enforces caps.
- `src/json.c` (444 l): full JSON parser/serialiser, zero deps.

## 8. Sandbox (M4 → M6)

`src/sandbox.c` + `src/tool-exec.c` (410 l). One worker process per tool call:
pipes in/out, `RLIMIT_CPU 15s`, `RLIMIT_AS 256MB`, `NOFILE 64`, `NPROC 64`,
`FSIZE 8MB`, child `alarm(15)` **and** parent SIGTERM→SIGKILL deadline,
`chdir(jail)`, rejects absolute paths, `..`, symlinked final component
(`O_NOFOLLOW`), `PR_SET_NO_NEW_PRIVS`, then Landlock (fs view = jail) followed
by seccomp allowlist. `AIOS_SANDBOX_HARD=1` is set by init inside the image;
on the host it is opt-in.

## 9. Desktop (M10) — the GUI

| Piece | File | Notes |
|---|---|---|
| Framebuffer | `src/fb.c` (496 l) | double buffer, damage-rect union + row-wise flush, alpha blend, 8×16 glyphs from `build/font8x16.h` (generated by `tools/gen_font.py` from DejaVu), `.aiosimg` loader, **procedural fallback for every asset** |
| Windows | `src/win.c` (317 l) | z-order, focus, title bars + close boxes, hit testing, title drag, cursor sprite, damage-driven compositing |
| Input | `src/input.c` (296 l) | raw evdev; US keymap + shift/ctrl/alt/caps/numlock; absolute tablet; kernel autorepeat filtered; unmapped keys ignored |
| Shell app | `apps/aios-desktop.c` (944 l) | wallpaper, launcher bar (NL → same aiosh engine/micro cortex), 5 windows: terminal, files (`/workspace`), agents (jobs), memory (remember/recall), autopilot (live budgets as bars), sembus toasts, `AIOS-DESKTOP-OK` CI marker |

Asset pipeline: `desktop/assets/*.png` → `tools/img2aiosimg.py` → `.aiosimg`
(`AIOSIMG1` magic + dims + raw RGBA). `desktop/assets/` currently ships **only
`ASSETS.md`** — no PNGs, so the OS renders the procedural fallbacks.

## 10. Tests (11 suites, host + guest)

`tests/`: `test_agent` (15), `test_aiproc` (9), `test_autopilot` (61),
`test_durable` (35–38), `test_fb` (89), `test_json` (20), `test_kernel` (20),
`test_mem` (25), `test_sandbox` (26), `test_sembus` (15), `test_shell` (100).
They are compiled into the image and executed **inside the booted OS** by
`boot/aios-selftest.sh`; boot logs show `11 passed, 0 failed`.

## 11. Documents at the repo root (the "constitution")

| File | Status line inside | Implemented? |
|---|---|---|
| `os_sys_decided.md` | FINAL / LOCKED | **no** (no code) |
| `action_descriptor.schema.md` | FINAL for v1, MANDATORY | **no** |
| `policy_engine.interface.md` | (host-only, fail-closed) | **no** |
| `rollback_snapshot.contract.md` | FINAL, HOST-ONLY | **no** |
| `sandbox_result.schema.md` | FINAL, HOST-VERIFIED | **no** |
| `os_sys_creating_ai_prompt.md` | FINAL for v1 | partly (tone/roles are agent-prompt material) |
| `os_sys_open_decisions.md` | OPEN / non-authoritative | n/a |
| `os_sys_improvements.md` | FUTURE / non-binding | n/a |
| `ai_con.md` | an older handoff written for a **Rust** design-only phase | stale — describes `ai_os_core/` with `lib.rs`, which does not exist here |

## 12. Release bundle at the root

`vmlinuz-virt` (10.8 MB) · `aios-initramfs.cpio.xz` (2.4 MB) · `data.img` (8 MiB)
· `run-qemu.sh`, `run-qemu.bat`, `run-gui.sh` · `boot-test.sh`,
`boot-gui-test.sh`, `console-test.sh` · `verify-cpio.py`, `mkfs_ext2.py` ·
`boot-test1.log`, `boot-test2.log` · `BOOT-GUIDE.md` · `source/` (the tree).

Note: `boot-test.sh` and `verify-cpio.py` at the root differ from their copies
in `source/boot/`; the root copies are the release-bundle variants.

## 13. Milestones as shipped

`M1 foundation → M2 aiproc → M3 sembus → M4 agent-runtime → M5 aiosh →
M5.5 bootable-image → M6 kernel-bridge → M7 autopilot → M8 durable →
M9 v1-rc (1.0.0) → M10 desktop (1.1.0)`. All marked DONE.
Post-M10 backlog named but unscheduled: `mesh` (TCP transport for the semantic
bus), `cockpit` (mission-control dashboard).
