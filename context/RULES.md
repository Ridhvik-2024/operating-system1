# RULES.md — build rules for AI-OS (READ BEFORE WRITING ANY CODE)

This file is the **contract** for anyone (human or AI) who changes this
repository. It has three parts:

1. **Universal rules** — how code is written here.
2. **System-specific rules** — how *this OS* works, and the traps that will
   break it (each one was found by reading the code, not guessed).
3. **Task protocol** — how to pick up a task, prove it is done, and hand it back.

If a task in `context/todo/` contradicts this file, **this file wins**.
If this file contradicts `os_sys_decided.md` (the constitution), **the
constitution wins** and you should stop and report the conflict.

---

## Part 1 — Universal rules

### 1.1 The stack (non-negotiable)

| Thing | Rule |
|---|---|
| Language | **C11**. No C++, no Rust, no generated bindings. |
| Dependencies | **Zero at runtime**, beyond libc, pthreads and libm. No new library, ever. |
| Build | `make` (host) and `make -f boot/Makefile` (image). Both must stay green. |
| Compilers | Must build on gcc ≥ 9 and clang ≥ 12 without warnings. |
| Size | The boot image has a **hard size cap** enforced by `boot/Makefile`. Do not raise it without a written reason. |

If you think you need a library, you need a syscall or 40 lines of C instead.

### 1.2 Code style (copy the existing code, do not invent a new one)

1. **Return codes**: public functions return `aios_status` (`aios/types.h`:
   `AIOS_OK`, `AIOS_ERR_ARG`, `AIOS_ERR_IO`, …) or `int` 0/1 for shell-level
   success/failure. Never return raw `-1` without a documented meaning.
2. **Errors out-of-band**: the last parameters are `char *err, size_t ecap`,
   filled with `snprintf(err, ecap, "...")`. Never print errors from library
   code; never call `exit()` from a library.
3. **Output via out-params**: `char **text_out`, `json_val **json_out`. A
   library function never writes to stdout. (`apps/` may print; `src/` may not.)
4. **Memory ownership is documented in the header comment**: "owned by
   caller", "borrowed", "freed by `x_free`". Match it exactly.
5. **Every `malloc`/`strdup` is NULL-checked.** Nothing calls `strcpy`,
   `strcat`, `sprintf`, `gets`. Use `snprintf`, `memcpy` with sizes, `strlcpy`-style
   helpers already present in the file.
6. **Bounded buffers**: never a VLA of unbounded size, never a stack buffer
   over ~1 KB on the desktop (it runs on a 512 MB VM).
7. **`static` for everything internal.** Public API lives only in
   `include/aios/*.h`; internal helpers stay in `src/` and are declared in a
   private header (`src/*.h`, `sandbox.h`, `shroute.h`).
8. **Comments say why, not what.** Every non-obvious constant gets one line.
   Keep the existing tone: lowercase, factual, no marketing.
9. **Warning-free or it does not land.** Build with the v1.0 flag set:
   `-Wall -Wextra -Wshadow -Wformat -Wformat-security
   -fstack-protector-strong -D_FORTIFY_SOURCE=2`.

### 1.3 Testing (a task without a test is not done)

1. New logic that is not UI → **a suite** in `tests/test_<name>.c` using the
   harness in `tests/t.h` (`CHECK`, `SKIP`, and a final
   `PASS: n checks, m failed`). It is picked up automatically by both
   Makefiles (glob) and **runs inside the booted OS** — that is the point.
2. UI/render logic → test through the **in-memory framebuffer seam**:
   `aios_fb_open_mem(w, h)` + `aios_fb_px(f, x, y)` (see `test_fb.c`'s `wm()`
   test). No QEMU needed on the host.
3. If a check needs a kernel facility (Landlock, eBPF, cgroups), it must
   `SKIP("reason")` **visibly** — never silently pass, never disable.
4. Never weaken or delete an existing check to make your change pass. If an
   existing check is wrong, say so and fix the check with a reason.

### 1.4 Definition of done

A task is done when **all** of these are true:

