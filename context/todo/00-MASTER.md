# 00-MASTER — AI-OS task list (the plan)

**Big picture:** this file is the **near term** (roadmap phase W0). The full
path to desktop capability — W0 base → W1 platform → W2 shell → W3 apps + SDK →
W4 hardware → W5 users/security → W6 ecosystem, ~18–24 months — is
`context/ROADMAP-TO-WINDOWS.md`. Cards here feed that plan; nothing here is
thrown away by it.

**How to use:** this is the phase plan and the index. Detailed, delegatable
cards live in the sibling files:

| File | Audience | What is in it |
|---|---|---|
| `01-HYGIENE.md` | you / coding agent | build, git, docs, CI — nothing user-visible |
| `02-OS-CORE.md` | you (needs judgement) | the mediator: policy, descriptor, staging, rollback, audit |
| `03-GUI-TASKS.md` | **ChatGPT** (small, self-contained) | desktop app + UI tasks, one card each |
| `04-SMALL-TASKS.md` | **ChatGPT** | tiny drop-in tasks (text, checks, one-liners) |
| `05-LARGE-TASKS.md` | you | arcs that span weeks; not for delegation |
| `06-ANTI-TASKS.md` | everyone | things that look tempting and would hurt the project |
| `07-FREE-TODO-FORMAT.md` | you | free-form scratch list to write your own items |

Read `context/RULES.md` **before** any card. Every card assumes it.

---

## Phase 0 — Stop the bleeding (≈1 day)

| ID | Task | Size | Why now |
|---|---|---|---|
| `H1` | ~~Commit the generated `font8x16.h` so `make` needs no Pillow~~ **DONE** | S | was: nobody could build on a clean machine |
| `H2` | ~~Add `LICENSE` + `CONTRIBUTING.md`~~ **DONE** | S | was: the repo was unlicensed |
| `H3` | ~~CI: `make && make test` on a clean runner~~ **DONE** | M | was: the green tests were not enforced |
| `H4` | ~~Fix stale counts and dead doc paths~~ **DONE** | S | docs now match the tree |
| `H5` | ~~Boot CI job (`boot-test.sh`) on a qemu-enabled runner~~ **DONE** | M | the two-boot durable proof now runs on every push |
| `H6` | ~~Keep future binaries out of git~~ **DONE** | S | was: 54.4 MB tracked, kernel committed twice |

## Phase 1 — The mediator (M11, ≈2–3 weeks) — **the real work**

| ID | Task | Size | Card |
|---|---|---|---|
| `M1` | `aios_action` descriptor type + JSON codec | M | `02-OS-CORE.md` |
| `M2` | `policy_eval()` fail-closed law table (Tier 0/1 v1) | M | `02-OS-CORE.md` |
| `M3` | staging + diff + `promote`/`discard` (overlay or copy) | L | `02-OS-CORE.md` |
| `M4` | `tool-exec` returns an *effects* document | M | `02-OS-CORE.md` |
| `M5` | snapshot + `rollback [micro\|session]` owned by PID 1 | L | `02-OS-CORE.md` |
| `M6` | hash-chained audit + `audit verify` | M | `02-OS-CORE.md` |
| `M7` | origin tagging (`HUMAN` vs `AI`) at the TTY | M | `02-OS-CORE.md` |
| `M8` | `promote`/`reject` UX on serial console **and** desktop | M | `02-OS-CORE.md` + `03-GUI-TASKS.md` |

## Phase 2 — Make the OS honest about itself (≈1 week)

| ID | Task | Size |
|---|---|---|
| `C1` | Supervise the governor in PID 1 (respawn + report `alive`) | S |
| `C2` | Move sembus to a unix-socket broker in PID 1 (API unchanged) | L |
| `C3` | `flock` the shared store; tolerant journal reader | M |
| `C4` | Log ring buffer in PID 1 + `log`/`audit` builtins | M |
| `C5` | Make `aios.selftest` opt-in for release images | S |
| `C6` | `aiproc` becomes the real intent registry (pid + cgroup + mem ns) | L |

## Phase 3 — Linux underneath (≈3–4 weeks)

Namespaces per action · overlayfs staging · seccomp user-notify mediation ·
uid split for AI work · PSI input · erofs/squashfs + A/B updates.
Details and acceptance criteria: `context/03-LINUX-INTEGRATION.md` phases L2–L5.

## Phase 4 — Desktop grows up (delegatable in parallel, any time)

App + UI tasks: `03-GUI-TASKS.md`. Recommended order:

