# 04-SMALL-TASKS — tiny, self-contained, drop-in cards

For ChatGPT or a short session. Each is **≤ 1 hour** unless marked. One card at a
time; each states files, the exact change, and how to prove it.

---

## Docs & truthfulness

- **`S-DOC-01` — one-page architecture map.** New `source/docs/ARCHITECTURE.md`:
  a 40-line ASCII diagram of the layers (kernel → ksched/cgroups/eBPF →
  sembus → mem/tools/durable → agent/cortex → shell/desktop) with, for each
  arrow, the file that implements it. No prose beyond one line per box.
  *Prove:* every file named exists (`grep -L`).
- **`S-DOC-02` — "what runs where" table.** New `source/docs/PROCESS-MODEL.md`:
  which process exists at boot (PID 1, governor, desktop, aiosh, tool-exec per
  call, forked jobs), its uid, its cgroup class, its lifetime, what kills it.
  *Prove:* matches `boot/init.c` line-for-line (cite lines).
- **`S-DOC-03` — honest status paragraph.** Add to `source/README.md` a
  "What this is / What this is not" pair of 5-line lists. Must state: no
  policy engine, no rollback, no networking, no big model in the image.
  *Prove:* every "not" is still true (grep for the gap).
- **`S-DOC-04` — `ai_con.md` historical note.** Prefix the root file with:
  "Historical: describes the pre-C, Rust design phase (`ai_os_core/`). It is not
  the current architecture; see `context/`." *Prove:* one line, top of file.
- **`S-DOC-05` — boot log tour.** `source/docs/BOOT-LOG-TOUR.md`: take
  `boot-test2.log`, annotate every `[init]` line with what it proves. ~30 lines.
  *Prove:* each quoted line exists in the log.

## Shell & tools

- **`S-SH-01` — `which <cmd>` builtin.** `src/shell.c`: print `builtin`, `tool`,
  or `natural language` for a name. Uses the existing resolution order.
  *Prove:* `test_shell` +3 (`which calc` → tool, `which version` → builtin,
  `which summarise the workspace` → natural language).
- **`S-SH-02` — `tools` builtin.** List the registry: name, sandboxed or native,
  one-line description (the data is already in `tool_def`).
  *Prove:* `test_shell` +2 (exactly 8 rows; `calc` marked sandboxed).
- **`S-SH-03` — `history N`.** The terminal already keeps 16 entries; the shell
  builtin prints them. *Prove:* `test_shell` +2.
- **`S-SH-04` — `audit N` builtin.** Print the last N lines of `audit.log`
  (read-only, no parsing). *Prove:* `test_shell` +2 with a temp store.
- **`S-SH-05` — better error for unknown tools.** Today an unknown name falls
  into NL routing and may produce a confusing agent run. Detect
  "looks like a tool but is not registered" (contains `.` and no spaces) and
  print `unknown tool 'x.y' — try 'tools'`. *Prove:* `test_shell` +2.
- **`S-SH-06` — `help <builtin>`.** One-line usage for the named builtin from a
  static table (extract from the existing help string).
  *Prove:* `test_shell` +3.

## Tests that are cheap and valuable

- **`S-T-01` — policy-free determinism test for the router.**
  `tests/test_shroute.c`: 20 fixed phrases → assert the same route kind every
  run (scripted and micro cortex). Catches accidental corpus drift.
- **`S-T-02` — memory consolidation cap test.** `tests/test_mem.c` +4: fill past
  `EPISODIC_CAP`, consolidate, assert the cap holds and the newest survive.
- **`S-T-03` — durable torn-tail fuzz.** `tests/test_durable.c` +5: write a
  journal, truncate at 10 random offsets, replay, assert every prefix is either
  fully applied or cleanly dropped (never a half-record).
- **`S-T-04` — JSON round-trip fuzz.** `tests/test_json.c` +6: random nested
  objects → serialise → parse → compare structurally.

## Desktop (tiny)

