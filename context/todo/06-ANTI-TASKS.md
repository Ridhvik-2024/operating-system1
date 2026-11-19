# 06-ANTI-TASKS — tempting things that would hurt this project

Each entry: **the temptation** → **why it is wrong here** → **do this instead**.
Paste this file into any AI session; it is the fastest way to stop a plausible
but damaging change.

---

### 1. "Let's add a 6th app to the launcher"
**Why not:** the launcher is hardcoded for 5 (`w - 440 + i*86`). Slot 6 starts at
`w - 10` and is **off-screen**. You will "add an app" and ship an invisible one.
**Instead:** `APP-03` (launcher layout) first.

### 2. "The sandbox is in the way — let's give `tool-exec` more syscalls"
**Why not:** the ~54-syscall allowlist is the thing that makes the sandbox real.
Each addition is a new capability with no containment.
**Instead:** name the syscall you need and the invariant it serves; add it in the
same change as a policy rule and a test that proves the boundary still holds.

### 3. "Tests fail on my machine — I'll skip that suite"
**Why not:** the suites are the only evidence the OS has. A skipped check is a
claim you can no longer make.
**Instead:** `SKIP("reason")` only for a missing *host facility* (Landlock, eBPF),
and say so in the report. If a suite is flaky, fix the flake.

### 4. "Let's add networking so we can use a real LLM"
**Why not:** `-nic none` is a design decision, the seccomp allowlist kills
`socket()`, and the constitution's Mode A promises API access that cannot work.
Bolting a socket into the shell would be an unaudited, un-mediated capability.
**Instead:** `L7` — choose air-gapped (vsock) or mediated (`netd` in PID 1), and
write the choice into the constitution.

### 5. "Link a JSON/C library — ours is slow"
**Why not:** zero dependencies is what lets a 2.4 MB initramfs boot on a stock
kernel and what makes the seccomp allowlist meaningful. `src/json.c` is 444 lines
and already tested.
**Instead:** optimise `json.c` (or a caller) and add a benchmark to
`docs/PERFORMANCE.md`.

### 6. "Use systemd/dbus for the event bus"
**Why not:** it would be a second control plane, un-auditable and un-mediatable,
and it contradicts "the host control plane is not addressable by the AI".
**Instead:** `L1` — a 200-line broker in PID 1 on a unix socket.

### 7. "Let the agent write the file directly; staging is too slow"
**Why not:** that is precisely the invariant ("the AI never writes reality").
Speed is not an excuse the constitution accepts.
**Instead:** stage, then auto-promote at job end with a visible diff and a 30 s
undo window (conflict `C2` in `context/05-DECISIONS.md` recommends this).

### 8. "Give the AI its own cgroup tweak / admin socket so it can fix itself"
**Why not:** self-tuning of its own limits is the definition of escalation.
**Instead:** the AI may *recommend*; PID 1 decides and records. `tune` already
writes the conf — keep it human-only and audit it.

### 9. "Raise the initramfs size cap"
**Why not:** the cap exists because bloat killed boot twice before. Raising it
silently makes every future regression free.
**Instead:** justify it in `boot/.m10-size-cap` + CHANGELOG, with the byte count
and what the bytes buy, and only with a real feature (as M10 did for the GPU
stack).

### 10. "Replace PID 1 with a Python/bash script"
**Why not:** PID 1 must be a static binary with no interpreter, no dynamic
linking, and correct signal/reaping behaviour. The current 548 lines do exactly
that.
**Instead:** add the feature to `boot/init.c`, keep it under ~700 lines.

### 11. "Delete the desktop and just use the serial console"
**Why not:** the desktop is the only place a human can *see* the AI's effects;
it is also the natural home of the promote/diff UI (`M8`).
**Instead:** keep the console as the guaranteed front-end (`aios.gui=0`) and
improve the desktop.

### 12. "Add a `rm` / `mv` / `chmod` tool so the agent is more useful"
**Why not:** every destructive verb lands before staging/rollback exist (M3/M5).
That is how a project gets a reputation for eating files.
**Instead:** build M3 (staging, tombstones) first; then `fs.delete` becomes
"move to trash", which is safe by construction.

### 13. "Refactor `shell.c` while adding the feature"
**Why not:** 1 682 lines with 24 builtins in a hot path; a refactor buried in a
feature makes the diff unreviewable and the bug unattributable.
**Instead:** one concern per change. Put the refactor in the todo list.

### 14. "Auto-update the OS from a URL"
**Why not:** the constitution says the AI cannot update itself and updates must
be rollbackable; a URL update without verity/signature is a supply-chain hole.
**Instead:** `L4` — signed slots + `fs-verity` + A/B switch.

### 15. "Make the micro-LM bigger / add a transformer"
**Why not:** its value is that it is 12 KB, deterministic and offline; it is the
proof that the *OS* can think without a network.
**Instead:** keep it; improve routing accuracy with more corpus (`S-LM-02`), and
leave real models to the `cortex` seam (`S-LM-03` documents the seam).

### 16. "Commit a stub `font8x16.h` so the build passes"
**Why not:** a stub renders blank text; CI would go green while the desktop is
illegible.
**Instead:** regenerate the real table and commit it (`H1`). Stubs are only for
local compile checks.

### 17. "Turn off the boot selftest to speed up boot"
**Why not:** on its own, fine — but only **after** CI runs the same suites
(`H3`/`H5`). Turning it off before CI exists removes the only automatic proof.
**Instead:** `S-K-02` (default off) **plus** CI on.

### 18. "Hide the gaps in the README"
**Why not:** the docs' small over-claims are the project's biggest credibility
risk. A reviewer finds them in ten minutes.
**Instead:** `S-DOC-03` — a "What this is not" list. Honesty is the feature.
