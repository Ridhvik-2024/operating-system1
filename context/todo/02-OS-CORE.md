# 02-OS-CORE — the mediator (M11): make the constitution true

These are **not** ChatGPT-sized. They need judgement, tests and an
understanding of the constitution. Read `context/02-WHAT-IS-MISSING.md` G1–G9
first; each card says which gap it closes.

Guiding sentence for all of them: *the OS must be able to answer, for every
effect the AI caused: what was proposed, what was allowed, what actually
happened, and how do we undo it.*

---

## M1 — Action Descriptor (`aios_action`) · **M** · closes G2 — ✅ **DONE 2026-11-17**

> `include/aios/action.h` + `src/action.c`, `tests/test_action.c` (129 checks).
> Every command in `shell.c` now passes through `action_gate()`; both the typed
> (HUMAN) and routed (AI) paths are verified against the running shell, with the
> descriptor captured on `sh.action.{accept,refused}`.
>
> **Three decisions worth carrying into M2–M6:**
> 1. **Trust fields never come from parsed JSON.** `origin` arrives
>    `ORIGIN_UNSPECIFIED` and the *caller* stamps it; expiry may be shortened by
>    input but never extended (clamped to now + 30 s). Proven: a descriptor
>    claiming `"origin":"human"` is refused for having no host stamp.
> 2. **`declare_tool_effects()` is the one table of what a tool touches**, next
>    to the type rather than inferred from tool behaviour. `effects_complete`
>    distinguishes "touches nothing" from "forgot to declare".
> 3. **`intent_hash` is an exact hex STRING.** It is 64-bit; JSON numbers are
>    doubles and silently dropped the low 11 bits — caught by the round-trip
>    test, would have broken M6 audit correlation.
>
> **Deliberately not done:** nothing is *blocked* yet. Validation refuses
> malformed descriptors; denying legitimate-but-forbidden ones is `M2`.
>
> **Hardening found by probing it afterwards:** a path too long to record was
> silently truncated into the declared effect (the record then named a
> different resource than the one touched) and a failed declaration was
> ignored. Both now fail closed - refuse, with the reason. `effects_complete`
> means UNKNOWN when 0, which is why the refusal lives at the call site and not
> in `is_valid()` (agents cannot enumerate effects until they plan).

**Goal.** One type that represents *any* AI-proposed action, before it happens.

**Do.**
1. `include/aios/action.h`:
```c
typedef enum { ACT_TOOL, ACT_AGENT, ACT_FS_WRITE, ACT_FS_DELETE,
               ACT_JOB_SPAWN, ACT_SHELL } aios_act_kind;
typedef enum { ORIGIN_HUMAN = 0, ORIGIN_AI = 1 } aios_origin;

typedef struct {
    aios_act_kind kind;
    aios_origin   origin;        /* set by the shell, never by the model   */
    char         *target;        /* path / tool name / goal               */
    json_val     *args;          /* tool args or action payload           */
    char         *declared[16];  /* effects claimed in advance: "write:a.txt" */
    int           n_declared;
    double        budget_ms;     /* wall clock the action may take        */
    int           reversible;    /* 1 = fully undoable                    */
    uint64_t      intent_hash;   /* aios_fnv1a of the human intent text   */
    double        expires_ms;    /* descriptor is void after this         */
} aios_action;

aios_action *aios_action_new(aios_act_kind, aios_origin, const char *target);
aios_status  aios_action_add_effect(aios_action *, const char *effect);
char        *aios_action_to_json(const aios_action *);   /* owned */
aios_action *aios_action_from_json(const char *, char *err, size_t ecap);
void         aios_action_free(aios_action *);
int          aios_action_is_valid(const aios_action *, char *err, size_t ecap);
```
2. Validation rules: non-empty target, `budget_ms > 0 && <= 30000`,
   `expires_ms > now`, at most 16 declared effects, each effect
   `verb:path` with a verb from a fixed list. **Reject** anything else.
3. Every route in `src/shroute.c` (TOOL, AGENT, REPLY) must produce a descriptor;
   `src/shell.c::exec_tool` and `run_agent_fg` take `const aios_action *`
   instead of a bare name/args pair.
