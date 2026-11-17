# AI-OS v1.0 — Final Acceptance Record (M9 `v1-rc`)

Date: 2026-11-16. Machine: Linux 5.10 x86_64 sandbox, 2 logical cores,
QEMU 10.0.13 (TCG, `-m 512M -smp 2`) for boot proofs. Every gate below
was executed against the final v1.0 tree at commit
`M9 v1-rc ...` (see git log) with `AIOS_VERSION "1.0.0"`.

## Gate 1 — host build & full test battery

| check | result |
|---|---|
| `make clean && make -j2` | OK — **warning-free** under `-Wall -Wextra -Wshadow -Wformat -Wformat-security -fstack-protector-strong -D_FORTIFY_SOURCE=2` |
| `make test` | **10/10 suites PASS** (10 suites at v1.0; M10 added `test_fb`, so today it is 11/11) |

Per-suite (host run):

| suite | checks | notes |
|---|---|---|
| test_json | 20, 0 failed | |
| test_aiproc | 9, 0 failed | |
| test_sembus | 15, 0 failed | |
| test_mem | 25, 0 failed | |
| test_sandbox | 26, 0 failed | includes wedged-worker SIGKILL proof |
| test_agent | 15, 0 failed | |
| test_shell | 100, 0 failed | |
| test_kernel | 4, 0 failed, **5 env-skipped** | cgroup/eBPF/Landlock cases skip on the host; run fully in-image |
| test_autopilot | 61, 0 failed, 3 env-skipped | |
| test_durable | 35, 0 failed, 1 env-skipped | |

Total: **310 checks, 0 failed** on the host (the 9 env-skips run inside
the booted OS, where the full kernel facilities exist).

## Gate 2 — install / upgrade path

| check | result |
|---|---|
| `make install DESTDIR=/tmp/aios-stage PREFIX=/usr` | OK — 5 binaries, 15 headers, `libaios.a` staged |
| run the INSTALLED aiosh (`/tmp/aios-stage/usr/bin/aiosh -c '6*7'`) | OK — `42`; the sandbox located the installed `tool-exec` via `/proc/self/exe` |
| `tools/install.sh` logic (build -> test -> install -> verify) | covered by the same steps |

## Gate 3 — bootable image + size ratchet

| check | result |
|---|---|
| `make -f boot/Makefile boot-image` | OK — `boot/build/aios-initramfs.cpio.xz` |
| **v1.0 initramfs size** | **1,611,872 bytes** |
| size guard (cap = the m8 shipped image, 1,642,932 bytes) | **PASS — 31,060 bytes under cap** |
| note | the guard held despite a gcc-14-rebuilt `aios_intent.ko` (+~600 B over m8): the M5.5-era `sbin/probe` (dead weight, referenced by nothing at runtime) was removed from the stage; source stays in `boot/probe.c` |

## Gate 4 — two-boot durable CI proof (`boot/boot-test.sh`)

Phase 1 (fresh disk): boot, selftest, bring-up writes durable state,
poweroff. Phase 2 (same disk): the previous session must come back.

| assertion | result |
|---|---|
| banner `AI-OS 1.0.0 (v1.0)` | OK |
| `durable: /dev/vda on /data (ext2)` | OK |
| `AIOS-DUR mounted=1` | OK |
| `AIOS-SELFTEST: PASS` (both boots) | OK |
| `SELFTEST test_durable: PASS` | OK |
| `AIOS-BOOT-OK selftest=PASS` (both boots) | OK |
| `autopilot governor: pid` | OK |
| boot 2: `AIOS-DUR mounted=1 jobs=[1-9] mem=[1-9] audit=[1-9]` | OK — the previous session's state came back |
| boot 2: `this is boot #2 for this data disk` | OK |
| boot 2: governor journal replayed | OK |
| **verdict** | **[boot-test] PASS — AI-OS v1.0 boots and its state survives a reboot** |

## Gate 5 — live console test (`boot/console-test.sh`)

Driven over the serial console, interactive QEMU:

| assertion | result |
|---|---|
| `AI-OS console ready` / `aiosh>` prompt | OK |
| banner reports `1.0.0 (v1.0)` | OK |
| `6*7` -> `42` (sandboxed tool worker, seccomp+Landlock active in-image) | OK |
| `dur` reports the durable store | OK |
| `lm classify ...` -> `class=urgent` (micro-LM live) | OK |
| `kernel` -> `kernel bridge:` (registry, budgets, BOTH eBPF observers) | OK |
| `autopilot` -> `mode=` (governor live) | OK |
| **verdict** | **[console-test] PASS** |

## Gate 6 — boot timeline (evidence: `boot/build/boot-test1.log`)

Kernel timestamps under TCG emulation: `0.000000` kernel start ->
`8.640748` `reboot: Power down`. A full CI boot — kernel, initramfs
unpack, mounts, **10-suite selftest**, micro-LM bring-up, job spawn,
governor start, CI poweroff — completes in **8.6 s** emulated. (Host
sum of the same suites: 3.35 s; see `docs/PERFORMANCE.md`.)

## Release verdict

**ACCEPTED — v1.0.** All six gates green. The release artifacts
(`aios-v1.0.zip`, `aios-v1.0.tar.gz`, `aios-v1.0-boot/`) are packaged
from exactly this tree, with the boot-test serial logs from this run
shipped alongside as evidence.
