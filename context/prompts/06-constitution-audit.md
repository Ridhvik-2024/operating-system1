# Name: AUDIT-CONSTITUTION
**Use when:** periodically (every milestone), or after any change that touches
control flow, files, spawning or input.

---

Audit **AI-OS** against its own constitution and report the truth, not a
reassurance. Read `os_sys_decided.md`, `action_descriptor.schema.md`,
`policy_engine.interface.md`, `sandbox_result.schema.md`,
`rollback_snapshot.contract.md`, `os_sys_creating_ai_prompt.md`, then
`context/02-WHAT-IS-MISSING.md` and the code under `source/src/` and
`source/boot/`.

Produce a table with **one row per constitutional invariant**, in this shape:

| # | Invariant (quoted, with section) | Enforced by (file:line / syscall) | Test that proves it | Verdict |
|---|---|---|---|---|

Verdicts: `ENFORCED` (mechanism + passing test) · `PARTIAL` (mechanism exists
but a path bypasses it — name the path) · `PAPER` (documented only) ·
`VACUOUS` (nothing can exercise it yet).

Cover at minimum: §3 authority ordering · §4 the three layers · §5 input
arbitration · §6 what the AI may/may not do · §7 two-phase shell execution ·
§8 rollback · §10 memory separation · §11 filesystem staging/tombstones ·
§12 hardware consent · §13 the six non-negotiables.

Then:

1. **Score**: X enforced / Y partial / Z paper / W vacuous, out of the total.
2. **The three most dangerous `PARTIAL`s** — where a real bypass exists, with
   the exact steps to exploit it in the current build.
3. **Anything that is PAPER but claimed as done in `README.md`, `CHANGELOG.md`
   or `ROADMAP.md`** — quote both sides.
4. **The cheapest fix** that moves the most rows from PAPER to ENFORCED, with
   an acceptance test.
5. **A one-paragraph honest status** I could put in the README without
   over-claiming. No marketing.

Do not soften findings. If an invariant is vacuous, say it is vacuous — that is
better news than a false `ENFORCED`.
