# M8 — Promote / reject UX: the manual script

Two front-ends, one engine. Everything below is done twice where it matters:
once in the console (`aiosh`) and once in the desktop window. If they ever
disagree, the window is wrong — it calls the same builtins a person types.

**What this milestone is actually for.** Before M8 a person could be shown a
change, say yes, and have nothing happen. The desktop answered every held action
with `DISCARD` *before* opening the window that asked about it, then toasted
"re-run the command to apply it" — advice that would have been wrong anyway,
because a re-run produces a different change id. So the two claims to check are:

1. a person can find out what is about to happen, and
2. their answer reaches the change they were shown — yes applies it, no applies
   nothing, and neither of them silently drops it.

---

## 0. Setup

```sh
cd source
make -j8 aiosh aios-desktop
export JAIL=/tmp/m8-manual/jail STORE=/tmp/m8-manual/store
rm -rf /tmp/m8-manual && mkdir -p "$JAIL" "$STORE"
A() { ./aiosh --jail "$JAIL" --store "$STORE" -c "$1"; }
```

`aiosh` treats a *human* `fs.write` as a direct write; only an AI-originated
write is staged. The console has no origin switch, so stage with the scripted
cortex, which stages:

```sh
./aiosh --jail "$JAIL" --store "$STORE" -c 'write alpha to one.txt'
./aiosh --jail "$JAIL" --store "$STORE" -c 'write beta to two.txt'
```

Each command opens its own session, so that is **two** pending changes — the M7.5
rule that a staged session belongs to the command that opened it.

---

## 1. Console: see what is pending

```sh
A pending
```

Expect a list where every row has an id, a summary of what it does, and the
audit sequence that records it being offered:

```
2 pending changes
  [16011410610707404832-1]  add two.txt                     seq 4
  [7891506204226969561-1]   add one.txt                     seq 2
  diff <id>      read one change
  promote <id>   apply it
  discard <id>   throw it away
```

The id is the only handle `promote`/`discard`/`diff` accept. **Check:** the id
you copy from here is the one that works below. (This is the whole point of M7.5
— an id you can read and type.)

## 2. Console: read one change

```sh
A 'diff 7891506204226969561-1'
```

```
change 7891506204226969561-1
  + one.txt                              add      +6 bytes  +1/-0 lines
  1 file: 1 added, 0 modified, 0 deleted (net +6 bytes)
```

**Check the failure modes, too — a usage message is part of the UX:**

```sh
A diff                 # -> usage: diff <id>   (`pending` lists the ids)
A 'diff promote'       # -> no pending change with id `promote`
```

A verb where an id belongs is refused rather than guessed. That is D34's rule
applied to the newest command.

## 3. Console: say yes to ONE change

```sh
A 'promote 7891506204226969561-1'
A pending                       # the other change is still there, still named
```

**Check:** the jail contains `one.txt` and not `two.txt`.

## 4. Console: say yes to all of them, deliberately

`all` is a **word**, not a wildcard and not a default. A bare `promote` with
several pending changes still refuses, and names them (see §7).

```sh
A 'promote all'
```

```
promoting 2 changes
  [16011410610707404832-1]  promoted 1 file
  ...
2 changes applied; nothing left pending
```

**Check the counting is honest.** A staged session can declare an effect without
holding any bytes; such a session reports `nothing staged to apply`, and is *not*
counted as applied. Try it:

```sh
A 'fs.write path=ghost.txt content=x'   # declares, stages nothing
A 'promote all'
```

## 5. Desktop: the pending window

This is a **sixth window**, not a sixth launcher app — RULES §2.4 pins the
launcher at five slots (`w-440+i*86`), and `i=5` would land off a 1280 screen.

1. Boot the desktop (or `run-gui.sh`). With something staged, the launcher bar
   shows an amber `N pending` badge immediately left of the app slots.
2. Click the badge. A window titled `pending changes` opens over the others.
3. Rows are: id (tail), what it does. An `AI` badge marks a change whose proposal
   is on the audit record — read from the record, not from the directory name.
