# 03-GUI-TASKS — desktop & UI cards (delegatable to ChatGPT)

**How to delegate:** give ChatGPT **one card at a time**, plus
`context/RULES.md` and the prompt `context/prompts/07-desktop-app.md`.
Every card is written to be self-contained: files, API, steps, acceptance.

**Facts every card assumes** (verified in `apps/aios-desktop.c`,
`include/aios/fb.h`, `include/aios/win.h`, `include/aios/input.h`):

- Windows draw in **canvas** coords; `aios_win_cw()×aios_win_ch()`; title is
  24 px (`AIOS_TITLE_H`).
- Text = 8×16 cells; width `8*strlen(s)`; `aios_fb_text_w(s)`.
- Colours `0xAARRGGBB`, `AIOS_RGB(r,g,b)`; bg `0` = transparent.
- Per-window state: `calloc` in `open_app()`, pass to `aios_wm_open(...)`, get
  back with `aios_win_user(w)`.
- Keys: ASCII 32..126 or `AIOS_KEY_*`; backspace `0x08`/`0x7f`.
  **`mods` (ctrl/alt) is currently dropped by `main`** — see `UI-03`.
- Adding an app kind touches: `app_kind`, `APP_META`, `open_app()`,
  `refresh_app()`, `key_to_app()`, `g_icons[]`, and the launcher loop.
- **The launcher fits exactly 5 apps** (`w - 440 + i*86`, 80 px wide).
  A 6th app needs `APP-03` first.
- CI (`boot-gui-test.sh` + `tools/gui_check.py`) asserts 5 title bars at
  `(40,40) (780,40) (40,510) (440,510) (840,510)`, a gradient, the accent bar,
  the cursor, and `6*7 → 42`. Moving/resizing those windows breaks CI.
- Everything a window *changes* must go through `sh_exec(d->sh, "...", &r)`.

---

## APP-01 — **notepad**: a real text editor window · **M** (the flagship small app)

**Goal.** A usable plain-text editor window: type, edit, save, open, with a
visible filename and a dirty marker. This is the "notepad" the project needs.

**Files.** `apps/aios-desktop.c` (new `APP_NOTEPAD` + state struct),
`source/desktop/assets/ASSETS.md` (icon slot), `tests/test_fb.c` (geometry).

**Do.**
1. State:
```c
#define NP_MAX_BYTES  (64 * 1024)
#define NP_MAX_LINES  512
typedef struct {
    char path[256];          /* "" = untitled                        */
    char lines[NP_MAX_LINES][128];
    int  n_lines, cur_line, cur_col;   /* cursor                      */
    int  scroll;                       /* first visible line          */
    int  dirty;                        /* 1 = unsaved changes          */
    char status[96];                   /* last message (saved/failed)  */
} np_buf;
```
   (64 KiB / 512 lines is deliberate: it fits the 8 MiB store and the VM's RAM;
   do not make it unbounded.)
2. `np_open_file(np_buf *, const char *jail_path)` — read through
   `aios_read_file()` (`aios/types.h`) from the **workspace jail**, split on
   `\n`, truncate a line at 127 chars, set `dirty=0`.
3. `np_save(desktop *d, np_buf *)` — write the buffer with
   `aios_write_file()` into the jail. **Until the mediator exists (M11), this is
   the only direct write in the GUI** — put a `/* MEDIATOR: route through stage
   when M3 lands */` comment on the line, and keep it inside the jail path only.
   Report failures in `status[]` (never crash).
4. Rendering (`np_render`): title shows `path or "untitled"` + `*` when dirty;
   body = lines, current line highlighted, caret drawn as a 7×15 filled rect at
   `6 + cur_col*8`, `2 + (cur_line-scroll)*16`; footer =
   `status` + `" ^S save  ^O open  ^Q close"` (even though those keys land in
   `UI-03`, print the hint only after `UI-03` ships — until then print
   `"esc = menu"`).
5. Keys (`np_key`): printable → insert at cursor (split lines on `\n` when out
   of space; keep it simple: append, do not insert in the middle for v1);
   backspace → delete char, join lines when col == 0; `UP/DOWN/LEFT/RIGHT,
   HOME/END, PGUP/PGDN` → move + scroll; `\n` → new line; **no undo yet**
   (that is `APP-06`). Mark `dirty` and `aios_win_dirty(w)`.