- [ ] `cd source && make -j4` → 0 errors, **0 warnings**
- [ ] `cd source && make test` → `== 11 passed, 0 failed ==` (or more suites)
- [ ] the new/updated test exists and fails without your change
- [ ] `git status` shows only intended files (no `*.o`, `*.d`, `build/`, stores)
- [ ] no new runtime dependency; no new `malloc` without a check
- [ ] behaviour is documented in the header comment of the function you added
- [ ] `context/` updated (see §3.4)
- [ ] if it touches boot/desktop: `bash boot/boot-gui-test.sh` still passes
      (or you say clearly that you could not run it and why)

---

## Part 2 — System-specific rules (AI-OS)

### 2.1 The architecture you must not break

```
kernel  = stock Linux (Alpine linux-virt 6.6.142). Never patch it.
above   = ours: init, aiosh, agent runtime, memory, scheduler policy,
          durable store, framebuffer desktop.
```

- **One visible computer.** No second desktop, no shadow UI, no mirroring.
- **The sandbox is disposable.** Tools run in `tool-exec`, a separate process.
  Nothing in `tool-exec` may persist state, learn, or outlive its request.
- **Authority is structural.** Human > host (PID 1) > AI work. Code must never
  let an AI-originated path do something a human-originated path is not
  allowed to do, and must never let AI work configure its own limits.
- **The OS owns state, binaries are stateless.** Anything persistent goes to
  `$AIOS_STORE` (host) / `/data/aios` (image) through `src/durable.c`.
  Never invent a new store location; never write the store with `fopen("w")`.
- **Capability may scale; authority may not.** A change that gives the AI more
  power must ship containment (audit, bound, revert) in the same change.

### 2.2 Build traps (these have all bitten this tree)

| # | Trap | What happens | What to do |
|---|---|---|---|
| T1 | `build/font8x16.h` is **generated** by `tools/gen_font.py` (needs **Pillow**) and is **not committed** | `make` dies at `build/font8x16.h` on a clean machine | install Pillow, or write a stub header for compile checks (must define `AIOS_FONT_FIRST`, `AIOS_FONT_LAST`, `AIOS_FONT8X16[95][16]`, `AIOS_FONT8X16B[95][16]`). Do **not** commit a stub. |
| T2 | `tool-exec` is found via `$AIOS_TOOL_EXEC`, then next to `/proc/self/exe`, then `./tool-exec` | sandbox/agent/shell suites fail with confusing errors | run tests from `source/` where `tool-exec` was built, or export `AIOS_TOOL_EXEC=$PWD/tool-exec` |
| T3 | glibc ≥ 2.36 defines `struct sched_param` in `<sched.h>`, and so does `<linux/sched/types.h>` | `error: redefinition of 'struct sched_param'` | never include both. `src/ksched.c` declares `struct sched_attr` itself — reuse that pattern |
| T4 | `source/boot/dl/` (kernel, busybox, modules) is large and only restored by `bash boot/fetch.sh` | the image/LKM cannot build | run `fetch.sh` once; never commit new blobs |
| T5 | suite count is **11**, docs may say 10 | you chase a phantom failure | the truth is `ls source/tests/test_*.c \| wc -l` |

### 2.3 Shell / tool rules

- A new user-facing capability is normally **a tool or a builtin**, not a new
  binary. Tool → `src/tools.c` (`tool_reg_install_default`). Builtin →
  `src/shell.c` dispatcher + the `help` text.
- Tools that touch the filesystem are `sandboxed = true` and live in
  `src/tool-exec.c`. They must resolve every path **inside the jail** and
  reject absolute paths, `..`, and symlinked final components (`O_NOFOLLOW`).
- Size caps are real: `fs.read` ≤ 256 KiB, `fs.write` ≤ 1 MiB (`FS_READ_MAX`,
  `FS_WRITE_MAX` in `tool-exec.c`). Do not raise them inside a feature task.
