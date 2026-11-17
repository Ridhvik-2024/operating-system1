# AI-OS v1.1 — Boot & Test Guide (the M10 desktop release)

This bundle boots AI-OS, the AI-native operating system built on
a Linux kernel base. m6 coupled the OS to the kernel; m7 gave it a
closed-loop scheduler and its own language model; m8 gave it a
memory: an ext2 side disk holds memory, job history, the audit log
and the autopilot journals with crash-consistent writes — and they
come back on the next boot. **v1.0 (M9) is the release-engineering
milestone on top: install/upgrade path, hardening review, performance
pass, and the full acceptance run recorded in docs/ACCEPTANCE-v1.0.md.
**v1.1 (M10) adds the desktop: with a display attached the boot lands
in the aios-desktop graphical shell — the proof run is
boot-gui-test.sh, recorded in docs/ACCEPTANCE-M10.md.**

## 1. Requirements

- `qemu-system-x86_64` on PATH (Linux/macOS/Windows builds all work)
- nothing else: no root, no mkfs, no compiler — the data disk ships
  pre-formatted (`data.img`), the kernel and initramfs are included

## 2. Unpack

```bash
unzip aios-m10.zip
cd aios-m10
chmod +x run-qemu.sh boot-test.sh
```

| file | what it is |
|---|---|
| `vmlinuz-virt` | Linux 6.6.142 kernel (Alpine `linux-virt`) |
| `aios-initramfs.cpio.xz` | AI-OS root filesystem: init, aiosh, governor, micro-LM, agent runtime, `aios_intent.ko`, both eBPF observers, the ext2 storage stack (`virtio_blk` + `mbcache` + `ext2`), sandbox hardening, **all 10 test suites** |
| `data.img` | **the M8 durable side disk** (8 MiB ext2) — memory/jobs/audit/autopilot live here and survive reboots; delete it for a factory reset |
| `run-qemu.sh` / `run-qemu.bat` | boot interactively (attaches `data.img` automatically) |
| `boot-test.sh` | headless CI verification — **two boots on the same disk** |
| `mkfs_ext2.py` | rebuilds `data.img` from scratch (deterministic, no host tools) |
| `boot-test1.log` / `boot-test2.log` | serial logs from the release run (boot 1 writes, boot 2 replays) |
| `source/` | complete M1-M9 source tree (incl. the micro-LM trainer, the ext2 builder, install/upgrade tooling and the docs) |

## 3. Step 1 — verify (headless, two boots)

```bash
./boot-test.sh
```

The test boots twice on the same data disk. Expected (exit 0):

```
# phase 1 — fresh disk: write durable state, power off
[init] durable: insmod virtio_blk.ko ok
[init] durable: insmod mbcache.ko ok
[init] durable: insmod ext2.ko ok
[init] durable: /dev/vda on /data (ext2) — restored 0 files, ...
  SELFTEST test_durable: PASS ...
AIOS-BOOT-OK selftest=PASS

# phase 2 — same disk: the previous session comes back
[init] durable: /dev/vda on /data (ext2) — restored 2 files,
       previous session: jobs=1 mem=9 audit=6 autopilot=1
[init] durable: this is boot #2 for this data disk
[governor] durable: 1 journal entries replayed from the previous session ...
[boot-test] PASS — AI-OS v1.1 boots and its state survives a reboot
```

## 4. Step 2 — boot for real

```bash
./run-qemu.sh
```

What you see, and what it means:

```
AI-OS 1.1.0-m10 (v1.1 desktop)
[init] durable: /dev/vda on /data (ext2) — restored N files,
       previous session: jobs=... mem=... audit=... autopilot=...
[init] durable: this is boot #N for this data disk      <- real persistence
[init] autopilot governor: pid N (mode balanced)        <- the closed loop
[init] phase 1/2: AI-OS selftest (M1-M8 unit + integration suite)
  ... 14 suites ...
AIOS-SELFTEST: PASS
[init] phase 2/2: system bring-up via aiosh (AI shell)
lm: class=bg conf=1.00 (micro-lm)                       <- the OS's own LM
...
AI-OS console ready. You are talking to aiosh (the AI shell).
```

## 5. What to test (the guided tour)

At the `aiosh>` prompt, try these in order:

**a. The durable store (new in m8)**

```
dur
durable: /data/aios (ext2 side disk, 1 boots) — this shell's store is on it
  memory:     2 entries
  audit:      6 tool calls
  jobs:       1 spawned / 2 journal records
  autopilot:  1 journal entries (last: {"tick":1,...})
remember this OS now has a disk ; recall disk
spawn summarize the workspace notes ; wait all
poweroff
```

Then boot again (`./run-qemu.sh`) and check what survived:

```
dur                      # boot count +1, memory/audit/jobs kept growing
jobs                     # the previous session's job is in the history
recall disk              # 'this OS now has a disk' — still there
```

