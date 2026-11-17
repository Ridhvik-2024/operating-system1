# 01-HYGIENE — build, git, docs, CI

Nothing here is user-visible. All of it makes everyone else's work possible.
Sizes: **S** ≤ 2 h · **M** ≈ a day · **L** ≈ 2–3 days.

---

## H1 — Commit the generated font table so `make` works with no Pillow · **S**

**Problem.** `src/fb.c`, `src/win.c`, `src/input.c` include `"font8x16.h"`, which
is generated into `build/` by `tools/gen_font.py` (requires **Pillow** +
DejaVu fonts) and is **not committed**. On a clean machine `make` dies:
`ModuleNotFoundError: No module named 'PIL'` → `make: *** [Makefile:57] Error 1`.
`src/microlm_weights.h` is generated **and committed** — that is the pattern.

**Do.**
1. Generate once on a machine with Pillow: `python3 tools/gen_font.py build/font8x16.h`.
2. Move the artifact to a stable, non-build path — `src/font8x16.h` — and
   change the two includes (`src/fb.c:23`, `src/win.c:15`) to `#include "font8x16.h"`.
3. Keep the generator; add a `make font` target that regenerates it and prints
   a diff hint. Add a comment at the top of the generated file saying "generated
   by tools/gen_font.py; committed on purpose so `make` needs no Python".
4. Update both Makefiles: drop the `$(BLD)/font8x16.h` / `build/font8x16.h`
   rule and the `-I$(BLD)`/`-Ibuild` dependency for the font only.

**Acceptance.**
- `git clone && cd source && make -j4` on a machine **without** Pillow → 0 errors, 0 warnings.
- `make test` → 11/11.
- `git ls-files | grep font8x16.h` shows it tracked.
- No behaviour change: `test_fb` glyph checks still pass (they need a real table).

---

## H2 — LICENSE + CONTRIBUTING · **S**

**Problem.** No `LICENSE` anywhere. For an OS with a written constitution, this
blocks contributors and makes the "project-internal" note in `types.h` ambiguous.

**Do.**
1. Pick a licence with the owner (my recommendation: **Apache-2.0** — patent
   grant matters for a kernel-adjacent project; MIT if you want maximum
   permissiveness).
2. Add `LICENSE` at the repo root and one line in `source/README.md`.
3. Replace `License: project-internal.` in `include/aios/types.h` with the
   chosen licence name.
4. `CONTRIBUTING.md`: the rules from `context/RULES.md` §1.1/§1.2/§1.3 (stack,
   style, definition of done), plus "one card = one change".

**Acceptance.** `LICENSE` exists, is referenced from README, and `types.h`
carries the same name.

---

## H3 — CI: build + test on every push · **M**

**Problem.** The tree's own claim ("`make && make test` passes") is not enforced.
It was false on this snapshot's host (glibc clash) and nobody noticed.

**Do.**
1. `.github/workflows/ci.yml`:
   - matrix: `ubuntu-22.04`, `ubuntu-24.04` (the second catches glibc ≥ 2.36)
   - steps: `apt-get install -y gcc make` → `cd source` → `make -j4` → `make test`
   - **fail on warnings**: `make CFLAGS="... -Werror"` (keep the v1.0 flag set)
   - upload `build/last_test.log` on failure
2. Second job `boot` (separate, `allow_failure: false` once stable):
   `apt-get install -y qemu-system-x86 xz-utils` →
   `make -f boot/Makefile boot-image` → `bash boot/boot-test.sh`.
3. Cache `source/boot/dl/` (60 MB) keyed on `boot/fetch.sh`'s checksum, so the
   kernel is not re-downloaded.

**Acceptance.** A PR that adds `int x = "str";` fails CI. A clean PR is green in
< 10 min. The boot job logs `[boot-test] PASS`.

---

## H4 — Fix the stale numbers and dead paths in the docs · **S**

Already partly done in this snapshot (README/INSTALL now say 11 suites). Remaining:

| Where | Wrong | Right |
|---|---|---|
| `source/docs/ACCEPTANCE-M10.md` | cites `scripts/m10-spike.sh`, `scripts/m10-bochs-probe.sh` | those files do not exist — either commit them or rewrite the sentence as "recorded in the M10 PR" |
| `source/CHANGELOG.md` | cites `download/m10-boot-shots/` | not in the repo — say "screenshots in the release bundle" |
| `source/README.md` §"Boot it" | `boot-test.sh` "two boots on the same ext2 disk" — correct, but the sample log shows `10 suites` | update the pasted transcript to 11 |
| `source/BOOT-GUIDE.md` | same pasted-transcript drift | same |
| root `ai_con.md` | describes a Rust crate `ai_os_core/` that is not in this repo | add a header line: "historical: pre-C design phase; current memory is `context/`" |

**Acceptance.** `grep -rn "10 suites\|10 passed\|scripts/m10\|download/m10" source/` → no hits.
`ai_con.md` starts with the historical note.

---

## H5 — Boot CI (two-boot durable proof) · **M**

**Problem.** `boot-test.sh` is the strongest evidence the OS has (state survives
a reboot) and nothing runs it automatically. QEMU in CI is feasible: no root, no
KVM needed (`-machine accel=tcg`).

**Do.**
1. Reuse H3's `boot` job. Timeout 15 min (TCG is slow; the script already has
   `$TIMEOUT`).
2. Assert **both** markers it already prints: `AIOS-BOOT-OK selftest=PASS` and
   the second boot's `boot #2` + `restored N files`.
3. Upload the serial log as an artifact on failure (that log is how every past
   bug was debugged).
4. Add a `AIOS_QEMU` fallback note: locally, `boot/fetch.sh` finds a no-root QEMU.

**Acceptance.** Deliberately break durable replay (e.g. skip the mount in
`init.c`) → CI fails with the serial log attached.

---

## H6 — Keep binaries out of future commits · **S**

**Problem.** ~52 MB of tracked binaries: `linux-virt-dev.apk` (18.5 MB, a
build-time download for the LKM), `vmlinuz-virt` committed **twice** (root +
`source/boot/dl/kernel/`), GPU modules, `data.img` (8 MiB).

**Do.**
1. Do **not** rewrite history yet (that breaks clones); instead:
   - keep `source/boot/dl/` as the single copy of the kernel/modules for the
     offline build, and **document that it is intentionally tracked**;
   - delete the **duplicate** `vmlinuz-virt` at the repo root (or make it a
     documented release artifact) — one copy only;
   - decide `linux-virt-dev.apk`: it is only needed to build the LKM. Prefer
     `boot/fetch.sh` fetching it on demand and deleting it after the LKM build.
2. Add a `docs/REPO-LAYOUT.md` that says, per top-level directory, "source" vs
   "release artifact", so the next person does not commit another 10 MB kernel.
3. Optional, separate task: `git filter-repo` to shrink history (announce it
   first; everyone must re-clone).

**Acceptance.** `git ls-files -s | wc -l` and total tracked bytes in
`docs/REPO-LAYOUT.md` are accurate, and no new binary lands in a feature PR.
