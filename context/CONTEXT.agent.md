# CONTEXT.agent.md — coding-agent memory (AI-OS)

**Read this file first; then `CONTEXT.md`. Then `04-BUILD-VERIFY.md` before you
run anything.** You are working in `Ridhvik-2024/operating-system1`.

## 0. Orientation in 20 seconds

- The **real code** is in `source/`. The repo root is a **release bundle** +
  design documents. Do not edit root copies of scripts when the source-tree
  version exists.
- It is **C11, zero runtime dependencies** (libc + pthreads + libm). Keep it that way.
- Two build systems: `source/Makefile` (host runtime) and
  `source/boot/Makefile` (bootable image, wraps the first).
- Kernel stays Linux. Nothing above it is anything but our code.

## 1. Commands that work (and their traps)

```bash
cd source
make -j4                      # TRAP 1: needs build/font8x16.h (Pillow + DejaVu)
make test                     # TRAP 2: needs tool-exec next to the test binaries
make -f boot/Makefile boot-image   # TRAP 3: needs boot/fetch.sh downloads first
bash boot/boot-test.sh        # needs qemu-system-x86_64 (absent in most sandboxes)
```

**Trap 1 — font.** `fb.c`, `win.c`, `input.c` include `"font8x16.h"` from
`build/`. `tools/gen_font.py` needs Pillow. If Pillow is missing, either
`pip install --break-system-packages Pillow`, or write a stub header to
compile-check (it must define `AIOS_FONT_FIRST`, `AIOS_FONT_LAST`,
`AIOS_FONT8X16[95][16]`, `AIOS_FONT8X16B[95][16]`). `test_fb` will fail with
`glyph has ink` under a stub — that is expected.

**Trap 2 — tool-exec discovery.** `src/sandbox.c:resolve_worker()` looks at
`$AIOS_TOOL_EXEC`, then a `tool-exec` **next to `/proc/self/exe`**, then
`./tool-exec`. If tests fail with sandbox errors, you are running from the
wrong directory. Run tests from `source/` with `./tool-exec` built, or export
`AIOS_TOOL_EXEC=$PWD/tool-exec`.

**Trap 3 — fetched kernel/modules.** `source/boot/dl/` is gitignored. Without
it, the LKM and the boot image cannot build. `bash boot/fetch.sh` restores it.

**Trap 4 — glibc ≥ 2.36.** Never include both `<linux/sched/types.h>` and
`<sched.h>`. `struct sched_param` is defined in both since glibc 2.36. Use the
`struct sched_attr` declaration that now lives in `src/ksched.c`.

## 2. File map (what to open when)

| Task | Open |
|---|---|
| shell grammar, builtins, pipelines, jobs | `source/src/shell.c` (1 682 l), `source/include/aios/shell.h` |
| NL routing (tool / agent / reply) | `source/src/shroute.c`, `src/shroute.h` |
| agent loop, observation JSON | `source/src/agent.c`, `include/aios/agent.h` |
| planners (scripted / llm) | `source/src/cortex.c`, `src/llmbridge.h` |
| memory tiers + recall + consolidation | `source/src/mem.c`, `include/aios/mem.h` |
| tools + audit log | `source/src/tools.c` |
| sandbox parent side | `source/src/sandbox.c` |
| sandbox child (the worker) | `source/src/tool-exec.c` |
| Landlock + seccomp | `source/src/hardening.c` |
| cgroups + sched classes + /dev/aios | `source/src/ksched.c`, `boot/module/aios_intent.c` |
| eBPF observers | `source/src/ebpf.c` |
| autopilot policy (pure) | `source/src/autopilot.c`, `apps/aios-governor.c` |
| micro-LM | `source/src/microlm.c`, `src/microlm_weights.h`, `tools/microlm_gen.py` |
| durable store primitives | `source/src/durable.c`, `include/aios/durable.h` |
| PID 1 | `source/boot/init.c` |
| framebuffer / windows / input | `source/src/fb.c`, `win.c`, `input.c` |
| the GUI shell | `source/apps/aios-desktop.c` |
| tests | `source/tests/test_*.c`, harness `tests/t.h` |

## 3. Patterns the codebase enforces (copy them, don't fight them)

