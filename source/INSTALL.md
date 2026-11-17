# AI-OS v1.0 — Installation Guide

Two ways to run AI-OS: as a **host runtime** (the system services layer
on your Linux box — shell, agents, memory, sandbox) or as a **bootable
OS** (our initramfs on a Linux kernel, in QEMU or a VM).

---

## 1. Host runtime (from source)

### Requirements

- `cc` (gcc or clang) and `make` — that's it for the runtime itself
- `python3` — only needed to regenerate the micro-LM weights
  (`tools/microlm_gen.py`) or rebuild the ext2 data disk
  (`tools/mkfs_ext2.py`); both are checked into the repo as generated
  artifacts, so a plain build does not need Python
- No root, no external libraries — everything is C11 + pthreads

### Quick install

```bash
./tools/install.sh                       # build + 10-suite test + install
sudo ./tools/install.sh                  # same, to /usr/local
PREFIX=$HOME/.local ./tools/install.sh   # user-local, no root
```

### Manual install

```bash
make -j          # libaios.a, tool-exec, aiosh, aios-demo, aios-memctl,
                 # aios-governor
make test        # must print: == 11 passed, 0 failed ==
make install     # PREFIX=/usr/local by default; DESTDIR works for staging
```

What lands where (`PREFIX` defaults to `/usr/local`):

| path | contents |
|---|---|
| `$PREFIX/bin/aiosh` | the AI shell (interactive, `aiosh file.aiosh`, `aiosh -c 'line'`) |
| `$PREFIX/bin/aios-demo` | the M4 agent demo (`--scripted` for offline) |
| `$PREFIX/bin/aios-memctl` | inspect the OS-owned memory store |
| `$PREFIX/bin/aios-governor` | the autopilot daemon (host-side) |
| `$PREFIX/bin/tool-exec` | the sandboxed tool worker — kept **next to aiosh**; the sandbox resolves it via `/proc/self/exe`, no config needed |
| `$PREFIX/include/aios/` | public headers (json, aiproc, sembus, mem, tools, agent, shell, ksched, ebpf, hardening, microlm, autopilot, durable) |
| `$PREFIX/lib/libaios.a` | the runtime library |

```bash
make uninstall   # removes exactly the files above
```

### Environment (all optional)

| variable | meaning | default |
|---|---|---|
| `AIOS_STORE` | where the OS-owned memory/audit/jobs live | `./.aios-store` |
| `AIOS_SHELL_CORTEX` | `scripted` \| `micro` \| `llm` | `scripted` |
| `AIOS_TOOL_EXEC` | explicit path to the sandbox worker | auto (next to aiosh, then `./tool-exec`) |
| `AIOS_SANDBOX_HARD` | set to `1` to install the seccomp allowlist + Landlock jail in the worker | unset (practical jail only) |
| `AIOS_DUR_ROOT` | durable-store root for the governor | `/var/lib/aios` |

### First run

```bash
aiosh
aiosh> version
aiosh> 6*7
aiosh> remember AI-OS v1.0 is installed
aiosh> recall installed
aiosh> spawn summarize the workspace ; jobs ; wait all
```

Run the demo twice in a row — the second run recalls the first run's
facts. That is the OS owning memory, not the app.

---

## 2. Bootable OS image (the real OS)

### Requirements

- `qemu-system-x86_64` (any recent build; Windows/macOS/Linux all work)
- nothing else: the release bundle ships kernel, initramfs and a
  pre-formatted `data.img` — no root, no compiler, no mkfs

### Release bundle (recommended)

```bash
unzip aios-v1.0.zip && cd aios-v1.0
./boot-test.sh     # headless two-boot verification (exit 0 = good)
./run-qemu.sh      # boot the OS, aiosh is your console
```

Details, the guided tour and the full console transcript:
[BOOT-GUIDE.md](BOOT-GUIDE.md).

### Kernel cmdline knobs

| flag | meaning |
|---|---|
| `aios.mode=test` | CI mode: selftest -> `AIOS-BOOT-OK` -> poweroff |
| `aios.selftest=0` | skip the in-OS selftest phase |
| `aios.autopilot=off\|static\|balanced\|aggressive` | governor policy at boot (default `balanced`) |
| `aios.durable=0` | skip the ext2 data disk — RAM-only session (m7 behavior) |

### Building the image from source

```bash
bash boot/fetch.sh                    # kernel + busybox + modules (~60 MB, once)
make -f boot/Makefile boot-image      # -> boot/build/aios-initramfs.cpio.xz
make -f boot/Makefile boot-test       # headless two-boot CI proof
make -f boot/Makefile boot-run        # interactive serial console
```

No root needed at any step (custom cpio packer emits device nodes;
QEMU runs unprivileged). The LKM (`aios_intent.ko`) is built against
the downloaded Alpine kernel headers the first time.

---

## 3. Virtual / real hardware

The image is a standard x86_64 bzImage + initramfs. Any VM that boots
a Linux kernel can boot it (the serial console is the primary
interface: `console=ttyS0`). Bare metal works the same way as long as
a serial console is available; the durable side disk needs a virtio or
IDE disk holding an ext2 filesystem (see `tools/mkfs_ext2.py`).
