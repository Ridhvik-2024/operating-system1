# Name: MEDIATOR-SPEC
**Use when:** you start the milestone that makes the constitution true
(decided: M11, see `context/05-DECISIONS.md` D5).

---

We are building **the mediator** for AI-OS: the component that sits between an
AI-proposed action and reality. Read `context/CONTEXT.md`,
`context/02-WHAT-IS-MISSING.md` (G1–G9), `action_descriptor.schema.md`,
`policy_engine.interface.md`, `sandbox_result.schema.md`,
`rollback_snapshot.contract.md`, then `source/src/shell.c` (`exec_tool`,
`builtin_spawn`, `run_agent_fg`), `source/src/sandbox.c`,
`source/src/tool-exec.c` and `source/src/durable.c`.

Produce a **specification**, not code, containing:

1. **Data model** — the exact C structs and their JSON encoding for:
   `aios_action` (kind, target, declared effects, resource budget, reversibility,
   intent hash, expiry, `origin = HUMAN | AI`), `aios_effect` and
   `aios_verdict` (ALLOW / DENY / NEEDS_HUMAN + a machine-readable reason code).
   Every field must say who may set it.
2. **The pipeline** — the exact order of calls for a single tool action and for a
   single agent step, naming the function in each file. Show where the process
   boundary and the uid boundary are.
3. **Law table (Tier 0/1 v1)** — a concrete, finite list of rules. For each:
   the descriptor fields it reads, the verdict, and the test that proves it.
   Include at minimum: writes outside `/workspace`; deletes; bulk changes;
   spawning agents from an agent; any `exec`; any network; any device access.
   `DENY` everything not matched (fail-closed).
4. **Staging and diff** — how a write becomes a staged change, what the diff
   format is, what `promote`/`discard` do atomically, how deletes become
   tombstones, and what happens on crash mid-promote. Prefer **overlayfs**
   (`lowerdir`/`upperdir`) over hand-written copies; say what changes if we
   cannot mount in the host-runtime build.
5. **Snapshot and rollback** — what is snapshotted (store, staging, workspace),
   where it lives, cost in bytes, and the exact restore path. State which
   component owns it (it must not be the component that takes AI input).
6. **Audit chain** — record format with `prev` hash, where it is written, who can
   write it, and the `audit verify` algorithm including what it does with a torn
   tail.
7. **Human authority** — how the shell knows an action is `origin=HUMAN`
   (TTY vs model), and how `NEEDS_HUMAN` is presented on both the serial console
   and the desktop. What happens if a human never answers.
8. **Failure semantics** — mediator crash, policy error, staging full, promote
   interrupted, rollback impossible. Every case must fail **closed**.
9. **Migration** — which existing tests change, which builtins gain a new
   argument, what happens to `fs.write` (see conflict C2 in
   `context/05-DECISIONS.md`), and how to land this without breaking
   `boot-test.sh`.
10. **Acceptance** — the exact tests and their assertions, including one
    adversarial test: an agent that tries to write outside the workspace must be
    denied **and** the denial must appear in the verified audit chain, and after
    the run the sandbox must be provably empty.

Keep it to what one engineer can land in two weeks, and mark anything you would
defer with `LATER:` and why.
