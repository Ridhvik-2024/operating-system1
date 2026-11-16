# AI-OS v1.0 — Hardening Review

Status: **reviewed for v1.0** (M9). This document is the honest
statement of what is enforced, by which layer, and which test proves
it. It also states what AI-OS does NOT claim — an OS that hides its
scope is not hardened, it is marketing.

## 1. Threat model (explicit)

- **Single-tenant OS.** One user (you) runs the AI shell, agents and
  background jobs. There are no remote users; the booted image has no
  network stack attached (`-nic none` in every harness).
- **The untrusted surface is AI work**: tool arguments coming from a
  language model or from natural-language lines, and the files in the
  tool jail. The sandbox (section 2) exists precisely for this.
- **Trusted base**: PID 1 (`aios-init`), the runtime library
  (`libaios.a`), the governor, the LKM, and the kernel. These run with
  full privilege and are not protected from each other beyond normal
  Unix permissions.

## 2. The tool sandbox — layer map

Every `sandboxed: true` tool (`calc`, `fs.read`, `fs.write`,
`fs.list`) executes in a separate `tool-exec` worker process. Layers,
in the order they are applied:

| # | layer | detail |
|---|---|---|
| 1 | process isolation | fork/exec of `tool-exec`; request/response via pipes; the model never touches the parent's memory |
| 2 | rlimits | `RLIMIT_CPU` 15 s, `RLIMIT_AS` 256 MB, `RLIMIT_NOFILE` 64, `RLIMIT_NPROC` 64, `RLIMIT_FSIZE` 8 MB (`src/tool-exec.c: apply_limits()`) |
| 3 | wall-clock timeout | enforced TWICE: child `alarm()` + parent-side deadline with SIGTERM then SIGKILL (`src/sandbox.c`) — a wedged worker cannot hang the agent loop |
| 4 | path jail | worker cwd = jail dir; `..`/absolute escapes rejected before exec; jail path resolved to absolute before chdir |
| 5 | `PR_SET_NO_NEW_PRIVS` | set before any LSM/seccomp work, so privilege escalation via exec is impossible |
| 6 | Landlock (v1.0 hard env) | ruleset limiting the filesystem view to the jail (ABI v3; v2-era headers tolerated) — applied FIRST |
| 7 | seccomp cBPF (v1.0 hard env) | allowlist of **54 syscalls** (fd IO, mmap family, signals, time, pid/uid info, mkdir/rename/unlink family); `execve`, `fork`, `socket`, `mount`, `ptrace`, `bpf` and everything else -> `SECCOMP_RET_KILL_PROCESS`. Applied AFTER Landlock so the setup cannot lock itself out (`src/hardening.c`) |

Activation: `AIOS_SANDBOX_HARD=1` (set by init inside the booted OS;
opt-in on the host). Layers 6/7 are skipped cleanly with a documented
SKIP in `test_kernel` on hosts lacking Landlock/seccomp support.

## 3. Compiler-level hardening (new in v1.0)

The host build now carries:

```
-std=c11 -O2 -Wall -Wextra -Wshadow -Wformat -Wformat-security
-fstack-protector-strong -D_FORTIFY_SOURCE=2
```

and the **entire tree compiles warning-free** under that set (fixed in
this release: guarded `_GNU_SOURCE` defines, `warn_unused_result`
handled in test harnesses, provably-bounded path joins in the durable
tests).

Boot image tradeoff (documented, deliberate): the initramfs build
keeps `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections`
without stack-protector, to honour the hard size cap (v1.0 cap =
the m8 shipped image, 1,642,932 bytes). In the image the kernel-level
boundaries are Landlock + seccomp + cgroups; the binaries are static,
and the OS has no external input surface beyond the serial console and
the ext2 side disk it owns.

## 4. Source review findings (M9 pass)

- Raw `strcpy`: exactly two, both provably safe — a constant `"?"`
  into `struct utsname.release` (65 bytes) and `"/"` into a buffer
  guarded by `if (!*dir)`. Everything else is `snprintf` with explicit
  bounds.
- No `system()` / `popen()` anywhere in the runtime or apps (the only
  `system()` calls are test-harness fixture setup).
- The LLM bridge (`src/cortex.c`) spawns the model CLI with an
  argument array (no shell interpolation), isolates child stdio, and
  parses a strict JSON action contract with one corrective retry; a
  model returning garbage cannot reach the filesystem except through
  the sandboxed tool registry.
- Durable layer: every store path is validated relative to the
  durable root — `aios_dur_write_atomic("../escape")` is rejected
  (unit-tested, `test_durable`).
- Audit log is append-only and covers every tool call with who/when/
  args/ok/duration/result-digest — tampering with history requires
  write access to the store itself.

## 5. Claim -> enforcement -> test

| claim | enforced by | proven by |
|---|---|---|
| tools cannot escape the jail | path jail + Landlock | `test_sandbox` (26 checks), `test_kernel` Landlock case |
| tools cannot fork/exec/network in hard mode | seccomp allowlist | `test_kernel` seccomp kill/allow cases |
| a wedged tool cannot hang the OS | double timeout kill | `test_sandbox` wedged-worker SIGKILL case |
| memory cannot be exhausted by one tool | `RLIMIT_AS` 256 MB | rlimit round-trips in `test_kernel` |
| store paths cannot escape the durable root | path validation in `durable.c` | `test_durable` escape rejection |
| a torn journal line cannot poison the store | replay drops torn tail | `test_durable` torn-tail case |
| background AI work stays under budget | cgroup v2 cpu.max + governor lend-or-hold | `test_kernel` cgroup cases, `test_autopilot` policy cases |
| every tool call is auditable | append-only audit JSONL | audit integrity in `test_sandbox`/`test_shell` |

## 6. Honest scope (what is NOT claimed)

- Not multi-user; no user separation beyond the jail model above.
- No ASLR/CFI claims beyond what stock gcc + kernel defaults provide.
- The `micro` cortex is deterministic and offline; the `llm` cortex
  sends prompt text to the configured model CLI — model output is
  treated as untrusted input, the sandbox is the boundary, but a
  malicious model with `fs.write` can write inside the jail by
  design (it is the user's own workspace).
- The governor changes only cgroup cpu.max within a fixed total — it
  cannot grant more CPU than the boot budget.
- The ext2 driver is the kernel's own; the OS writes only inside
  `/data/aios`.
