# 04 — BUILD & VERIFY (what actually ran, 2026-11-16)

Everything here was executed in this checkout. Commands are copy-pasteable.
Where a claim in the repo's docs could not be reproduced, it is called out.

## 1. Environment

| Tool | Version / presence |
|---|---|
| gcc | 12.2.0 (Debian 12) |
| glibc headers | 2.36-era (defines `struct sched_param` in `<sched.h>`) |
| make, python3 | present |
| Python `Pillow` | **absent** (and cannot be installed: PEP 668 managed env) |
| `qemu-system-x86_64` | **absent** — the boot tests cannot be re-run in this sandbox |
| DejaVu fonts | present at `/usr/share/fonts/truetype/dejavu/` |

Because QEMU is unavailable here, the **in-OS** guarantees are quoted from the
checked-in serial logs (`boot-test1.log`, `boot-test2.log`, 472/480 lines,
which show `AIOS-SELFTEST: PASS`, `11 passed, 0 failed` (the log predates
M1), `AIOS-BOOT-OK
selftest=PASS`, `boot #2`, GPU stack `18/18`) and were **not** independently
re-executed. Everything in §2–§5 below *was* re-executed.

## 2. Build failure found (and fixed)

`make` on this host fails immediately:

```
src/ksched.c:28:8: error: redefinition of 'struct sched_param'
```
cause: `src/ksched.c` includes **both** `<linux/sched/types.h>` (which defines
`struct sched_param` and `struct sched_attr`) and glibc's `<sched.h>` (which, on
glibc ≥ 2.36, also defines `struct sched_param`). Same defect in
`source/boot/probe.c` (a dev-only probe, but it cannot compile either).

**Fix applied:** both files now `#include <sched.h>` and declare
`struct sched_attr` themselves (the kernel ABI struct they actually need for
`sched_setattr(2)`). No behaviour change.

Verified:
```
gcc -std=c11 -O2 -Wall -Wextra -Wshadow -D_GNU_SOURCE -Iinclude -c -o /tmp/k.o src/ksched.c
# exit 0
gcc -std=c11 -O2 -Wall -Wextra -Iinclude -c -o /tmp/p.o boot/probe.c
# exit 0
```
`probe.c` also gained `#define _GNU_SOURCE` (it called `syscall(2)` without the
declaration) — that warning is now gone too.

## 3. Full-tree compile check (fixed tree)

```
for f in src/*.c apps/*.c: gcc -std=c11 -O2 -Wall -Wextra -Wshadow \
  -Wformat -Wformat-security -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
  -D_GNU_SOURCE -Iinclude -Ibuild -c -o obj $f
```
→ **26/26 translation units compile, 0 failures, 0 warnings** (with a stub
`build/font8x16.h`; see §4). This required fixing four warnings that the v1.0
"warning-free" claim missed on this toolchain:

| file:line | warning | fix |
|---|---|---|
| `src/shell.c:883,902` | `-Wsign-compare` (`int i` vs `size_t a->n`) | loop counter is `size_t` |
| `src/shell.c:1004` | `-Wsign-compare` (same) | `size_t i` |
| `src/ebpf.c:154` | `-Wformat-truncation` (16383-byte `%s` into 768) | `" [%.700s]"` bounds the copy |

`boot/init.c` and `boot/probe.c` compile **clean**. `boot/mkcpio.c` keeps one
pre-existing `-Wformat-truncation` warning at line 108 (path join); harmless,
left alone — fix it with the same `%.*s` pattern when you next touch the packer.

## 4. Second build blocker: the font is generated, not shipped — **FIXED 2026-11-17**

> **Status:** fixed. The table now lives at `src/font8x16.h` and is committed;
> both Makefiles no longer run Python (proved with a `python3` stub that exits
> 99). `test_fb` passes **89/89** on the host with the real table. The section
> below is the original finding, kept for the record.

`make` needs `build/font8x16.h` before `fb.c`, `win.c`, `input.c`.
It is produced by `tools/gen_font.py`, which needs **Pillow**:

```
ModuleNotFoundError: No module named 'PIL'   →  make: *** [Makefile:57: build/font8x16.h] Error 1
```

So on a clean machine `make` (and therefore `make test`, `make -f
boot/Makefile boot-image`) cannot start. `microlm_weights.h` is handled the
right way — generated **and committed**. The font table should be too
(one `font8x16.h` ≈ tens of KB, deterministic from DejaVu).
For the checks in §5 a **stub** table was used, which is why `test_fb` fails
there (`glyph has ink`) — that failure is an artifact of the stub, not a code bug.

## 5. Test suite, actually run on this host

