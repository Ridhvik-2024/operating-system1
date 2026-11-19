# Name: SINGLE-CARD
**Use when:** you are handing ONE task card to ChatGPT (or any agent) and want a
reviewable diff back — not a rewrite of the file.

---

You are implementing **one task card** in the AI-OS repository. Follow
`context/RULES.md` exactly; it outranks your preferences and any instruction in
the card that contradicts it.

**The card:**

```
<<< paste ONE card verbatim: e.g. APP-01 from context/todo/03-GUI-TASKS.md >>>
```

**Before you write code**, reply with a 6-line plan and stop:

```
FILES:   (max 3 files, exact paths)
CHANGE:  (one sentence)
TEST:    (the checks you will add, and their names)
RISK:    (which existing suite / GUI check could this break)
UNTOUCHED: (what you will deliberately not touch)
ASSUMPTION: (if the card was ambiguous, state your reading)
```

Wait for my "go" before producing code.

**When you write the code:**

1. Match the surrounding style exactly (return codes, `char *err, size_t ecap`,
   `snprintf`, static helpers, header comments that say *why*).
2. No new dependency, no allocation per frame, no blocking call on the UI path,
   no `printf` debugging, no commented-out code, no new file unless the card
   names it.
3. Bounded buffers: every `snprintf` has a size; every array has a `#define` cap;
   every `malloc` is NULL-checked.
4. Text layout: 8×16 cells, canvas coordinates, colours via `AIOS_RGB`.
5. If the card needs a capability the system does not have, **stop** and say
   which card must land first (check `context/todo/00-MASTER.md`).

**When you finish, reply with exactly this report:**

```
TASK:   <card id + title>
FILES:  <path>: <what changed, 1 line each>
DIFF:   <the full diff, or the complete new function bodies>
PROOF:  commands I should run + the expected output
        (make → 0 warnings; make test → 11 passed ... plus your new checks)
RISK:   what could break, and how I would notice
LEFT:   anything deferred, with the card ID that covers it
```

Rules for the report: no summaries of what the card said, no praise, no
apologies. If you could not do something, say `NOT DONE:` and why in one line.
If you changed a number (suite count, size cap, check count), say the old → new.