4. `up`/`down` move the selection; the diff pane below re-reads for the selected
   change.
5. `p` promotes the selected change, `d` discards it. Both go through the **same
   builtin** the console uses, so the audit gate and its refusals apply unchanged.
6. `esc` or the close box closes the list. Closing a **list** is not a decision:
   nothing is promoted, nothing is discarded, the changes are still pending.

**Check:** after `p`, the toast reads `promoted N file into the jail`, and the
row is gone from the list. After `d`, the toast reads `discarded; nothing was
run`, and the jail is unchanged. Closing the **hold** window is different from
closing the list: the hold window says `closing this window = discard (nothing
runs)`, and the shell is told so (`sh_resume(DISCARD)`) rather than the bus
claiming a decision that never reached the engine.

## 6. Desktop: answering a held action (the M7 hole)

A destructive AI proposal produces `NEEDS_HUMAN`, which asks a person. On the
desktop the asker cannot block — the shell runs on the desktop's own event-loop
thread — so it answers `DEFER` and the shell **holds the action**.

1. Trigger a gated action (an AI-originated delete is the canonical one; the
   console's `_describe delete <file>` test seam does exactly this through the
   real gate).
2. The `confirm a held action` window opens, showing what, why, and who proposed
   it.
3. `p` answers yes, `d` answers no, `s` re-shows the detail.

**Check:** the answer now reaches the action. Before M8, `p` printed "re-run the
command to apply it" and applied nothing. What you should see instead is the
result of `sh_resume()`: either what ran, or an honest refusal naming the reason.

> **Known gap, stated rather than papered over:** `aiosh` has no execution path
> for every action kind a hold can carry (a `FS_DELETE` proposal is recorded and
> answered, but nothing deletes yet — `fs.delete` is not a tool). The toast says
> `you promoted a FS_DELETE proposal, but aiosh has no execution path for that
> kind yet -- it was recorded, not run`. That is a missing feature; it is not a
> safety property, and it does not pretend the answer was ignored.

## 7. The gate: a promote that cannot justify itself

```sh
# a) the chain does not verify
printf 'x' >> "$STORE/audit.log"        # or edit one byte in any record
A 'promote all'
# -> refusing to promote <id>: the audit chain does not verify
#    (<verdict> at seq N). What the record says about this change is exactly
#    what is in doubt.

# b) the chain is intact and says nothing about this change
#    (delete the line whose "stage" field names the id)
A 'promote all'
# -> refusing to promote <id>: no audit record names it. A change whose
#    proposal was never recorded cannot be shown afterwards to have been
#    approved. Discard it and stage it again.
```

**Check both.** Case (a) alone is the near-vacuous gate: a chain can verify
perfectly while containing nothing about the change in front of you. Case (b) is
the half that closes it. In both cases **the jail must be untouched** —
`find "$JAIL" -type f` unchanged — and `promote all` must say how far it got:

```
0 changes applied; stopped at the first refusal -- the rest were not touched
```

`promote all` also has to fail loudly and usefully. It used to print
`error: error` here, because a shell builtin's `1` was being rendered through
`aios_status_str()`, which only knows `aios_status` values: the result was
"unknown" behind the front-end's `error: ` prefix. The reason you read now is the
actual refusal.

---

## 8. What this milestone still does not do

Stated here so it is not mistaken for done:

- **The shown diff is not bound to the applied bytes.** A store writer can change
  `upper/` between `staged` and `promote` and the record still verifies. Closing
  this needs a content hash in the manifest (D35). The gate proves *a record names
  this change*, not *these are the bytes you saw*.
- **The origin tag is still a voluntary argument** in a one-uid process, and the
  audit writer is one of the components it audits. The chain is rewriteable
  wholesale by anyone who can write the store.
- **`fs.write path=..` declares an effect and stages no bytes**, so a promote of
  such a session has nothing to move. It is now reported honestly rather than
  counted.
- **The pending window is opened by a click**, not by a key binding, and it does
  not pop itself open when a change arrives. A change that waits silently is a
  change a person may never look at.