4. `origin` comes from the *caller*: `apps/aiosh.c` marks console input HUMAN;
   `shroute.c`/agent paths mark AI. A descriptor with `origin` unset is invalid.

**Acceptance.** `tests/test_action.c`: round-trip JSON; each validation rule
has a failing case; a descriptor claiming 17 effects is rejected; an expired
descriptor is rejected; `origin=AI` cannot be constructed from JSON (only from
code). `make test` → 12 suites.

**Risk.** This touches the shell's hottest path. Land it as a *pass-through*
first (build the descriptor, ignore it) with tests, then gate on it in M2.

---

## M2 — `policy_eval()` — fail-closed law table · **M** · closes G1 — ✅ **DONE 2026-11-17**

> `include/aios/policy.h` + `src/policy.c`, **169 checks**; the shell gate now
> asks policy for **every** command and obeys the verdict. `policy` and
> `policy check <kind> <target>` make the table readable at the console.
>
> **Enforced in the documented order** (policy_engine.interface.md §3), one
> function per step: schema → tier 0 → sandbox → tier 1 → rollback → advisory →
> verdict. Tier 0 **binds the human too**: the audit log, the law and PID 1's
> socket are refused for *every* origin (verified end-to-end).
>
> **Purity is real, and making it real found an M1 defect:** the evaluator took
> `now_ms` as a parameter but `aios_action_is_valid()` read the clock anyway, so
> "pure" was aspiration. Added `aios_action_is_valid_at()`. A pure function that
> calls an impure one is just a function that is usually reproducible.
>
> **Bugs the tests found (not code review):**
> 1. Requirements were discarded — steps 5/6 built fresh decisions, so
>    `needs_rollback` vanished from the final verdict. Now accumulated via
>    `inherit()`.
> 2. Policy read only **declared** effects, so a path that *could not be
>    declared* (a `..` component is refused by M1) left the set empty and
>    passed. Step 2 now inspects the path the action **carries** as well as the
>    one it claims.
> 3. Only `fs.write` was gated; delete/agent had no verdict at all.
> 4. Reason quality: a delete reported the sandbox rule instead of
>    `R_DESTRUCTIVE`, sending readers after the wrong problem.
>
> **Still not real:** no capability *issuance* (the list is compiled in), no
> Tier 1 time-bound overrides, no emergency-mode producer, no sandbox-result
> comparison (M4). `rollback_available` is hard-wired to 0 because no snapshot
> store exists (M5) — that is why destructive machine actions still stop at a
> human.

**Goal.** A pure function that turns a descriptor into `ALLOW / DENY /
NEEDS_HUMAN`, with a machine-readable reason code.

**Do.**
1. `include/aios/policy.h`:
```c
typedef enum { VERDICT_ALLOW, VERDICT_DENY, VERDICT_NEEDS_HUMAN } aios_verdict;
typedef enum { R_OK, R_NOT_MEDIATED, R_PATH_OUTSIDE_JAIL, R_BULK_CHANGE,
               R_DESTRUCTIVE, R_AI_SELF_LIMIT, R_NO_CAPABILITY, R_EXPIRED,
               R_BUDGET_TOO_LARGE } aios_reason;
typedef struct { aios_verdict v; aios_reason r; char why[160]; } aios_decision;

aios_decision aios_policy_eval(const aios_action *a, const char *jail,
                               const char *const *caps, int n_caps);
const char *aios_policy_reason_str(aios_reason);
```
2. Tier-0 rules (compile-time constant table, **no file, no env, no config**):
   - `DENY` any action whose effect path is outside the jail.
   - `DENY` `ACT_SHELL` for `origin=AI`.
   - `DENY` any action touching `/data/aios/store/audit.log`, the policy table
     itself, or PID 1's socket.
   - `DENY` `ACT_JOB_SPAWN` from inside an agent (no agent spawns agents).
   - `NEEDS_HUMAN` for delete, for >8 files written in one action, and for any
     `ACT_FS_WRITE` outside `/workspace`.
   - `ALLOW` everything else **only** if a matching capability is present.
3. **Fail closed**: unknown kind, unknown reason, missing capability, empty
   descriptor → `DENY`. `aios_policy_eval` must never return ALLOW by accident.
4. Pure: no I/O, no allocation (fixed buffers), unit-testable. It must not be
   able to read the clock (pass `now` in) — determinism is the point.