- Builtins set `*text_out = strdup(...)` and return 0/1 — they never print.
- Anything that runs a command from the GUI must go through
  `sh_exec(d->sh, "...", &r)` — **never** a private code path. Same engine,
  same sandbox, same future mediator.

### 2.4 Desktop / GUI rules (M10)

Coordinates and drawing:

- Windows draw in **canvas coordinates** (0,0 = below the title bar), not
  screen coordinates. Screen coordinates are only for `aios_wm_*` calls.
- Title bar height = `AIOS_TITLE_H` (24 px). The canvas is
  `aios_win_cw()` × `aios_win_ch()`.
- Text is a fixed **8×16 cell grid**: width = `8 * strlen(s)` px, so a line of
  N characters needs `8*N` px. `aios_fb_text_w(s)` gives the width.
- Colours are `0xAARRGGBB`; use `AIOS_RGB(r,g,b)` for opaque colours. Transparent
  bg = `0` in `aios_fb_text` / `aios_win_text`.
- **Never touch `/dev/fb0` directly** — everything goes through `aios-fb`.

Rendering discipline (it is a single-threaded damage-driven loop):

- **No allocation per frame.** Allocate the window's state struct once in
  `open_app()` (`calloc`), store it with `aios_wm_open(..., user)` and retrieve
  it with `aios_win_user(w)`.
- **No blocking calls** on the UI path (no `sleep`, no network, no agent run).
  Long work goes to `sh_exec` (which may fork a job) or a background job.
- Call `aios_wm_damage_all(d->wm)` only when something changed; the compositor
  is cheap when nothing is damaged.
- Mark the window dirty with `aios_win_dirty(w)` (or damage_all) **after**
  drawing; the flush happens once per loop iteration in `main`.
- Never read more than you need from disk per frame. Load into the state
  struct on open / on explicit action, then render from RAM.

Input:

- Key codes arrive as ASCII (32..126) or `AIOS_KEY_*` (`UP`, `DOWN`, `LEFT`,
  `RIGHT`, `HOME`, `END`, `PGUP`, `PGDN`, `F(n)`). Backspace arrives as
  `0x08` or `0x7f`.
- **Modifier keys are dropped**: the event carries `mods` (`1 shift | 2 ctrl |
  4 alt`) but `main` passes only `ev.key` to `key_to_app()` / `nl_key()`. So
  there is **no Ctrl+S today**. Adding shortcuts means threading `mods`
  through first — that is its own task (`UI-03`), not a side effect of a feature.
- Every window needs a **keyboard** path, not only mouse clicks.

Layout:

- Screen target is **1280×800** (`boot-gui-test.sh` asserts it); the layout code
  also assumes that resolution (e.g. `w - 440`, fixed y=510).
- **The launcher holds exactly 5 apps.** Slots are drawn at
  `w - 440 + i*86`, 80 px wide: `i=4` ends at `w-16`, `i=5` starts at `w-10`
  and falls off-screen. **Adding a 6th app requires changing the launcher
  layout first** (wrap to two rows, smaller pitch, or a scroll strip).
- `APP_COUNT` sizes `g_icons[]` and several loops — adding an app kind touches
  `app_kind`, `APP_META`, `open_app()`, `refresh_app()`, `key_to_app()` and the
  launcher.

Assets:

- `desktop/assets/*.png` are converted at build time to `.aiosimg`. **No PNGs
  are committed**, so every asset must have a **procedural fallback** in
  `src/fb.c` (`aios_img_*_fallback`). A new asset needs a fallback or the OS
  loses its face on a clean build.

Close / lifecycle:

- Closing is handled in `main`'s event loop: it sets `apps[i].win = NULL` and
  calls `aios_wm_close`. If your window owns heap state, it **leaks today** —
  freeing it is a task (`UI-06`), do not assume it exists.
- If the desktop crashes 5 times, PID 1 gives up on graphics and falls back to
  the text console. Never `abort()`, never `exit()` on a drawable error.

GUI test compatibility (this bites hard):

