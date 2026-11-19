# 03 — LINUX INTEGRATION ("use bits and parts of Linux")

Your direction: *"we will eventually add Linux to this, use bits and parts of
Linux in this AI operating system."* This file says **which parts**, **in what
order**, and **how we know it worked** — with the rule that every adopted
Linux feature must replace something we would otherwise invent, and must not
weaken an invariant in `os_sys_decided.md`.

## 1. The line we already hold

```
+---------------------------------------------+
| aiosh · aios-desktop · agentd-style runtime |  ours  (C11, zero deps)
+---------------------------------------------+
| sembus · memory · durable store · autopilot |  ours
+---------------------------------------------+
| intent classes · cgroups · landlock/seccomp |  OURS on LINUX PRIMITIVES
+---------------------------------------------+
| Linux kernel 6.6.142 (Alpine linux-virt)    |  adopted as-is
+---------------------------------------------+
```
Today's answer to "use Linux parts" is: **kernel + cgroups v2 + sched_setattr +
eBPF + kprobe + Landlock + seccomp + ext2 + initramfs**. That is already a
respectable list. The missing half is Linux's **isolation, layering and
mediation** machinery, which is exactly what the constitution needs.

## 2. Feature-by-feature: adopt this, not that

| We need | Do NOT build | Adopt from Linux | Why it fits |
|---|---|---|---|
| Real sandbox isolation | our own chroot+jail | **user+mount+pid+net namespaces**, `pivot_root`, `CLONE_NEWUSER` | kernel-enforced, unprivileged, composable with cgroups |
| Reversible filesystem | copy trees in C | **overlayfs** (`lowerdir`/`upperdir`/`workdir`) | staging + diff + commit is the overlay's native behaviour |
| Whole-system rollback | bespoke snapshot format | **dm-snapshot**, **btrfs subvol + snapshot**, `fsfreeze` for consistency | block-level, fast, already audited |
| "AI asks, OS decides" on syscalls | patches to tool-exec | **seccomp user notification** (`SECCOMP_RET_USER_NOTIF`) | a supervisor decides each `open`/`connect` — this *is* the policy engine's enforcement point |
| Immutable policy that AI cannot touch | compiled-in C table only | **BPF LSM** / **Landlock** | attached by the host, not modifiable from the AI trust domain |
| Semantic IPC across processes | a TCP stack | **unix `SOCK_SEQPACKET` + `SCM_RIGHTS`**, **`memfd_create`**, **`eventfd`** | zero-config, credential-bearing (`SO_PEERCRED` = real authority) |
| Host ↔ VM channel (`mesh`) | port-forward hacks | **`AF_VSOCK`** (+ **virtio-fs** for sharing) | purpose-built for guest↔host, no NIC needed |
| Scheduler pressure signal | more kprobes | **PSI** (`/proc/pressure/{cpu,io,memory}`) | kernel's own stall metrics; less code than our eBPF deltas |
| Device/GPU access under control | raw `/dev/mem` | **VFIO** (or `virtio-gpu` + `virgl`) with time-bound grants | revocable, IOMMU-isolated |
| Binary/update trust | custom hashing | **fs-verity** / **IMA**, **module signing**, measured boot (`tpm2`) | "auditable by hash" is literally a kernel feature |
| Immutable base OS + A/B updates | rsync into initramfs | **erofs/squashfs** read-only root + overlay, two slots + bootloader switch | matches `os_sys_open_decisions.md` §1.2 option list |
| Per-workload resource caps | our own accounting | **cgroup v2** (already), **`memory.max`**, **`pids.max`**, **`io.max`** | we only use `cpu.max` today |
| Time-bound capability grants | bespoke token file | **file capabilities** + `setresuid` in a namespace, or a small LSM policy | revocable by killing the namespace |

## 3. Phased plan (each phase ships something testable)

### Phase L0 — Make the box build and stay green *(days)*
- [x] Fix `struct sched_param` redefinition (`src/ksched.c`, `boot/probe.c`) — done in this snapshot.
- [ ] Commit `build/font8x16.h` (like `microlm_weights.h`) so `make` needs no Pillow.
- [ ] `make test` → 11/11 on a clean Debian/Ubuntu host; add `.github/workflows/ci.yml`
      running `make && make test` + `boot-test.sh` (needs qemu, no root).
- [ ] `.gitignore` (done), `LICENSE` (pick one), move `source/boot/dl/*` and
      duplicate kernels out of git into releases.
- **Done when:** a fresh clone builds and tests green with no network and no
  Python packages.

### Phase L1 — The mediator: descriptor → policy → staging → diff → promote *(1–2 weeks)*
This is the phase that makes the constitution true. Pure userspace first; Linux
parts arrive in L2.
- [ ] `include/aios/action.h`: `aios_action{kind, target, declared_effects[],
      budget, reversibility, intent_hash, origin(HUMAN|AI)}` + JSON codec.
- [ ] `src/policy.c`: compiled-in law table, `policy_eval() → {ALLOW, DENY,
      NEEDS_HUMAN}`; fail-closed; unit tests for every rule.
- [ ] `src/stage.c`: stage a write into `staging/<intent>/`, compute the diff,
      `promote()` (atomic rename) or `discard()`; deletes → `staging/.trash/`.
