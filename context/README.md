# AI-OS — context folder (project memory for humans and AIs)

This folder is the **memory** of the AI-OS project. It exists so any chat,
person, or coding agent can resume work without reading 12 000 lines of C.

## Start here

| If you are… | Read |
|---|---|
| a human, first time | `01-WHAT-IS-HERE.md` → `02-WHAT-IS-MISSING.md` |
| a coding agent (me, Cursor, Copilot, Claude Code) | `CONTEXT.agent.md` → `04-BUILD-VERIFY.md` |
| ChatGPT / a new chat with no repo access | `CONTEXT.chatgpt.md` (paste it whole) |
| returning after a break | `CONTEXT.md` (top block = resume instructions) |

## The five-line version

1. **AI-OS** is an AI-native operating system on a **stock Linux kernel**;
   everything above the kernel is our own C11 (zero runtime dependencies).
2. It **boots** (Linux 6.6.142 + our PID 1 + 2.4 MB initramfs) and runs a
   desktop, an AI shell, agents, a self-tuning scheduler and a durable store.
3. It **works and is tested** — 11 suites on the host and inside the booted OS.
4. Its **constitution** (policy engine, action descriptors, staging, snapshots,
   rollback, authority model, audit chain) is **not implemented** — that is the
   next milestone.
5. Direction: **adopt Linux primitives** (namespaces, overlayfs, dm-snapshot,
   seccomp-notify, unix sockets, PSI, fs-verity, aarch64) instead of inventing them.

## Files

```
context/
├── README.md                  ← you are here
├── CONTEXT.md                 the memory (resume block + current state)
├── CONTEXT.agent.md           coding-agent memory: commands, traps, recipes
├── CONTEXT.chatgpt.md         self-contained paste for ChatGPT
├── RULES.md                   BUILD RULES — universal + AI-OS-specific + traps
├── ROADMAP-TO-WINDOWS.md      v1.1 -> v5.0: W0..W6, phases, exit tests, effort
├── 01-WHAT-IS-HERE.md         inventory of every subsystem, with evidence
├── 02-WHAT-IS-MISSING.md      gaps G1–G23, severity, smallest honest fix
├── 03-LINUX-INTEGRATION.md    which Linux parts, in which phase, acceptance criteria
├── 04-BUILD-VERIFY.md         exact commands, real pass/fail numbers, doc drift
├── 05-DECISIONS.md            decision log + 5 open conflicts to resolve
├── todo/                      the task lists
│   ├── 00-MASTER.md           phase plan + index + progress tracker
│   ├── 01-HYGIENE.md          build/git/docs/CI (H1–H6) — all DONE
│   ├── 02-OS-CORE.md          the mediator M11 (M1–M8) — needs judgement
│   ├── 03-GUI-TASKS.md        desktop + UI cards → **give these to ChatGPT**
│   ├── 04-SMALL-TASKS.md      tiny drop-in cards (≤1 h) → ChatGPT too
│   ├── 05-LARGE-TASKS.md      multi-week arcs (L1–L7) — do not delegate
│   ├── 06-ANTI-TASKS.md       18 temptations that would hurt the project
│   └── 07-FREE-TODO-FORMAT.md your own scratch list
└── prompts/                   named prompts (00-README lists them)
```

**Hygiene (H1–H6) is complete.** A clean clone builds with no Python; CI runs
build (`-Werror`) + tests on two Ubuntu generations + the two-boot QEMU durable
proof; the repo is Apache-2.0; the docs match the tree; and there is exactly one
copy of every binary — 31.2 MiB tracked, documented in
[`../docs/REPO-LAYOUT.md`](../docs/REPO-LAYOUT.md).

## Delegating a task to ChatGPT (30 seconds)

1. Paste `context/CONTEXT.chatgpt.md` (or just `RULES.md` for pure code work).
2. Paste **one** card from `context/todo/03-GUI-TASKS.md` or `04-SMALL-TASKS.md`.
3. Paste `context/prompts/09-single-card.md`.
4. When it answers, run §3.3 of `RULES.md` yourself — never trust the claim.

## Prompts by name

`BOOTSTRAP` · `ARCHITECT` · `MEDIATOR-SPEC` · `BUILD-FIX` · `LINUX-ADOPT` ·
`AUDIT-CONSTITUTION` · `DESKTOP-APP` · `HANDOFF` — see `prompts/00-README.md`.

## Maintenance rule

Code changes → update `04-BUILD-VERIFY.md` with the command and the number.
New gap → line in `02-WHAT-IS-MISSING.md`. Real decision → entry in
`05-DECISIONS.md`. Never delete history; mark it closed.