- `boot/boot-gui-test.sh` + `tools/gui_check.py` assert: a vertical gradient, the
  accent bar, **five title bars at hardcoded positions**
  `(40,40) (780,40) (40,510) (440,510) (840,510)`, a cursor sprite near the
  centre, then type `6*7` → the terminal region changes and `has_42` passes.
- Therefore: **moving or resizing existing windows, or changing the palette's
  brightness, breaks CI** unless you update `gui_check.py` in the same change.
  Add a new check (with a `PASS`/`assert` line) when you add visible features.

### 2.5 Kernel-facing code

- Everything in `src/ksched.c`, `src/ebpf.c`, `src/hardening.c` is
  **best-effort and never fatal**. On failure: record the reason, degrade,
  continue. Never abort the OS because a kprobe did not attach.
- Never require root for the host build; never require a kernel patch.
- The LKM (`boot/module/aios_intent.c`) builds out-of-tree against fetched
  headers; keep it compiling and keep `/proc/aios` output format stable —
  `aios-governor` parses it.

### 2.6 What you may NOT do (hard stops)

1. No new runtime dependency, no vendored library, no dynamic loading.
2. No disabling, shortening or bypassing the sandbox to make something work.
3. No writing outside the jail from a tool; no direct `open()` for file edits
   from the desktop (use the `fs.*` tools via `sh_exec`).
4. No new authority for AI code: it may not edit policy, the audit log, its own
   limits, or PID 1's decisions.
5. No editing the constitution (`os_sys_decided.md`, `action_descriptor.schema.md`,
   `policy_engine.interface.md`, `rollback_snapshot.contract.md`,
   `sandbox_result.schema.md`).
6. No `printf` debug output left in `src/`; no commented-out code committed.
7. No "temporary" hacks that skip a test. No tests deleted.
8. No binary blobs, no `build/`, no stores in commits.

---

## Part 3 — Task protocol

### 3.1 Before you write code (5 minutes, do not skip)

1. Read `context/RULES.md` (this file), then the task card, then the **files
   the card names** — not the whole repo.
2. Answer in one line each:
   - Which function/file will I change?
   - What is the smallest diff that satisfies the acceptance test?
   - What existing behaviour could this break (which suite, which GUI check)?
   - Am I adding a capability without containment? (if yes: stop)
3. If the task is ambiguous, choose the interpretation that **touches fewer
   files** and state the assumption at the top of your answer.

### 3.2 While writing

- One concern per change. Do not refactor while implementing a feature —
  put the refactor in the todo list instead.
- Keep functions under ~80 lines; if a window handler grows past that, split
  render / key / action like the existing ones (`files_render`, `files_key`).
- Prefer editing an existing helper over adding a parallel one.

### 3.3 Prove it

Run and paste the output of:

```bash
cd source
make -j4                                 # 0 errors, 0 warnings
make test                                # == 11 passed, 0 failed ==
```

If you touched the desktop or boot:

```bash
make -f boot/Makefile boot-image         # size guard must pass
bash boot/boot-gui-test.sh              # needs qemu-system-x86_64
```

If QEMU is not available, say so explicitly and rely on the mem-fb test seam.

### 3.4 Hand back (do not skip — this is how the project remembers)

1. Update `context/todo/00-MASTER.md`: move your task to **Done** (with date).
2. If you changed a number (suites, checks, sizes), update
   `context/04-BUILD-VERIFY.md` with the command and the new number.
3. If you made a real decision, append it to `context/05-DECISIONS.md`
   (date · decision · why · cost · what would reverse it).
4. If you discovered a new trap, add a row to §2.2 here.

### 3.5 Report format (use this, it makes review fast)

```
TASK:   UI-04 (files: open a file → read-only viewer)
FILES:  apps/aios-desktop.c (files_buf, files_key, files_render, open_app)
CHANGE: added APP_VIEWER ... (3 sentences max)
PROOF:  make 0/0 · make test 11/11 · new checks: test_fb +6 (viewer geometry)
RISK:   launcher has 5 slots — used the existing APP_FILES window, no new slot
LEFT:   syntax highlighting (deferred, see APP-07)
```