- **`S-GUI-01` — clock in the bar** (subset of `UI-01`): only `HH:MM`,
  right-aligned before the job count. *Prove:* `test_fb` +2 on the formatter.
- **`S-GUI-02` — tooltips on launcher icons:** hover 600 ms → toast with the app
  name (the toast system exists). *Prove:* pure `hover_elapsed()` test.
- **`S-GUI-03` — boot splash timing line:** the splash already shows phases;
  add `boot #N` to one of them (the number is in `/data/aios/boots.jsonl`).
- **`S-GUI-04` — window titles show state:** terminal → `terminal [n jobs]`,
  files → `files · <dirname>`, notepad → `notepad · <file>*`. One `snprintf` per
  render in `refresh_app`; needs `aios_wm_set_title()` (~10 lines in `src/win.c`).
  *Prove:* `test_fb` +2 on the title strings.
- **`S-GUI-05` — cursor: text I-beam in the notepad/terminal.**
  `aios_wm_set_cursor_shape()` with two procedural 32×32 images (arrow, ibeam)
  drawn like `aios_img_cursor_fallback()`. *Prove:* `test_fb` +2 (shape switch
  on focus change).

## Build & repo

- **`S-B-01` — `make help`.** A phony target printing every target with one line
  (host + boot Makefiles). *Prove:* `make help` exits 0 and lists 8+ targets.
- **`S-B-02` — `make check-size`.** Print the current initramfs size vs the cap
  without building. *Prove:* exits 1 when the file is missing.
- **`S-B-03` — `.editorconfig`.** 4 spaces, LF, UTF-8, C + Markdown rules.
- **`S-B-04` — `scripts/` for the two missing M10 scripts.** `docs/ACCEPTANCE-M10.md`
  cites `scripts/m10-spike.sh` and `scripts/m10-bochs-probe.sh`; either commit
  reconstructions (they are simple QEMU + module-load scripts) or rewrite the
  doc. Prefer **rewrite the doc** unless you still have the originals.
- **`S-B-05` — `tools/lint.sh`.** `gcc -fsyntax-only` over `src/ apps/` with the
  v1.0 warning flags, `-Werror`, no linking. Fast feedback before a full build.

## Kernel / boot (small but careful)

- **`S-K-01` — supervisor for the governor.** `boot/init.c`: remember
  `gov_pid`, respawn with backoff in the reap loop, log
  `[init] governor died (rc=N) - respawning`. *Prove:* kill the governor in a
  booted image (`kill <pid>` from the emergency shell) and see the log line.
  (This is `C1` in the master list — small but high value.)
- **`S-K-02` — `aios.selftest` default off for release.** Keep it on when
  `aios.mode=test`. *Prove:* boot without the flag → no selftest phase, console
  comes up faster; CI still passes.
- **`S-K-03` — `/proc/aios` format test.** A parser test in `tests/test_kernel.c`
  that feeds the documented format (`pid\tCLASS\twakeups\tcomm`) into the
  governor's parser, so a format change cannot silently break autopilot.
- **`S-K-04` — boot time measurement.** Print `[init] boot: N ms to console` and
  record it in `docs/PERFORMANCE.md`. *Prove:* the line appears once per boot.

## Micro-LM

- **`S-LM-01` — `lm explain <text>`.** Show the top-3 class scores (the
  classifier already computes them; expose them). *Prove:* `test_autopilot` or a
  new `test_microlm` +3.
- **`S-LM-02` — corpus growth without drift.** Add 12 phrases per class to
  `tools/microlm_gen.py`, regenerate, and keep `aios_microlm_selftest()` green
  (18 phrases today). *Prove:* `lm selftest` reports 0 failures with the larger
  baked set.
- **`S-LM-03` — document the seam.** `src/llmbridge.h` + `src/cortex.c`: a
  header comment that states exactly what a "big model" must implement to plug
  in (one function, one JSON in, one JSON out) and that it currently cannot run
  in the image. *Prove:* comment only; no code change.