- [ ] `src/snapshot.c`: snapshot `{store, staging, workspace}` before an agent
      run; `rollback micro|session` restores it. Owned by PID 1.
- [ ] `src/audit.c`: hash-chained JSONL (`prev`) + `audit verify` builtin.
- [ ] `tool-exec` returns an **effects** document; the host compares it with the
      descriptor and blocks on mismatch.
- **Done when:** `fs.write` from an agent produces a *pending* change that a
  human types `promote 3` to accept, and `rollback` undoes it; a test proves an
  undeclared write is blocked.

### Phase L2 — Put Linux's isolation under the mediator *(2–4 weeks)*
- [ ] `tool-exec` (or a new `aios-sandboxd`) creates a **user + mount + pid netns**
      per action, `pivot_root`s into an **overlay** whose `upperdir` is
      `staging/<intent>`. Staging and rollback now come from the kernel.
- [ ] **seccomp user-notify** supervisor for `open`, `connect`, `execve`,
      `mount` — the mediator's decision point; verdicts come from `policy.c`.
- [ ] AI work runs under its own uid; the PID-1 request socket checks
      `SO_PEERCRED` so authority is a kernel credential, not a flag.
- [ ] Migrate sembus to a unix `SOCK_SEQPACKET` broker in PID 1;
      `sembus_pub/sub` API unchanged; desktop, governor and jobs finally share events.
- [ ] `aiosh` is started with the human's credentials only; anything reaching it
      from a model is tagged `origin=AI` and can never self-promote.
- **Done when:** an agent that tries `fs.write` outside `workspace/` is denied by
  a kernel-enforced path rule, the attempt is in the hash-chained audit, and the
  sandbox is provably empty after the run (`nsenter` shows nothing persisted).

### Phase L3 — Real persistence, removal of the RAM ceiling *(2–3 weeks)*
- [ ] Two-partition image: **read-only erofs/squashfs root** + **ext4 data
      partition** with `overlayfs` upper for `/usr` mutations.
- [ ] Grow `/data` beyond 8 MiB; add rotation/caps for `audit.log`,
      `jobs.jsonl`, `memory.journal`; `flock` the consolidation path.
- [ ] **A/B slots** + a `aios-update` verb: write the new image to the inactive
      slot, `fs-verity` it, switch on next boot, keep the old slot for instant
      rollback. AI cannot initiate an update (constitution §Open 12.2).
- [ ] `fs-verity` on `/sbin/*` and the LKM; verify at boot.
- **Done when:** `reboot` after an update lands in the new slot; deleting the
  slot reverts; `audit.log` survives 100k lines with rotation.

### Phase L4 — Network, on purpose *(decide first, then 2–3 weeks)*
Two mutually exclusive options — **pick one, write it down**:
- **(A) Air-gapped appliance.** No NIC. Local model behind the `cortex llm`
  seam (GGUF/llama.cpp as a *host* process reached over `AF_VSOCK`, or a
  quantised model in the image). Keeps `-nic none`, keeps the seccomp story as-is.
- **(B) Mediated network.** One `netd` in PID 1 owns the only socket; `net.*`
  tools submit requests; per-domain allowlist; every request visible in the
  desktop status bar; seccomp still kills direct `socket()` in the sandbox.
- Either way: `z-ai` CLI dependency must die or be replaced by a real client
  that speaks to `netd`/vsock.
- **Done when:** `lm`-style routing still works with no network; a model-backed
  route works without the sandbox ever holding a socket.

### Phase L5 — Hardware breadth *(ongoing)*
- [ ] **aarch64**: rebuild modules for Pi-class kernels (device tree, no
      `bochs-display`; target `simple-framebuffer`/`vc4`), serial `ttyAMA0`.
      The Raspberry Pi in `ai_con.md` is the real-world goal.
- [ ] `PSI` as an autopilot input (replaces some eBPF plumbing).
- [ ] Governor **supervised by PID 1** (respawn + `autopilot: alive` reporting).
- [ ] `aiproc` becomes the real intent registry (pid + cgroup + memory ns),
      so "a process is an intent" is an OS fact, not a library.

## 4. What NOT to adopt

- **systemd / udev / dbus** — they would become a second control plane and
  contradict "the host control plane is not addressable by the AI".
  Our PID 1 is 548 lines and does exactly what we need.
- **A general-purpose distro userland** — it multiplies the attack surface the
  seccomp allowlist currently closes down to ~54 syscalls.
- **Kernel patches** — stay on stock `linux-virt`; an out-of-tree LKM is
  already the maximum (and it taints the kernel — see boot logs).
- **Docker/podman** as the sandbox — it brings a daemon, a registry client and
  a network path we do not want; namespaces + overlay directly are smaller
  and auditable.

## 5. Invariants each phase must preserve

1. Zero runtime dependencies beyond libc/pthreads/libm (keep it that way; Linux
   features are syscalls, not libraries).
2. The initramfs size cap holds (or the raise is documented in
   `boot/.m10-size-cap` with a reason).
3. Warning-free build under the v1.0 hardening flags.
4. `boot-test.sh` two-boot durable proof stays green.
5. No new authority for the AI, ever. If a phase gives the AI more *capability*,
   it must ship with more *containment* in the same commit.
