# 05-LARGE-TASKS — arcs that span weeks (not for delegation)

These change the shape of the system. Each needs a written spec **before** code,
its own branch, and its own acceptance run. Do not hand these to ChatGPT; an
agent can do the mechanical parts, but the decisions are yours.

---

## L1 — Cross-process semantic bus (sembus over unix sockets) · **L**

**Why.** `src/sembus.c` is an in-process object (`struct sembus { sub *head; }`),
`calloc`'d per process (`shell.c:1554`). The desktop, the governor, tool workers
and spawned jobs **cannot see each other's events**. The "semantic IPC" claim is
false across processes. `emit boot.up` goes nowhere.

**Shape.**
1. `src/sembusd.c` — a broker in PID 1 on `<store>/bus.sock`
   (`SOCK_SEQPACKET`), one topic registry, fan-out on delivery.
2. Keep the API **identical** (`sembus_create/sub/pub/pub_kv`); the transport
   changes underneath. Topics stay strings; payloads stay JSON (bounded: 4 KiB).
3. Wildcards (`sh.*`) are matched in the broker, not the client.
4. `SO_PEERCRED` on accept → every event gains an `origin` (this is also why
   `M7`/origin tagging is a prerequisite, not an afterthought).
5. Backpressure: a slow subscriber is dropped after 64 queued messages (never
   block the publisher).

**Acceptance.** Two processes: A subscribes to `sh.cmd`, B (the shell) runs a
line → A receives it. A killed subscriber does not stop the publisher. A 4 KiB
payload is delivered whole; 8 KiB is rejected. Existing 15 `test_sembus` checks
still pass unchanged (API compatibility is the point).

**Risks.** PID 1 becomes a service (keep it < 200 lines), and boot ordering:
the broker must be up before any subscriber.

---

## L2 — Namespaces + overlayfs per action · **L**

**Why.** G4/G6: today every process is root in one initramfs; a tool writes
reality directly. The kernel already has the machinery.

**Shape.**
1. A supervisor (`aios-sandboxd` or a function in PID 1) that, per action:
   `clone(CLONE_NEWUSER|NEWMNT|NEWPID|NEWNET)` → map uid 0 in-ns to the `aios`
   uid outside → mount an overlay with the jail as `lowerdir` and
   `<store>/staging/<intent>/upper` as `upperdir` → `pivot_root` → `execve`
   the worker.
2. The worker keeps its rlimits, seccomp allowlist and Landlock (defence in
   depth: the namespace is containment, seccomp is capability).
3. Promotion = rename out of `upper` (this is `M3` arriving via the kernel).
4. Rollback of a whole session = throw away `upper` (this is `M5` arriving via
   the kernel).

**Acceptance.** Inside the action: `id` shows the mapped uid; `/workspace` is
writable but writes land in `upper`; the host jail is untouched; the action
cannot see other processes (`ps` shows only its own); after the action the
namespace is gone. A test proves an undeclared write is invisible until promoted.

**Blockers.** Requires `M1/M2/M3` to exist first (a descriptor to mediate, a
policy to consult, a staging place). Do not start before the mediator.

---

## L3 — seccomp user-notify as the mediation point · **L**

**Why.** `src/hardening.c` allowlists syscalls, but an allowed syscall is
unconditional. The constitution needs *per-call* decisions ("this `open` of this
path by this action").

**Shape.** A supervisor installs `SECCOMP_RET_USER_NOTIF` for `openat`,
`connect`, `execve`, `mount`; each notification carries the pid, path
(`/proc/<pid>/mem` read) and the action's descriptor; the verdict comes from
`policy_eval()`. Default deny on supervisor timeout.

**Acceptance.** A tool opening a path outside the jail is denied *and* the denial
appears in the audit chain with the action id. A tool opening a declared path
proceeds. The supervisor dying fails closed (the tool is killed, not allowed).

---

## L4 — Persistent root: erofs/squashfs + overlay + A/B updates · **L**

**Why.** The root is a RAM initramfs; only an 8 MiB ext2 disk persists. You
cannot install anything, cannot survive an update, and the store will fill.

**Shape.** Read-only `erofs` root + `ext4` data partition; `overlayfs` for
`/usr` and `/etc`; two root slots + a bootloader switch; `fs-verity` on the
binaries; `aios-update` writes the inactive slot and switches on next boot.

**Acceptance.** Update → reboot → new version in the new slot; rollback = switch
back; `fs-verity` verification fails loudly if a binary is modified; the store
survives both.

---

## L5 — aarch64 / Raspberry Pi · **L**

**Why.** `ai_con.md` names a Pi as the eventual hardware; every `.ko` shipped is
x86_64 and the GPU path is `bochs-display`.

**Shape.** A second boot target: kernel + modules for the Pi, `ttyAMA0` console,
`simple-framebuffer`/`vc4` instead of bochs, device-tree-derived screen size.
Keep the userspace identical (it is already portable C11 — that is the reward
for zero dependencies).

**Acceptance.** `make -f boot/Makefile boot-image ARCH=aarch64` produces an
image that boots on a Pi (or QEMU `-M virt`) and reaches `aiosh` + the desktop
with a fallback font/asset path.

---

## L6 — `aiproc` becomes the real process model · **L**

**Why.** `include/aios/aiproc.h` describes "a process is an intent"; nothing
outside `tests/test_aiproc.c` calls it. Work is `fork()` + threads.

**Shape.** An intent registry in PID 1 (`/run/aios/intents.json`, or a
`/proc/aios-intents` view) where every unit of work — a shell line, an agent
step, a tool call, a job — has an id, an intent, a class, a cgroup, a memory
namespace and a lifecycle (INTENT→READY→RUNNING→BLOCKED→DONE/FAILED/KILLED).
`aiproc` becomes the client library of that registry.

**Acceptance.** `jobs` and a new `intents` builtin show the same truth; killing
an intent kills its whole process group; a crashed intent is visible as
`FAILED` with a reason, not as a disappeared thread.

---

## L7 — Networking, decided on purpose (Phase L4 of the integration plan) · **L**

Two mutually exclusive options; pick one and write it into
`os_sys_decided.md` §9 (Brain Modes):

- **(A) air-gapped appliance** — no NIC; a local model behind the `cortex` seam
  reached over `AF_VSOCK` from the host. Keeps `-nic none` and the seccomp story.
- **(B) mediated network** — one `netd` in PID 1 owns the only socket; `net.*`
  tools submit requests; per-domain allowlist; every request visible on the
  desktop. The sandbox never holds a socket.

**Acceptance for (A):** the OS answers a model-backed question with no NIC and
no `z-ai` binary. **For (B):** a request to a non-allowlisted domain is denied
and audited; `socket()` inside the sandbox still dies by seccomp.