**Acceptance.** `tests/test_policy.c` with **one case per rule** (≥ 20 cases),
including a table-driven loop over every `aios_reason`. Add a check that the
function is deterministic: same inputs → identical `aios_decision` bytes.

---

## M3 — Staging, diff, promote/discard · **L** · closes G4 — ✅ **DONE 2026-11-17**

> `include/aios/stage.h` + `src/stage.c`, **122 checks**. An AI write lands in
> `<store>/staging/<intent>-<seq>/upper/`, the human sees a diff, and `promote`
> is the only thing that makes it real. Deletes move to `trash/` with a
> tombstone; `stage_undelete()` restores them.
>
> **The card's premise was wrong, and the tests found it.** The card said wire
> `tool-exec`'s `fs.write`. But the agent loop called `tool_exec()` directly —
> it never built a descriptor, never asked policy, and never went through the
> shell's gate. So the largest hole in the constitution was not a missing
> staging layer; it was that **the component most likely to act unsupervised
> was the one unmediated path in the system**. Fixed with `agent_guard`: the
> loop must ask before every tool, and a refusal is reported back into the
> planner's history rather than aborting the run.
>
> **Bugs found by testing, in order of embarrassment:**
> 1. `realpath()` was handed a 2048-byte buffer. Stack smash, every AI write.
> 2. Agent descriptors had no intent hash → M1 refused every step. Fail-closed
>    and completely useless; the guardian looked "installed" and nothing worked.
> 3. The action target was the *path* instead of the *tool*, so every step
>    failed the capability lookup.
> 4. Policy required a sandbox for a single write — which no single write can
>    ever satisfy — and looked up `act:FS_WRITE` while the shell grants
>    `tool:fs.write`. Two vocabularies for one grant meant **every AI write was
>    denied**. Milestones M1 and M2 both shipped that bug; it only became
>    visible when something finally tried to write.
> 5. Promote counted *notes* rather than *moves*, so a re-run after an
>    interruption claimed to have promoted files it had not touched.
> 6. The diff measured deletes against the jail, where the file no longer
>    exists, reporting every delete as 0 bytes — the one number a human reads.
>
> **Promote is two functions on purpose.** `stage_promote()` trusts the
> directory; `stage_promote_locked()` — the one a human triggers — re-derives
> everything from the manifest and refuses symlink escapes, because a person who
> can write into the store could otherwise promote an arbitrary file and bypass
> the whole policy engine without it ever looking like a bypass.
>
> **Still not real:** no overlayfs (this is copy-on-write with renames, which is
> portable and testable but O(bytes)); no diff *content*, only per-file counts
> (+lines/-lines are computed by prefix comparison, so a whole-file rewrite
> reads as such); staging lives inside the store, so it is not protected from a
> human who owns the store; `promote` trusts that the human typing it is the one
> who was shown the diff (no session binding until M6/M7).

**Goal.** No AI write lands in reality. It lands in staging, shows a diff, and a
human promotes it.

**Do.**
1. `include/aios/stage.h` + `src/stage.c`:
```
stage_begin(intent_hash)  -> staging dir  <store>/staging/<intent>/upper
stage_write_jail()        -> path tools should write into
stage_diff(dir, out)      -> list of {path, op, bytes, +lines/-lines}
stage_promote(dir)        -> atomic: rename files into the jail, fsync dir
stage_discard(dir)        -> move to <store>/staging/.trash/<ts>-<intent>
stage_gc(max_age_ms)      -> drop stale/empty staging dirs
```
2. Preferred implementation inside the image: `mount -t overlay` with the jail
   as `lowerdir` and the staging dir as `upperdir`; `promote` = renames out of
   `upper`. Host runtime fallback (no mount privileges): a copy-on-write temp
   dir + a three-way compare against the original.
3. Deletes never `unlink` — they move into the trash dir, and the tombstone is
   recorded in the audit chain. `stage_undelete()` reverses it.
4. Crash safety: a staging dir with no `manifest.json` is garbage; `stage_gc`
   removes it. `promote` writes a manifest first, then renames, then fsyncs —
   an interrupted promote must be detectable and re-runnable (idempotent).
5. Wire: `src/tool-exec.c`'s `fs.write` receives the staging dir from the parent
   (an extra argument, not an env var) and refuses to run without one.