**b. The micro-LM (the OS's own model)**

```
lm                        # model summary: vocab, classes, bigrams
lm classify run the nightly report in the background at idle priority
lm: class=bg conf=1.00 (micro-lm) text="..."
lm classify the console is frozen fix it asap
lm: class=urgent conf=0.98 (micro-lm) text="..."
lm gen about the scheduler
lm gen about memory       # deterministic OS-flavoured sentences
lm selftest
```

**c. Autopilot — the closed-loop scheduler**

```
autopilot
autopilot: mode=balanced governor-ticks=14 adjustments=0 registry=57
  live budgets: tools=50% agents=25% (cgroup cpu.max)
  last decision: steady: ... no adjustment
tune mode=aggressive      # lower thresholds, bigger steps — and it
                          #   PERSISTS now: the conf lives on the disk
autopilot                 # mode=aggressive, budgets follow
tune mode=balanced        # back to the default policy
```

**d. Natural language scheduling classes**

```
later, summarize the workspace notes
[job 2] spawned: later, ...      <- agents class (IDLE), id continues
jobs                      # it runs in the background, capped at 25% CPU
wait all
this is urgent stop everything and handle the summary now
                          # urgent wording stays INTERACTIVE (foreground)
```

**e. Kernel bridge (M6/M7) + the classics**

```
kernel                    # registry + budgets + BOTH eBPF observers
version                   # 1.1.0-m10
6*7                       # 42, via the seccomp+Landlocked tool worker
list files
```

## 6. Exiting

Type `poweroff` at the aiosh prompt, or press `Ctrl-A X` to kill QEMU.
Either way the durable state is already on the disk — every journal
write is fsync'd before the prompt returns.

## 7. What's new in the source tree (M1-M9)

| file | role |
|---|---|
| `tools/mkfs_ext2.py` | builds the 8 MiB ext2 data disk from scratch — deterministic, zero host dependencies (superblock, group descriptors, bitmaps, inode table, `/aios` pre-created) |
| `src/durable.c` | the crash-consistent store layer: atomic writes (fsync+rename+dir fsync), fsync'd journal appends, torn-tail-safe replay, mirror/restore |
| `include/aios/durable.h` | the store API — every path is relative to the durable root |
| `src/shell.c` | job spawn/exit journaled to `jobs.jsonl` (fsync'd); fresh shells replay it — job history survives reboots |
| `apps/aios-governor.c` | mirrors state + journal to the disk every tick; replays the previous journal at start |
| `src/ksched.c` | (M7) budgets, cpu.stat, /proc/aios |
| `src/shroute.c` | (M7) scheduling classes from language |
| `boot/init.c` | loads virtio_blk/mbcache/ext2, mounts /data, restores the previous session, prints the replay report, logs this boot |
| `tests/test_durable.c` | suite 10: atomicity, torn-tail replay, mirror/restore, escape rejection, job-journal replay through a real shell, live side-disk write (in-VM) |
| `tools/install.sh` + `make install` | the v1.0 install path (M9): binaries, headers and lib to `$PREFIX`, `DESTDIR`-aware, verified by a post-install shell run |
| `boot-gui-test.sh` | the M10 desktop proof: boots with a `bochs-display` GPU + virtio keyboard, screendumps the desktop through the QEMU monitor, pixel-asserts wallpaper/accent/title bars/cursor, types `6*7` in the GUI terminal and asserts the answer `42` (screendumps are written to `download/m10-boot-shots/` by the script when it runs) |
| `tools/perf-run.sh` | the v1.0 performance pass: build/suite/latency measurements behind `docs/PERFORMANCE.md` |
| `docs/` | HARDENING.md (threat model + claim->test matrix), PERFORMANCE.md, ACCEPTANCE-v1.0.md (the release record) |

## 8. FAQ

**A tiny language model — why? Is this a real LLM?**
It is a real *statistical* language model (naive-Bayes classifier +
bigram generator) trained on an OS corpus at build time — a few hundred
parameters, ~12 KB of weights, no network, fully deterministic. It is
not a transformer and does not pretend to be. The point is the SEAM:
`lm`, the intent router, and the boot script all speak to "the OS's
model" through one interface. When you want big models, `cortex llm`
plugs a real LLM into the same seam — no OS changes, and the micro-LM
keeps working offline as the fallback.

**What does the autopilot actually change?**
Only `cpu.max` of the two cgroup v2 class groups (`tools`, `agents`),
and only by borrowing: a hungry class gets budget from a quiet class,
so the total AI work stays bounded exactly as at boot. Every decision
is journalled with a human-readable reason, and since m8 the journal
lives on the disk — the governor reads where it left off last boot.

**How durable is "durable"?**
Honest-crash-consistent, not transactional: journal appends are fsync'd
before the command returns, file replacements are temp+fsync+rename
(with a directory fsync), and the replay reader drops a torn last line
instead of trusting it. A power cut mid-write can lose the record that
was being written — it can never corrupt the ones already committed.

**Why is the OS image smaller than m8, with the full v1.0 release pass?**
The v1.0 initramfs is 1,611,872 bytes — 31,060 UNDER the m8 shipped
image (1,642,932), which the boot Makefile now enforces as a hard
fail. The initramfs is xz-packed (the kernel decompresses it,
`RD_XZ=y`); the v1.0 release pass dropped the dead M5.5-era `probe`
binary from the image, absorbing the gcc-14-rebuilt kernel module.
Kernel + initramfs together: ~12.5 MB.

**Where do the milestones go from here?**
M1-M10 are all DONE. **M10 `desktop` is this release**: after the
selftest, init loads the GPU/input module stack (18 gzipped modules),
draws a splash, and supervises the graphical **aios-desktop** on
/dev/fb0 AND the serial aiosh forever. Graphics notes:

- The GPU path is `bochs-display` (plain PCI-BAR framebuffer), picked
  by boot evidence over virtio-gpu, whose scanout needs a display
  backend. QEMU args if you run manually:
  `-device bochs-display,xres=1280,yres=800 -device virtio-keyboard-pci -device virtio-tablet-pci`.
- `aios.gui=0` on the kernel cmdline skips the GPU stack (pure text
  boot); five desktop crashes fall back to text automatically.
- Every artwork asset has a procedural fallback; drop artist PNGs per
  `desktop/assets/ASSETS.md` into `desktop/assets/` and rebuild to use
  them.

`mesh` (multi-VM semantic bus) and `cockpit` (live dashboard) stay on
the backlog.