1. **`aios_status` return codes** (`AIOS_OK`, `AIOS_ERR_*`) + an out-param
   `char *err, size_t ecap` filled with `snprintf`. Every public function does this.
2. **No `malloc` without a NULL check; no `strcpy`; `snprintf` always.**
   The tree builds warning-free with `-Wall -Wextra -Wshadow -Wformat
   -Wformat-security`; keep it that way.
3. **Internal headers** (`sandbox.h`, `shroute.h`, `llmbridge.h`, `fb_priv.h`)
   are private; public API lives in `include/aios/*.h` and must be documented
   with the same comment style (what it is, why, and the invariant).
4. **Everything kernel-facing is best-effort and never fatal** (`ksched.c`,
   `ebpf.c`, `hardening.c`): on failure, report and degrade; never abort the OS.
5. **Persistence goes through `durable.c`** — atomic write (temp+fsync+rename)
   or append+fsync. Never write the store with a bare `fopen("w")`.
6. **State ownership:** binaries are stateless; the OS owns state under
   `$AIOS_STORE` (host) / `/data/aios` (image).

## 4. How to add things (recipes)

**A builtin** (`source/src/shell.c`):
1. write `static int builtin_x(sh_shell *sh, sargv *a, char **text_out, char *err, size_t ecap)`
2. add `if (strcmp(c0, "x") == 0) return builtin_x(...)` in the dispatcher (~`shell.c:1445`)
3. add a line to the `help` text (~`shell.c:594`)
4. set `*text_out = strdup(...)`; return 0/1; never print to stdout directly.

**A tool** (`source/src/tools.c`): add a `tool_def` in `tool_reg_install_default()`
with `.sandboxed = true` (runs in `tool-exec`) or `.native = fn` (in-process).
Sandboxed tools must validate paths (`..`, absolute, `O_NOFOLLOW`).
Then extend the positional-arg shorthand in `shell.c:450` if it deserves one.

**A durable journal** — use `aios_dur_append(path, line)` / `aios_dur_atomic_write`,
and teach `dur` in `shell.c` to count lines if it should be visible.

**A test** — `tests/test_<name>.c` following `tests/t.h` (`CHECK`, `SKIP`,
`PASS: n checks, m failed` on stdout; exit 0 only when green). It is picked up
automatically by both Makefiles (glob) and will run **inside the booted OS**.

**A desktop window** — `apps/aios-desktop.c`: extend `app_kind`, `APP_META`,
`open_app()`, `refresh_app()`, and the draw/input handlers; use `aios-fb` and
`aios-wm` APIs, never touch `/dev/fb0` directly.

## 5. Invariants — do not break these, even if asked nicely

1. Zero external runtime deps. No new library. Syscalls or our own code.
2. Warning-free build; size cap for the initramfs (`boot/Makefile` enforces it;
   raising it requires a documented reason).
3. Every new capability must come with containment in the same change
   (`os_sys_improvements.md` §13: *capability may scale, authority may not*).
4. Do not let AI-originated input bypass anything. If you add a path where a
   model can cause an effect, it goes through the sandbox + audit.
5. Do not weaken the sandbox to make a test pass. Fix the test or SKIP it with
   a printed reason (`SKIP(...)`).
6. Do not commit: `*.o`, `*.d`, `build/`, `boot/dl/`, stores, boot logs from a
   local run. `.gitignore` now covers them — keep it correct.

## 6. Current state / next task

- Version `1.1.0-m10`. All M1–M10 milestones are DONE and the boot proof is in
  the logs. **The next milestone is the mediator** (D5 in
  `context/05-DECISIONS.md`): action descriptor → policy → staging → diff →
  promote/rollback → hash-chained audit.
- If you are asked to "add a feature", check first whether it is one of the
  S1 gaps in `context/02-WHAT-IS-MISSING.md` — those come first.
- Before you finish: run `make` and `make test`, and update
  `context/04-BUILD-VERIFY.md` with the new numbers and the exact command.

## 7. Known doc/code drift (fix when you touch the file)

`README.md`/`INSTALL.md` say "10 suites" and `== 10 passed`; there are 11.
`docs/ACCEPTANCE-M10.md` cites `scripts/*.sh` that do not exist. CHANGELOG
cites `download/m10-boot-shots/` that does not exist. `ai_con.md` describes a
Rust crate that is not in this repo.