**Acceptance.** `tests/test_stage.c`:
- write → file does **not** exist in the jail, **does** exist in staging;
- diff lists exactly the changed paths and the byte delta;
- promote → file appears in the jail with the right content, staging is empty;
- discard → jail untouched, file present in trash;
- interrupted promote (simulate by removing the manifest mid-way) → detected,
  re-runnable, no partial file in the jail;
- `stage_gc` removes a 0-byte orphan dir and keeps a live one.

**Risk.** Highest-risk card. Land it behind `AIOS_STAGE=1` first and keep the
direct-write path for the existing tests; flip the default only when the boot
suite is green.

---

## M4 — `tool-exec` returns an effects document · **M** · closes G3 — ✅ **DONE 2026-11-17**

> `include/aios/effects.h` + `src/effects.c`, **84 checks**; wired through
> `sandbox_run_ex()`, `tool_reg_last_effects()` and the shell's post-check.
>
> **The card's design is implemented and it is not sufficient, which the
> adversary test proved.** A swapped `tool-exec` that ignores the `stage`
> argument, writes into the jail and reports only what it was asked about,
> produces an account that matches the declaration exactly. Every rule the card
> asks for passes. The file is already in the jail.
>
> So the guarantee was being enforced **by the worker**, the component we least
> control. Added the check that does not consult it: the parent hashes each
> declared path before and after, and refuses with `JAIL_MUTATED` if the jail
> moved. **Testimony is what we ASK; the witness is what we CHECK.**
>
> **Honest limit:** the leaked file IS in the jail — a swapped worker can write
> it. This milestone notices and refuses the result. Making the write impossible
> needs the read-only mount (L2). The test asserts the limit so nobody later
> mistakes it for more.
>
> **Also found:** `tool_exec` never cleared `*result_out`, so any caller reusing
> one variable double-freed on the first failure (ASan found it); `REQ_MAX` was
> 64 KiB against a 1 MiB write limit, making the documented limit unreachable;
> a failed tool's account was thrown away, so "failed" meant "says nothing" even
> when it had written something first; and two test fixtures skipped instead of
> failing when they did not compile — a skipping test reports PASS.

> **And the part worth more than the feature:** the first version of this passed
> CI's build and test jobs and failed the boot job four times. Not because the
> logic was wrong — 15/15 passed on the host and in an extracted rootfs — but
> because the *tests themselves* could not run where the product runs: a fixture
> that needed a compiler the image does not have, fixtures linked against a libc
> the initramfs does not have, and a `/proc/self/exe` lookup before `/proc` is
> mounted. Three separate bugs, all invisible on a developer machine, all silent.
> Then the last one was real: an over-limit write was refused by the worker after
> the parent had shipped a megabyte down a pipe, which under emulation arrives
> after the call's own deadline — so a clear refusal read as a timeout.
>
> **Run the suite where the product runs** (D22). `boot-test.sh` and
> `aios-selftest.sh` now put the failing assertions in the job annotation rather
> than an artifact nobody opens; four cycles went into fixing the diagnostics that
> would have found the bug in one.


**Goal.** The host learns what *actually* happened, not what the model claimed.

**Do.**
1. Extend the worker protocol (one JSON object, as today):
```json
{"ok":true,
 "result":{...},
 "effects":[{"path":"a.txt","op":"write","bytes":12}],
 "usage":{"ms":3,"cpu_ms":2,"bytes_read":12,"bytes_written":12}}
```
2. In `src/tool-exec.c`, instrument `fs.write` / `fs.read` to append to an
   effects array; keep the caps (`FS_READ_MAX` 256 KiB, `FS_WRITE_MAX` 1 MiB).
3. In `src/sandbox.c`, parse and hand `effects` back to the caller; **any**
   effect that was not in the descriptor's `declared[]` is an undeclared effect.
4. Undeclared effect → the action is auto-`DENY`d after the fact: discard the
   staging dir, write an audit entry with `reason=UNDECLARED_EFFECT`.

**Acceptance.** `tests/test_sandbox.c` additions: a tool that writes a file it
did not declare → result `ok:false` + `undeclared_effect`; a declared write →
`ok:true` and effects match exactly; a tool that writes more than
`FS_WRITE_MAX` → error, no effect recorded.

