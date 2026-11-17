# Name: DESKTOP-APP
**Use when:** adding a window/app to `aios-desktop` (M10 GUI).

---

Add a new application window to the AI-OS desktop. Read
`context/CONTEXT.agent.md`, then `source/apps/aios-desktop.c` (the five existing
windows, `open_app`, `refresh_app`, `bar_click`, the draw/input loop),
`source/include/aios/fb.h`, `source/include/aios/win.h`,
`source/include/aios/input.h`, and `source/desktop/assets/ASSETS.md`.

Hard rules:
- Use `aios-fb` + `aios-wm` APIs only. **Never** touch `/dev/fb0` directly.
- No new dependency, no dynamic allocation per frame, no blocking calls on the
  UI path (the desktop is a single-threaded damage-driven loop).
- Every asset must have a **procedural fallback** (ASSETS.md contract), so the
  OS still boots with no PNGs present.
- Anything the window can *change* (files, memory, jobs) must go through the
  same `sh_exec()` engine the serial console uses — the desktop must not get a
  private back door around the mediator/sandbox.
- Text must fit 8×16 cells; assume 1280×800; degrade at smaller sizes.
- Add the window to `APP_META` (title + icon), and give it a keyboard path — the
  pointer alone is not enough for accessibility.

Deliver:
1. The new `app_kind`, its state struct, `open_app`/`refresh_app` handlers, and
   the draw + input handlers.
2. A short note on what happens if the window's data source fails (the desktop
   must show an empty state, never crash — PID 1 gives up after 5 crashes).
3. `test_fb` additions if you add drawing primitives, or a note on why no test
   is possible.
4. Proof: `bash boot/boot-gui-test.sh` still reaches `AIOS-DESKTOP-OK` and the
   `6*7 → 42` typing check still passes; include a screendump check for your
   window's title bar.
