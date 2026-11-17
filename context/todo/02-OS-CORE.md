# 02-OS-CORE — the mediator (M11): make the constitution true

These are **not** ChatGPT-sized. They need judgement, tests and an
understanding of the constitution. Read `context/02-WHAT-IS-MISSING.md` G1–G9
first; each card says which gap it closes.

Guiding sentence for all of them: *the OS must be able to answer, for every
effect the AI caused: what was proposed, what was allowed, what actually
happened, and how do we undo it.*

---

## M1 — Action Descriptor (`aios_action`) · **M** · closes G2

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

## M2 — `policy_eval()` — fail-closed law table · **M** · closes G1

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

## M3 — Staging, diff, promote/discard · **L** · closes G4

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

## M4 — `tool-exec` returns an effects document · **M** · closes G3

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

## M5 — Snapshots and rollback · **L** · closes G5

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

## M6 — Hash-chained audit + `audit verify` · **M** · closes G8

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

## M7 — Origin tagging (human vs AI) · **M** · closes G6

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