All 11 suites of that audit were built against the fixed tree and executed
(11 then; **12** since M1 added `test_action`):

| Suite | Result here | Notes |
|---|---|---|
| `test_agent` | **PASS** 15 checks | after `tool-exec` was built (the agent shells out to it) |
| `test_aiproc` | **PASS** 9 | |
| `test_autopilot` | **PASS** 61 | |
| `test_durable` | **PASS** 35 | |
| `test_fb` | FAIL 2/89 | `glyph has ink` — **stub font**, expected |
| `test_json` | **PASS** 20 | |
| `test_kernel` | FAIL 1/5 (4 skipped) | `landlock: jail readable, /etc/passwd blocked` — Landlock not permitted in this sandbox; eBPF `BPF_MAP_CREATE: EPERM` (reported as SKIP) |
| `test_mem` | **PASS** 25 | |
| `test_sandbox` | **PASS** 26 | timeout/kill path works |
| `test_sembus` | **PASS** 15 | |
| `test_shell` | **PASS** 100 | |

**=> 9/11 pass on a hostile sandbox; the 2 failures are environmental**
(no real font table, no Landlock/BPF permission). In-tree boot logs show
11/11 with 0 failures in the guest, where Landlock LSM exists.

First run without `tool-exec` in PATH-and-position gave `test_agent`,
`test_sandbox` and `test_shell` failures — that is expected behaviour and a
**documentation trap**: the sandbox resolves `tool-exec` via
`/proc/self/exe`'s directory or `$AIOS_TOOL_EXEC`, so tests must run from the
build dir (or with `AIOS_TOOL_EXEC` set).

## 6. Live shell check (the product actually works)

```
$ ./aiosh --jail ws -c '6*7'                                  → 42
$ ./aiosh --jail ws -c 'fs.list . | count'                    → 3
$ ./aiosh --micro --jail ws -c 'lm classify run the report in the background later'
  lm: class=bg conf=1.00 (micro-lm)                           → micro-LM works offline
$ ./aiosh --jail ws -c 'remember AI-OS review done'           → remembered
$ ./aiosh --jail ws -c 'recall review'                        → [0.60] episodic AI-OS review done
$ ./aiosh --jail ws -c 'list files'                           → metrics.csv notes.txt summary.md
```
Persistent memory across *separate process invocations* works (the store is
on disk), and natural-language routing works.

## 7. Claims in the repo's docs that do NOT check out

| Claim | Reality |
|---|---|
| README/INSTALL: `make test` → `== 10 passed, 0 failed ==` | **FIXED** — there are **12** suites now; docs say 12 |
| `make` works "in seconds" on any machine | fails on glibc ≥ 2.36 (fixed here) and needs Pillow for the font |
| `docs/ACCEPTANCE-M10.md` cites `scripts/m10-spike.sh`, `scripts/m10-bochs-probe.sh` | **no `scripts/` directory exists** |
| CHANGELOG cites `download/m10-boot-shots/` | **no `download/` directory exists** |
| `desktop/assets/` has artist artwork | only `ASSETS.md`; all assets are procedural fallbacks |
| "the OS's own language model … swappable for big LLMs" | the only big-LLM path is `execlp("z-ai", …)`, absent from the image, and the image has no NIC |
| `aiproc`: "a process is an intent" | `aiproc` has **no caller** outside its own test; work is `fork()` + threads |
| `sembus`: "semantic IPC between processes" | in-process object only (`calloc` per process) |
| `os_sys_*.md` "FINAL / LOCKED" | zero implementation (no policy engine, descriptor, diff, snapshot, rollback) |
| `ai_con.md` describes `ai_os_core/` Rust crate | that crate is not in this repository (older design phase) |

None of these are dishonest about *capability* — the code does what the
milestones say — but the docs over-claim in small ways that a reviewer will
find in ten minutes. Fix the strings as you touch the files.

## 8. How to re-run the full proof (on a machine with QEMU)

```bash
# 1. clean build (after committing font8x16.h, or with Pillow available)
cd source && make && make test          # expect 14/14

# 2. bootable image
bash boot/fetch.sh                      # kernel + busybox + modules (~60 MB, once)
make -f boot/Makefile boot-image        # size guard runs here

# 3. the real proof
make -f boot/Makefile boot-test         # two boots, asserts reboot replay
bash boot/console-test.sh               # drives aiosh over serial, asserts 42
bash boot/boot-gui-test.sh              # boots the desktop, asserts pixels + 6*7→42
```

`boot/.m10-size-cap` is generated on first build and is now `.gitignore`d —
if you need the historical cap, it is quoted in `CHANGELOG.md`
(2 605 888 bytes) and `docs/ACCEPTANCE-M10.md`.