| Order | ID | Task | Size |
|---|---|---|---|
| 1 | `APP-01` | **notepad** — a real text editor window (the flagship small app) | M |
| 2 | `APP-02` | files window: open a file → read-only viewer | M |
| 3 | `APP-03` | launcher: room for more than 5 apps (two rows / pitch) | M |
| 4 | `UI-01` | status bar: clock, cortex, budgets, job count | S |
| 5 | `UI-02` | window resize from the bottom-right grip | M |
| 6 | `UI-03` | thread `mods` through to apps → Ctrl+S / Ctrl+Q work | S |
| 7 | `UI-04` | clipboard: copy/paste inside the notepad + terminal | M |
| 8 | `UI-05` | minimize + a taskbar of hidden windows | L |
| 9 | `UI-06` | free window state on close (stop leaking) | S |
| 10 | `APP-04` | notepad: line numbers + wrap + scrollbar | M |
| 11 | `APP-05` | notepad: search (Ctrl+F) | M |
| 12 | `UI-07` | focus ring + HUD "AI is driving" (needed before GUI automation) | M |

## Ordering rule

Phases are **sequential for one engineer**, but Phase 4 (desktop) and
`04-SMALL-TASKS.md` are **parallel-safe**: they touch `apps/aios-desktop.c`,
`src/fb.c`, `src/win.c` and docs — never the mediator. If you are running
ChatGPT on the small tasks while you do Phase 1, restrict it to those two files
sets and give it one card at a time.

## Do not do (see `06-ANTI-TASKS.md`)

Adding a 6th app before fixing the launcher · adding networking to the shell ·
skipping the sandbox in a tool · a new dependency · "just disable the test".

---

## Progress tracker (edit this section)

| Phase | Status |
|---|---|
| 0 hygiene | **`H1`-`H6` all DONE** |
| 1 mediator | **`M1` + `M2` + `M3` DONE** — next is `M4` (declared-vs-actual) |
| 2 OS honesty | not started |
| 3 Linux underneath | not started |
| 4 desktop | not started (cards ready) |

### Done

| ID | Task | Date | Where |
|---|---|---|---|
| — | build unblocked: `struct sched_param` clash (`ksched.c`, `probe.c`) | 2026-11-16 | `src/ksched.c` |
| — | 4 warnings fixed (3× sign-compare `shell.c`, 1× format-truncation `ebpf.c`) | 2026-11-16 | `src/shell.c`, `src/ebpf.c` |
| — | `.gitignore` added | 2026-11-16 | `.gitignore` |
| — | suite count corrected 10 → 11 in README/INSTALL | 2026-11-16 | `source/README.md` |
| `H1` | font table committed at `src/font8x16.h`; both Makefiles no longer run Python; `test_fb` now passes on the host (89/89) | 2026-11-17 | `src/font8x16.h`, `Makefile`, `boot/Makefile`, `tools/gen_font.py` |
| `H2` | Apache-2.0 `LICENSE` + `CONTRIBUTING.md`; `types.h` + README updated | 2026-11-17 | `LICENSE`, `CONTRIBUTING.md` |
| `H3` | CI (build -Werror, test on 22.04+24.04, boot two-boot proof). Also fixed a real test bug: test_kernel probed `/sbin/aiosh` (image-only) as the "outside" Landlock path, so it failed on every host with ENOENT; now probes `/etc/passwd` and SKIPs explicitly if absent. **Host suite is 11/11 for the first time.** | 2026-11-17 | `.github/workflows/ci.yml`, `tests/test_kernel.c` |
| `H5` | CI boot job GREEN: fetch.sh works offline, image builds, both QEMU boots verified (31 KB of serial logs uploaded). Fixed 2 fetch.sh bugs (sentinel for a module that never ships; versioned apk name vs committed name) and 4 real `-Wformat-truncation` warnings the `-Os` boot build exposed. | 2026-11-17 | `.github/workflows/ci.yml`, `boot/fetch.sh`, `boot/mkcpio.c`, `src/shell.c`, `src/agent.c`, `tests/test_shell.c` |
| `H4` | stale suite counts + dead `scripts/`, `download/` paths fixed in BOOT-GUIDE, CHANGELOG, README, UPGRADE, PERFORMANCE, ACCEPTANCE-v1.0/M10, ASSETS.md; `ai_con.md` marked HISTORICAL | 2026-11-17 | `source/*.md`, `source/docs/*.md`, `ai_con.md` |
| `H5` | CI boot job GREEN: fetch.sh works offline, image builds, both QEMU boots verified (31 KB of serial logs uploaded). Fixed 2 fetch.sh bugs (sentinel for a module that never ships; versioned apk name vs committed name) and 4 real `-Wformat-truncation` warnings the `-Os` boot build exposed. | 2026-11-17 | `.github/workflows/ci.yml`, `boot/fetch.sh`, `boot/mkcpio.c`, `src/shell.c`, `src/agent.c`, `tests/test_shell.c` |
| `H6` | duplicate kernel + factory `data.img` + stale root initramfs untracked; `tools/make-release-bundle.sh` generates the flat bundle (verified: 21 MB, 11 files); `docs/REPO-LAYOUT.md` with measured counts. Tracked: 54.4 MB -> **31.2 MB**, 179 files. | 2026-11-17 | `docs/REPO-LAYOUT.md`, `tools/make-release-bundle.sh`, `.gitignore` |
