# Contributing to AI-OS

AI-OS is an AI-native operating system on a Linux kernel base. This file is the
short version; the authoritative rules live in
[`context/RULES.md`](context/RULES.md).

## The one-paragraph version

Everything above the kernel is our own C11 with **zero runtime dependencies**
(libc, pthreads, libm). The kernel stays stock Linux. Safety is enforced by
architecture — isolation, mediation, rollback — never by prompts or model
goodwill. **Capability may scale; authority may not.**

## Before you write code

Read, in this order:

1. [`context/RULES.md`](context/RULES.md) — style, traps, definition of done
2. [`context/CONTEXT.agent.md`](context/CONTEXT.agent.md) — file map, recipes
3. [`context/02-WHAT-IS-MISSING.md`](context/02-WHAT-IS-MISSING.md) — what is
   actually missing (start here, not with a new feature)
4. `os_sys_decided.md` — the constitution. It outranks your preferences.

## Build and test

```bash
cd source
make -j4          # must be 0 errors AND 0 warnings
make test         # must print: == 11 passed, 0 failed ==
```

Requires only `cc` and `make`. Python is **not** needed for a plain build: every
generated artifact is committed (`src/microlm_weights.h`, `src/font8x16.h`).

To boot the OS (needs `qemu-system-x86_64`):

```bash
bash boot/fetch.sh                       # once: kernel, busybox, modules (~60 MB)
make -f boot/Makefile boot-image
bash boot/boot-test.sh                   # two boots on one disk: the durable proof
```

## Style (enforced by review)

- `aios_status` return codes; errors via `char *err, size_t ecap`.
- Library code (`src/`) never prints and never calls `exit()`.
- Every `malloc` is checked; `snprintf` always; no `strcpy`/`sprintf`.
- Public API in `include/aios/*.h` with a comment saying *why* it exists.
- Build with the full flag set: `-Wall -Wextra -Wshadow -Wformat
  -Wformat-security -fstack-protector-strong -D_FORTIFY_SOURCE=2`.
- No new runtime dependency. Syscalls or our own code instead.

## Tests

A change without a test is not done.

- Logic → a suite in `tests/test_<name>.c` using `tests/t.h` (`CHECK`, `SKIP`).
  Suites run on the host **and inside the booted OS**.
- UI → test through the in-memory framebuffer seam
  (`aios_fb_open_mem()` / `aios_fb_px()`), as `tests/test_fb.c` does.
- A missing kernel facility (Landlock, eBPF) is `SKIP("reason")` — never a silent
  pass, never a deleted check.

## Hard stops

1. No new dependency, no vendored library, no dynamic loading.
2. Never weaken or bypass the sandbox to make something work.
3. No writes outside the jail; no direct file writes from the GUI.
4. No new authority for AI code: it may not edit policy, the audit log, its own
   limits, or PID 1's decisions.
5. Do not edit the constitution (`os_sys_decided.md` and the four contracts).
6. Never commit `*.o`, `*.d`, `build/`, runtime stores, or new binary blobs.

See [`context/todo/06-ANTI-TASKS.md`](context/todo/06-ANTI-TASKS.md) for the
eighteen tempting shortcuts that would hurt the project.

## Pull requests

- One concern per PR. If it needs two, it is two PRs.
- Commit messages: `area: what changed and why`, with the *verified* evidence
  (command → result), not a claim.
- State what you could not verify (e.g. "no QEMU in my environment") — that is
  expected, not a failure.

## Licence

Apache License 2.0 — see [LICENSE](LICENSE). By contributing you agree your
contribution is licensed under the same terms.