---

## M5 — Snapshots and rollback · **L** · closes G5 — ✅ **DONE 2026-11-17**

> `include/aios/snapshot.h` + `src/snapshot.c` (**86 checks**),
> `include/aios/supervisor.h` + `src/supervisor.c` (**55 checks**), shell builtins
> `snap` / `rollback` (in `tests/test_shell.c`), `dur` now reports snapshot
> availability, and the boot proof runs in the real image on every CI run.
>
> **The card's own premise needed correcting in three places:**
> 1. It said `snap_restore` "swaps dirs atomically". It cannot. `rename()` across
>    filesystems is `EXDEV` (D23), so the restore stages siblings and moves the
>    current state aside first; the guarantee is "a failed restore leaves the
>    previous state recoverable", not "the swap is atomic".
> 2. It said ids were timestamps. Timestamps collide on a coarse virtual clock
>    (D24).
> 3. It said the socket check "never accepts an AI uid (Phase 3)". Everything runs
>    as one uid today, so that check cannot distinguish anything yet; the header
>    says so instead of implying a boundary that does not exist (D25).
>
> **What is real:** take/restore across filesystems, verified in the image;
> retention enforced on every take; a snapshot survives a reboot; restore revokes
> running jobs; PID 1 serves `snap|list|restore|kill|ping` on
> `/run/aios/supervisor.sock` (0600) and the shell asks rather than acting.
>
> **What is not:** no overlayfs, so a snapshot is O(bytes) (an 8 MiB disk holds a
> handful — hence `AIOS_SNAP_KEEP 5`); no content diff between snapshots; nothing
> yet binds "the diff you were shown" to "the rollback you asked for" (that is
> M6/M7's hash chain).

**Goal.** Rollback exists, is available always, and is owned by PID 1 — not by
the component that takes AI input.

**Do.**
1. `include/aios/snapshot.h`:
```
snap_take(const char *name)        -> snapshot id   (store + staging + workspace)
snap_list(out)                     -> ids with ts + sizes
snap_restore(const char *id)       -> swap dirs atomically, restart agents/jobs
snap_prune(keep_n)
```
2. Cheapest correct v1: `data.img` is only 8 MiB — copy the store dir + the
   workspace into `<store>/snapshots/<ts>/` with `durable.c`'s fsync discipline.
   Later (Phase 3): btrfs subvolume or `dm-snapshot`.
3. Take a snapshot automatically **before** each agent run and before each
   `promote` batch; keep the last 5 (`snap_prune`).
4. PID 1 owns it: `aios-init` listens on a unix socket and serves
   `snapshot|restore|kill`; `aiosh` asks, the AI cannot ask (the socket never
   accepts a connection whose `SO_PEERCRED` uid is the AI uid — Phase 3).
5. `aiosh` builtins: `snap`, `rollback [micro|session|<id>]`, plus a visible
   line in `dur`.

**Acceptance.** `tests/test_snapshot.c`: take → mutate → restore returns the
store and workspace to the exact prior bytes (compare a hash of both trees);
restore revokes running jobs (a spawned job is gone afterwards); a snapshot
survives a simulated reboot (write, then re-open the store and list).
Boot proof: extend `boot-test.sh` — boot, write a file, snapshot, write again,
rollback, reboot, assert the first content.

---

## M6 — Hash-chained audit + `audit verify` · **M** · closes G8 — ✅ **DONE 2026-11-17**

> `include/aios/audit.h` + `src/audit.c` (**77 checks**), `audit [verify]` builtin,
> `dur` reports chain health, boot proof verifies the chain in the image on every
> CI run.
>
> **The card's promise needed splitting into two claims**, because only one of them
> is true and the other is what "tamper-evident" quietly implies:
> - **true:** a changed record, a removed middle record, a reordering and a forged
>   append are all detected, at the right seq (D27);
> - **NOT true and cannot be:** a wholesale rewrite, or a cleanly removed final
>   record, is undetectable from the file alone. The card asked for the deleted tail
>   to be reported as "torn"; it is not, because nothing about it is unfinished
>   (D27). `audit verify` prints the head hash so a future milestone can commit to
>   it somewhere the writer cannot reach.
>
> **The bug that makes the point better than the feature does:** the first version
> truncated the log to zero before every append, so it held one record, always
> `seq 1`, always chaining to genesis — and `verify()` said "1 records, chained and
> intact". A verifier's opinion about a log it has seen one record of is not
> evidence (D28). Tests now assert the chain GREW.
>
> **Also honest:** the write is still made by the component being audited
> (`tools.c`, on the AI-facing path). Policy denies writes to `audit.log`, so the
> AI cannot edit it through a tool, but moving the writer into PID 1's supervisor
> is M7's work and is the only thing that makes suppression impossible rather than
> merely visible.
>
> **In the image, on the ext2 disk, after real sandboxed calls:** boot 1 → 6 records
> intact; boot 2 → 14 records intact. The count rising is the assertion.

**Goal.** Tamper-evident record of every decision and every effect.

**Do.**
1. One record per action, JSONL, at `<store>/audit.log`:
```json
{"seq":7,"ts":...,"origin":"ai","action":{...},"verdict":"allow",
 "reason":"R_OK","effects":[...],"prev":"<hex>","hash":"<hex>"}
```
`hash = fnv1a64(prev || canonical_json_without_hash)`. Use `aios_fnv1a` (already
in `types.h`) — it is not cryptographic, so **say so in the header comment**;
upgrade to sha256 later.
2. Written through `durable.c`'s append+fsync. Nothing else may append to it.
3. `audit verify`: walk from the first record, recompute, report the first bad
   seq and whether the tail is torn (torn tail = crash artifact, not tampering —
   `durable.c` already has that notion).
4. Replacement for the current `tools.c::audit_log()` (which is plain append).
   Keep the old fields so `dur`'s "audit: N tool calls" keeps working.

**Acceptance.** `tests/test_audit.c`: 100 records verify clean; changing one
byte in the middle is detected at the right seq; deleting the last line is
reported as a torn tail, not as tampering; two writers cannot interleave
(append is single-writer by contract — assert with a `flock` or an explicit
`EBUSY`).

---

## M7 — Origin tagging (human vs AI) · **M** · closes G6 — ✅ **DONE 2026-11-18**

> `sh_exec_origin()` (no default), the human-only builtin gate, `stage_promote()`
> taking an origin, the three-choice hold in both front-ends, and the stale
> `rollback_available = 0` replaced by a real question. **300 checks in
> `test_shell`** (up 76), 122 in `test_stage`, plus a two-direction boot proof.
> Three defects found by doing it rather than by reading the card — see D31.

**Goal.** The OS knows whether a request came from a human or from a model, and
only human-origin actions may ever be promoted without confirmation.

**Do.**
1. `apps/aiosh.c`: stdin lines are `ORIGIN_HUMAN`. `sh_exec()` gains an
   `origin` argument (or a `sh_set_origin()` per call) — no default.
2. `shroute.c`: everything the router produces is `ORIGIN_AI`.
3. Agent tool calls: `ORIGIN_AI`. `spawn`: inherits the caller's origin, and
   `ORIGIN_AI` may not spawn (policy M2 already denies it).
4. The desktop: launcher text is human; a future AI-driven input path (UI-07)
   sets `ORIGIN_AI` explicitly.
5. `NEEDS_HUMAN` verdicts are presented on the serial console *and* in a desktop
   window with three choices: `promote`, `discard`, `show diff`. No timeout
   auto-promote — timeout = discard.

**Acceptance.** `tests/test_shell.c` additions: a line executed with
`ORIGIN_AI` that writes outside `/workspace` gets `NEEDS_HUMAN` and is not
written; the same line with `ORIGIN_HUMAN` (after `promote`) succeeds. A test
that an AI-origin action can never reach `stage_promote()` directly.

**What landed, and what the card got wrong.**

1. `sh_exec_origin(sh, line, origin, out)` is the only entry point;
   `sh_exec()` is *defined* as `ORIGIN_HUMAN` and nothing else. `ORIGIN_UNSPECIFIED`
   is refused, not defaulted — a default is the silent assumption the milestone
   exists to remove. `sh_last_origin()` reports what the last line was stamped as,
   and a fresh shell reports `UNSPECIFIED` (it was `calloc`'d to 0, and 0 is
   `ORIGIN_HUMAN` — the first bug this milestone found).
2. The builtin gate (`SH_HUMAN_ONLY`) refuses `promote`, `discard`, `rollback`,
   `snap`, `exit`, `quit`, `cortex`, `autopilot`, `gc`, `undelete` on an
   AI-origin line, **at dispatch, not in the router** — a guard a router rewrite
   could drop is not a guard. `policy`, `staged`, and `machine` are deliberately
   allowed: introspection makes a refusal understandable, and blinding a model
   does not make it safer.
3. `spawn` refuses `ORIGIN_AI` at the door *and* inherits the caller's origin
   otherwise, so a spawned job is judged by the same standard as the line that
   asked for it.
4. `stage_promote()`/`stage_promote_locked()` now **require** an `aios_origin` and
   refuse anything but `ORIGIN_HUMAN` in the implementation. This is the
   acceptance test's "can never reach `stage_promote()`": enforced where the
   rename happens, not only where it is called from.
5. The hold. `action_gate` used to collapse `NEEDS_HUMAN` into a refusal
   (`if (d.v != VERDICT_ALLOW)`), which made the middle of the verdict space
   unreachable. It now offers the action via `ui_present()`: three choices,
   bounded, and **no path from "nobody answered" to promote** — no asker at all
   means discard, EOF means discard, and an asker that only ever says "show me"
   eventually gets "no". The console installs `sh_console_ask` **only for
   human-origin runs**, because an asker reachable from `--as-ai` would be a
   prompt a model can answer.
6. The desktop gets the same window (`hold_open`/`hold_key`), same three words,
   and closing it is a discard. Keys go to it exclusively while it is open.
7. `pctx.rollback_available` was hardcoded `0` long after M5 shipped snapshots.
   It now asks `snap_list_all()` for a **complete** snapshot (no DONE marker = no
   rollback), through one `sh_rollback_available()` shared by the gate and
   `policy check` — so the introspection builtin cannot drift from the gate.

**Two lessons the card could not have taught, both now comments in the code:**

- Rule 4c ("an AI write outside the jail is `NEEDS_HUMAN`") is **unreachable**:
  step 2d denies any outside path for *every* origin first, so `/etc/x` is a
  DENY before 4c is consulted. The acceptance line "an AI write outside
  `/workspace` gets `NEEDS_HUMAN`" therefore cannot be satisfied as written —
  the write is refused, which is stronger, and the test says *which rule* did it
  rather than claiming the wrong one.
- Taking a snapshot does not unhold an AI delete: "an AI may not delete without a
  human" is its own tier-1 rule, independent of undoability. What the snapshot
  changes is the fact the rollback rule is fed, and that is what
  `policy check … as judged: rollback=available` now shows.

---

## M8 — Promote/reject UX (console + desktop) · **M** · closes G4/G9 UX half

**Goal.** A human can see pending changes, read a diff, and decide — in both
front-ends, with the same engine underneath.

**Do.**
1. Console builtins: `pending` (list), `diff <id>`, `promote <id|all>`,
   `discard <id>`.
2. Desktop: a **pending-changes** window (6th *window*, not a 6th launcher app —
   see RULES §2.4 on the 5-slot launcher): rows = intent id, origin, summary;
   keys `p`/`d`/`enter`; a diff pane rendered with the 8×16 grid (green `+`,
   red `-`).
3. The window must show the AI-origin badge (a future UI-07 HUD uses the same
   flag).
4. Refuse to promote anything whose audit record failed `audit verify`.

**Acceptance.** Manual script in `docs/` + `tests/test_stage.c` covers the
API; GUI: add `gui_check.py` assertions for the pending window title bar and a
`+`/`-` diff row.

---

## Sequencing (do not reorder)

```
M1 (descriptor, pass-through)
  └─ M2 (policy, unit-tested, not yet enforced)
       └─ M4 (effects from the worker)
            └─ M3 (staging) ── M6 (audit chain)
                 └─ M5 (snapshot/rollback, PID 1)
                      └─ M7 (origin) ── M8 (promote UX)
```

Why: you cannot gate writes (M3) before you can decide (M2), and you cannot
decide without a descriptor (M1). Audit (M6) comes as soon as there are
decisions to record, because retro-fitting the chain into an existing log is
worse than starting it.