6. Open/close: `open_app()` case `APP_NOTEPAD` with a 520×420 window at
   `x=200,y=120`; do **not** add it to the launcher (5 slots are full — reach it
   with a key or a builtin; see below).
7. Reachability without a 6th launcher slot: add a shell builtin `edit [FILE]`
   to `src/shell.c` that... **no** — the desktop cannot be told to open a window
   from the shell today. Instead open it on **`F2`** in `key_to_app()` (a
   global shortcut handled before window dispatch) and print the hint in the
   launcher placeholder text.
8. Icon: add `icon-notepad` to `ASSETS.md` and a case in
   `aios_img_icon_fallback()` (`src/fb.c`) drawing a simple page + pen with
   `aios_win_*` primitives (look at the existing fallbacks for the pattern).

**Acceptance.**
- `make` clean; `make test` 11/11 (add 6 geometry checks: canvas size, caret
  position for `(0,0)` and `(3,5)`, scroll clamp, dirty marker in the title).
- Manual: `F2` opens it; type 3 lines; the title gains `*`; save writes the file
  into `/workspace` (verify with the existing files window or `fs.read` from the
  terminal); reopen shows the same text.
- `boot-gui-test.sh` still passes (you added a window that is **not** opened at
  boot — if you open it at boot, CI's 5-titlebar check fails).

**Do not:** add a 6th launcher icon, use `printf` debugging, allocate per frame,
or save outside the jail.

---

## APP-02 — Files window: open a file → read-only viewer · **M**

**Goal.** In the Files window, `enter` on a file shows its contents (read-only)
instead of doing nothing (today `files_key()` only handles directories).

**Files.** `apps/aios-desktop.c` (`files_buf`, `files_key`, `files_render`).

**Do.**
1. Extend `files_buf` with `char preview[NP_LINES?]` — do **not** reuse the
   notepad struct; keep a simple `char preview[24][80]; int preview_n; int
   preview_on;`.
2. On `enter` with `!is_dir[sel]`: build the jail path, `aios_read_file()` with
   a 32 KiB cap, split into 24 lines of 79 chars, `preview_on = 1`.
3. `files_render`: when `preview_on`, draw a header (`-- path --`) and the lines;
   `esc` (`0x1b`) leaves the preview.
4. Show `…` on a truncated line and a footer `"(read-only; MEDIATOR will route
   edits through staging)"`.
5. Do not allow editing here — that is the notepad's job.

**Acceptance.** `test_fb` +4 checks (preview on/off state transitions on a
`files_buf` with a synthetic file); manual: open `notes.txt` from the files
window, see its text, `esc` returns to the list.

---

## APP-03 — Launcher: room for more than 5 apps · **M**

**Goal.** The launcher bar must not silently overflow at 6 apps.
**This card must land before any new launcher app (notepad icon, settings, etc.).**

**Files.** `apps/aios-desktop.c` (`bar_render`, `bar_click`, the `w - 440 + i*86`
arithmetic).

**Do.**
1. Compute the slot pitch from the space available: reserve the NL input area
   (x from 150 to `w - RESERVE`), then `pitch = min(86, (w - RESERVE - 150) /
   APP_COUNT)`, min 56 px. Slot width = `pitch - 6`.
2. If `pitch < 56`, wrap to **two rows** of 32 px inside `BAR_H` (or raise
   `BAR_H` to 92 — but then re-check the gradient/accent assertions).
3. Keep labels: at pitch < 72 print **icons only**, and put the app name in a
   tooltip-style toast on hover.
4. Update `bar_click()` to use the same arithmetic (factor it into one helper
   `bar_slot_rect(i, w, h, &out)` — never duplicate the formula).
5. `gui_check.py`: the `bar_accent` check looks for the accent line; make sure
   the bar geometry change does not move that line, or update the check.

**Acceptance.** `test_fb` +3 checks on `bar_slot_rect` (5 apps at 1280 wide,
9 apps at 1280 wide, 9 apps at 1024 wide → all inside the bar, no overlap).
Manual: temporarily set `APP_COUNT` to 7 and confirm nothing is clipped.

---

## UI-01 — Status bar: clock, cortex, budgets, jobs · **S**

**Files.** `apps/aios-desktop.c` (`bar_render`).

**Do.** Replace the current single `"cortex: %s  jobs: %zu"` with a
right-aligned group: `HH:MM` (from `time()`; `aios_now_ms()`/1000),
`cortex=<mode>`, `jobs=<n>`, and the two autopilot budgets read from
`/var/lib/aios/autopilot-state.json` (`json_parse_file`) as `cpu 50/25`.
Cache the file read: refresh only every ~25 frames (it is already the idle
tick pattern in `main`).

**Acceptance.** No per-frame file read (measure: the idle loop must not stat the
store every frame); `test_fb` +2 checks on the formatting helper
(`fmt_status_line(char *out, size_t)` — pure, easy to test).

---

## UI-02 — Window resize from the bottom-right grip · **M**

**Files.** `src/win.c`, `include/aios/win.h`, `apps/aios-desktop.c`.

**Do.**
1. `aios_wm_resize(aios_wm *m, aios_win *w, int new_w, int new_h)` — clamp to
   `>= 120×60` and inside the screen, reallocate the canvas, mark all damaged.
2. Draw a 3-dot grip in the bottom-right 12×12 of each window (`src/win.c`,
   compositor).
3. In `main`'s mouse handler: on left-press inside the grip → remember
   `resizing`; on move → resize; on release → stop. Do the same state machine
   that title-drag already uses.
4. Windows must re-render correctly at the new size (all `*_render` functions
   compute from `aios_win_cw/ch`, so mostly free — check the ones with
   hardcoded `440`, `200`, `510`).

**Acceptance.** `test_fb` +5 checks (clamp min/max, canvas realloc, content
still drawn after resize); manual resize of the terminal and notepad; CI green
(do not resize windows at boot).

---

## UI-03 — Thread modifier keys through to apps (Ctrl+S etc.) · **S**

**Goal.** `aios_event.mods` exists but `main` drops it, so no shortcut can work.
**This unblocks save/open/close in the notepad.**

**Files.** `apps/aios-desktop.c` (`key_to_app`, `nl_key`, every `*_key`),
`include/aios/input.h` (no change needed — `mods` is already there).

**Do.**
1. Change `key_to_app(desktop *d, int key, int mods)` and the per-app key
   handlers to take `mods` (`1 shift | 2 ctrl | 4 alt`).
2. Add a tiny helper `int is_ctrl(int mods)`.
3. Implement the global set: `Ctrl+S` save (notepad), `Ctrl+O` open, `Ctrl+Q`
   close window (with a "discard changes?" prompt if dirty), `Ctrl+L` focus the
   launcher input.
4. Document the shortcut list in `aios-desktop.c`'s header comment and in the
   notepad footer.

**Acceptance.** `test_fb` +4 checks on the pure mapping helper
(`map_shortcut(key, mods) → action enum`); manual: Ctrl+S in the notepad writes
the file, Ctrl+Q closes with a prompt.

---

## UI-04 — Clipboard (copy / paste) · **M**

**Scope.** One in-desktop clipboard shared by notepad, terminal and launcher.
**Not** an X11/Wayland clipboard — there is no display server, by design.

**Files.** `apps/aios-desktop.c` (a `char clip[4096]` in `desktop`, plus
`clip_copy()`/`clip_paste_into(char *dst, size_t cap, ...)`).

**Do.**
1. `Ctrl+C` copies the selected line (notepad) or the visible selection
   (terminal: current line for v1).
2. `Ctrl+V` pastes at the cursor in the active window; newlines in the notepad
   become new lines; in the launcher/terminal, newlines become spaces.
3. Show a `copied 42 bytes` toast.

**Acceptance.** `test_fb` +5 checks on `clip_paste_into` (empty clipboard, 4 KiB
cap, newline handling per target); manual round-trip notepad → terminal.

---

## UI-05 — Minimize + taskbar · **L**

**Goal.** Hidden windows can be brought back; today closing is the only way and
several windows are unreachable once closed (`open_app` is only called at boot,
`aios_desktop.c` ~line 860 — nothing reopens them).

**Do.**
1. `minimized[]` flags per app slot; `_` button in the title bar next to `X`
   (`src/win.c` compositor + `aios_win_min_box()` in the header, mirroring
   `aios_win_close_box()`).
2. A taskbar strip (left of the launcher) with one 24×18 chip per window; click
   restores + focuses.
3. `Enter`/click on a launcher icon must **reopen** a closed window
   (`open_app()` if `win == NULL`) — this fixes a real usability bug.

**Acceptance.** `test_fb` +6 (minimize hides from `aios_wm_hit`, restore
focuses, `open_app` twice does not double-allocate); manual: close every
window, reopen each from the launcher/taskbar.

---

## UI-06 — Free window state on close (stop the leak) · **S**

**Problem.** `main` sets `apps[i].win = NULL; aios_wm_close(...)` but the
`term_buf`/`files_buf`/`agents_buf`/`mem_buf` allocated in `open_app()` is never
freed. With `UI-05`'s reopen this leaks every close/reopen cycle.

**Do.** Add `static void free_app_state(desktop *d, app_kind k)` switching on
`kind` (`free(aios_win_user(w))`), call it in the close path **before**
`aios_wm_close`, and in `open_app()` assert the slot is empty.

**Acceptance.** Manual loop: close/reopen the notepad 200 times, watch RSS stay
flat (`/proc/self/status VmRSS` printed by a temporary debug builtin — remove
it after); `test_fb` +2 (state pointer is NULL after close).

---

## UI-07 — Focus ring + "AI is driving" HUD · **M**

**Goal.** Groundwork for GUI automation: a visible, unambiguous indicator of who
owns input. The constitution forbids "cursor fights" and ambiguous control —
this is the first half (the arbiter itself is M11/`G7`).

**Files.** `src/win.c` (focus ring already exists — extend it),
`apps/aios-desktop.c` (HUD), `include/aios/input.h` (an `aios_ev_source` enum).

**Do.**
1. Add `AIOS_SRC_HUMAN` / `AIOS_SRC_AI` to `aios_event` (defaults to HUMAN
   everywhere today; nothing sets AI yet — that is the point: the plumbing must
   exist before an AI input path is ever added).
2. HUD: a 200×24 pill at top-centre reading `AI is driving — press Esc to take
   over`, drawn only when any event in the last 2 s had `AIOS_SRC_AI`.
3. `Esc` from a human immediately sets a `human_override_until` timestamp and
   the desktop **drops** any AI-source event for 5 s (`RULES` §2.6 #4: no new
   authority — this only takes authority away).

**Acceptance.** `test_fb` +4 on the pure predicate
`hud_visible(now, last_ai_ms, override_until)`; the HUD never renders in normal
use (no AI source yet) — assert exactly that, so CI screenshots stay stable.

---

## APP-04 — Notepad: line numbers, wrap, scrollbar · **M**

Extends `APP-01`. Gutter of 4 chars (`%4d `), wrap at `canvas_width/8 - 4`,
a 6 px scrollbar on the right when `n_lines > visible`. **Acceptance:** `test_fb`
+5 on the wrap helper (pure: `wrap_line(const char *in, int cols, char out[][…])`).

## APP-05 — Notepad: search (Ctrl+F) · **M**

Extends `APP-01`/`UI-03`. `Ctrl+F` opens a one-line prompt at the bottom;
`Enter` jumps to the next match (wrap-around), `Esc` closes; highlight the match
with a 7-px accent underline. **Acceptance:** `test_fb` +6 on the pure finder
(`np_find(np_buf*, const char *needle, int from_line, int *line, int *col)`),
including case-insensitivity and no-match.

## APP-06 — Notepad: undo/redo · **L**

A ring of 32 snapshots of the *changed lines only* (not the whole buffer — 64 KiB
× 32 would be 2 MB). `Ctrl+Z` / `Ctrl+Y`. **Acceptance:** `test_fb` +8
(insert/delete/undo/redo/undo-past-start/redo-past-end).

## APP-07 — Notepad: syntax highlight for `.aiosh` and `.md` · **L**

Only after `APP-04`. A tiny tokeniser: comments (`#`), strings, keywords
(`spawn jobs wait kill remember recall`), and `$vars`. Render as colour runs on
the 8×16 grid. **Acceptance:** pure `tok_classify(line, pos)` tests; a screenshot
check in `gui_check.py` for two colours in a known file.
