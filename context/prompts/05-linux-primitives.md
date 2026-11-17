# Name: LINUX-ADOPT
**Use when:** you are deciding which part of Linux replaces something we would
otherwise build ourselves.

---

AI-OS keeps the Linux kernel and rebuilds everything above it. I want to adopt
**parts of Linux** instead of reinventing them, without weakening the
constitution. Read `context/CONTEXT.md`, `context/03-LINUX-INTEGRATION.md`,
`os_sys_decided.md`, then the code that would be affected:
`source/src/tool-exec.c`, `source/src/hardening.c`, `source/src/sandbox.c`,
`source/src/durable.c`, `source/src/sembus.c`, `source/boot/init.c`,
`source/src/ksched.c`.

For **each** facility below, produce a row: *what invariant it makes true*,
*what we delete*, *what new failure mode it introduces*, *how it is tested*,
*effort (days)*, *risk (low/med/high)*, and a **verdict** (adopt now / adopt
later / reject, with reason):

| facility | candidate Linux mechanism |
|---|---|
| per-action isolation | user+mount+pid namespaces, `pivot_root` |
| staging + commit | overlayfs (`lowerdir`/`upperdir`) |
| whole-system rollback | dm-snapshot, btrfs subvolumes |
| syscall mediation (AI asks, OS decides) | `SECCOMP_RET_USER_NOTIF` |
| un-bypassable policy | Landlock, BPF LSM |
| cross-process semantic bus | unix `SOCK_SEQPACKET` + `SCM_RIGHTS`, `memfd` |
| host↔VM channel | `AF_VSOCK`, virtio-fs |
| scheduler signal | PSI (`/proc/pressure/*`) |
| device grants | VFIO / IOMMU, virtio-gpu |
| binary + update trust | fs-verity, IMA, module signing, measured boot |
| immutable root + updates | erofs/squashfs + overlay, A/B slots |
| resource caps | cgroup v2 `memory.max`, `pids.max`, `io.max` |
| platform breadth | aarch64 build target (Raspberry Pi) |

Then answer:

1. **Ordering** — a dependency-aware sequence, given that the mediator
   (descriptor → policy → staging → promote/rollback → audit) comes first.
2. **What we must never adopt** and why (I currently exclude systemd/dbus/udev,
   a distro userland, kernel patches, and container runtimes as the sandbox).
   Tell me if any exclusion is wrong.
3. **The smallest experiment** that de-risks the biggest unknown, runnable in
   QEMU in under an hour, with the exact command and the observation that
   decides it. (Precedent: M10 chose bochs-display over virtio-gpu by booting,
   not by reading docs.)
4. **What breaks** in the boot CI (`boot-test.sh`, the two-boot durable proof,
   the size cap) for each adoption, and how to keep it green.

Rules: prefer syscalls over libraries (we have zero runtime deps), prefer
kernel-enforced over our-enforced, and never trade an invariant for a feature.
