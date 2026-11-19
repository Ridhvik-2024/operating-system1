# Name: HANDOFF
**Use when:** ending a session — so the next chat (or the next person) starts
where you stopped, not from scratch.

---

Update the AI-OS context folder (`context/`) so a new session can resume without
reading the whole repo. Read what exists first, then edit — do not rewrite from
memory.

Do exactly this:

1. **`context/CONTEXT.md`** — update: version in tree, snapshot date, the
   "Changes made in this snapshot" section (what you actually did, with files),
   and §8 "Current focus and next three actions". Keep it under ~250 lines; if
   it grows, move detail into the numbered files.
2. **`context/01-WHAT-IS-HERE.md`** — add any new subsystem/module/command to the
   inventory, with `file:line` evidence. Remove entries for code you deleted.
3. **`context/02-WHAT-IS-MISSING.md`** — move closed gaps to a `## Closed` list
   at the bottom with the commit/date; add new gaps with a severity (S1/S2/S3);
   never delete a gap silently.
4. **`context/03-LINUX-INTEGRATION.md`** — tick the phase items you completed.
5. **`context/04-BUILD-VERIFY.md`** — append a dated section: exact commands,
   pass/fail counts per suite, and every claim that could not be reproduced
   (say why: no QEMU, no Landlock, etc.).
6. **`context/05-DECISIONS.md`** — append a `D<n>` entry for each real decision
   you made (date, decision, why, cost, what would reverse it). If you resolved
   an open conflict (C1–C5), mark it resolved and say how.

Rules:
- Facts only. If you did not run it, say "not verified here".
- Every number gets its command.
- If you changed behaviour, say which test proves it.
- Do not update the constitution documents (`os_sys_decided.md`, the four
  contracts) — they change only by an explicit new-major-version decision.
- Finish with a 5-line "resume from here" block at the very top of `CONTEXT.md`,
  phrased as an instruction to the next assistant.
