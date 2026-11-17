# Repository layout: source vs. artifact

The point of this file is to stop the next person committing another 10 MB
kernel. Read it before you `git add` anything that is not text.

**Measured on 2026-11-17 (commit that added this file):**

| | value |
|---|---|
| tracked files | **179** |
| tracked bytes | **32,717,773 (31.2 MiB)** |
| of which binaries | 30,965,913 (29.5 MiB) — 95% |
| of which text | 1,751,860 (1.7 MiB) — 5% |

Reproduce with:

```bash
git ls-files | wc -l
git ls-files -z | xargs -0 du -cb | tail -1
```

## The rule

> **One copy.** Every binary in this repo exists because the build cannot
> fetch it offline. There is exactly one copy, under `source/boot/dl/`, and it
> is there on purpose.

Nothing generated is committed. If a file can be produced by `make`, by
`boot/fetch.sh`, or by `tools/make-release-bundle.sh`, it is git-ignored.

## Top-level

| path | what | tracked? |
|---|---|---|
| `source/` | **the operating system** — C11, the only thing that is source | yes |
| `context/` | engineering memory: rules, decisions, todo cards, roadmap | yes |
| `LICENSE`, `CONTRIBUTING.md`, `README.md` | project terms and entry point | yes |
| `ai_con.md` | **historical** — pre-C Rust design phase. Not buildable, kept for design history | yes (text, do not follow it) |
| `os_sys_decided.md` | the constitution: architectural law that outranks preference | yes |
| `vmlinuz-virt`, `data.img`, `aios-initramfs.cpio.xz` | **removed in H6** — generated bundle pieces, now git-ignored | **no** |

## `source/` — the source tree

| path | what | tracked? |
|---|---|---|
| `src/`, `apps/`, `include/` | the OS: libraries, programs, public API | yes |
| `tests/` | 11 suites, each runs on the host *and* inside the booted OS | yes |
| `boot/` | init, LKM, image builder, QEMU harnesses | yes |
| `src/microlm_weights.h`, `src/font8x16.h` | generated **but committed on purpose**: a clean clone must build with no Python | yes (text) |
| `tools/` | host-side helpers (mkfs, font generator, release bundle) | yes |
| `docs/` | acceptance records, performance measurements | yes |
| `boot/build/` | image build tree | **no** (regenerated) |
| `source/dist/` | release bundle output | **no** (regenerated) |
| `build/`, `*.o`, `*.a`, binaries | compiler output | **no** |

## `source/boot/dl/` — the only committed binaries

| file | size | why it is committed |
|---|---|---|
| `kernel/boot/vmlinuz-virt` | 10.4 MB | the stock Alpine `linux-virt` 6.6.142 kernel; the OS is useless without it and Alpine's CDN is a single point of failure |
| `linux-virt-dev.apk` | 17.7 MB | kernel headers, needed to build the LKM (`aios_intent.ko`). `fetch.sh` extracts it; the extract (`dl/kern-dev/`) is git-ignored |
| `busybox-root/bin/busybox.static` | 0.98 MB | static busybox for the initramfs |
| `modules/*.ko` (3) | 0.36 MB | ext2, mbcache, virtio_blk — storage |
| `modules-gpu/*.ko.gz` (18) | 0.80 MB | DRM/bochs + evdev input stack |

`boot/fetch.sh` verifies this set and downloads **only** what is genuinely
missing. It was audited for this: two bugs had it re-downloading 38 MB on
every run even with everything committed (a sentinel naming a module that
never ships, and a versioned apk filename that did not match the committed
one). Both are fixed, and CI asserts `fetch.sh` leaves `boot/dl` untouched.

## Generated, never committed

```bash
bash source/boot/fetch.sh                          # -> source/boot/dl/kern-dev/
make -f source/boot/Makefile boot-image            # -> source/boot/build/aios-initramfs.cpio.xz
bash source/tools/make-release-bundle.sh           # -> source/dist/aios-bundle/ (21 MB, 11 files)
bash source/tools/make-release-bundle.sh --zip     # -> source/dist/aios-v1.1.zip
```

The flat release bundle (`kernel + initramfs + data.img + scripts`, the layout
`INSTALL.md` documents) is **generated**. It used to be half-committed: the
kernel was in the repo twice, byte-identical (md5 `97521668…`), and a factory
`data.img` was tracked. That was 19.2 MB of duplication for something the
script above rebuilds in under a second.

## Adding a binary: the checklist

1. Can the build fetch or generate it? → generate it. If it must be fetched,
   make `fetch.sh` handle it and trust its verification.
2. Is it needed for an **offline** build? Only then commit it, once, under
   `source/boot/dl/`.
3. Is it over ~5 MB? Say why in this file and in the commit message.
4. Update the two numbers at the top of this file if the total moves by more
   than a megabyte.

## History

The old copies are still in git history (this file does not rewrite it).
Shrinking history with `git filter-repo` is a separate, announced task — it
forces every clone to re-clone, so it needs a decision, not a drive-by commit.
