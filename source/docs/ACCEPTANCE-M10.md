# M10 `desktop` — acceptance record (1.1.0-m10)

Every claim below is backed by a command that ran green in this tree.

## 1. Kernel evidence first (the spike)

- The graphics path was chosen by booting, not by documentation.
  a spike script loaded a candidate module stack on the stock
  `linux-virt 6.6.142` kernel under QEMU with display devices attached:
  - **virtio-gpu**: all modules load, `/dev/fb0` appears
    (`/proc/fb: 0 virtio_gpudrmfb`, 1280x800), and `/dev/input/event*`
    works (virtio keyboard/tablet). BUT the scanout never reaches the
    QEMU display surface: `screendump` stays 720x400 black even after
    the guest paints `/dev/fb0` — virtio-gpu fbdev uploads pixels
    through the DRM command queue (`resource_flush`), which does not
    update the QEMU surface without a display backend.
  - **bochs-display** (`bochs.ko` from the same kernel package): guest
    mmap writes are a shared PCI BAR — the QEMU surface resizes to the
    guest mode (1280x800) and shows pixels immediately.
    (The spike scripts themselves are not committed in this tree; the
    decision they produced is what shipped.)
- Decision: ship **bochs-display** as the QEMU target; `linux-virt`
  stays (no kernel rebuild, no linux-lts swap needed).

## 2. What ships

- 18 gzipped kernel modules (`boot/dl/modules-gpu/`, ~845 KB) loaded in
  a proven order by `aios-init`: fb helpers -> fb core (`fbdev`,
  `fb_io_fops`, `fb`) -> DRM (`drm`, `drm_kms_helper`,
  `drm_shmem_helper`, `ttm`, `drm_ttm_helper`, `drm_vram_helper`,
  `bochs`) -> input (`evdev`, `virtio_input`).
- `aios-fb` (`src/fb.c`): double buffer, damage-rect merging, 32bpp
  blits, 8x16 glyphs from the generated DejaVu Sans Mono table
  (`tools/gen_font.py` -> `src/font8x16.h`, committed), `.aiosimg` loader, procedural
  fallbacks for every ASSETS.md asset (wallpaper, logo, 8 icons,
  cursor).
- Window core (`src/win.c`): z-order, focus, title bars with close
  boxes, hit testing, damage-driven compositing, cursor sprite.
- Input (`src/input.c`): raw evdev keyboard (US keymap, modifiers,
  history-capable line editor codes) + absolute tablet pointer;
  kernel autorepeat is filtered (the OS does its own); unmapped keys
  never emit events (raw evdev codes are not ASCII).
- `aios-desktop` (`apps/aios-desktop.c`): wallpaper, launcher bar whose
  input box runs the same aiosh engine as the serial console (micro-LM
  cortex), terminal / files / agents / memory / autopilot windows,
  sembus toasts. CI marker: `AIOS-DESKTOP-OK w=1280 h=800`.
- Boot integration (`boot/init.c`): GUI phase after the kernel bridge
  (gunzip + insmod, count reported), boot splash with phase updates,
  and a dual front-end respawn loop — desktop on `/dev/fb0`, aiosh on
  serial — with five-crash desktop -> text fallback and `aios.gui=0`
  to skip the GPU stack entirely.

## 3. Size cap — the documented raise

The M9 guard pinned the cap to the m8 shipped image (1,642,932). M10
raises it once, explicitly: the GPU/input module stack ships ONCE in
the initramfs and the desktop binary joins it. New cap =
**2,605,888 bytes** (first M10 image 2,474,816 + 128 KB headroom),
pinned in `boot/.m10-size-cap`; the guard prints the whole history
(m6 5.0 MB -> m7 2.29 MB -> m8 1.64 MB -> m9 = m8 actual -> M10 raise).

## 4. Test evidence (all green in this tree)

- **Host suites**: `make test` — 11/11 suites, including the new
  `test_fb` (89 checks: clipping, damage union, glyph ink, `.aiosimg`
  round-trip + rejection of bad magic/truncation/missing files, all 8
  icon fallbacks, wallpaper gradient, logo transparency, cursor sprite,
  compositor z-order/focus/hit/close-box/wallpaper-restore, evdev
  headless mode).
- **In-guest selftest**: the boot selftest runs the same suites inside
  the OS — `SELFTEST test_fb: PASS (PASS: 89 checks)` on both boots.
- **Two-boot durable regression** (`boot/boot-test.sh`): boot 1 writes
  durable state, boot 2 replays it (`boot #2`, journal entries
  replayed, AIOS-BOOT-OK PASS) — M8's proof still holds with the GUI
  code in the image.
- **GUI boot proof** (`boot/boot-gui-test.sh`): boots with
  `bochs-display` + virtio keyboard/tablet, waits for
  `AIOS-DESKTOP-OK` (12 s), then:
  - screendump 1 + pixel assertions: wallpaper vertical gradient
    (diff=257), launcher accent line (27 green hits), title-bar ink on
    all five windows, cursor sprite (3000 px);
  - types `6*7` + Enter **through the virtio keyboard** and screendumps
    again: the terminal region changes and the aiosh answer appears
    (`has_42`: 29 changed px). Screenshots saved to
    `download/m10-boot-shots/desktop-idle.png` and `desktop-67.png` (written
    by `boot-gui-test.sh` when it runs) —
    the terminal visibly shows `aiosh> 6*7` and the answer `42`.
- **Graceful fallbacks verified**: headless boot without a display
  device loads the stack (18/18), finds no `/dev/fb0`, logs
  `no /dev/fb0 - text console only` and continues exactly like M1-M9.

## 5. Asset contract status

No artist PNGs were supplied for this release, so every asset rendered
through its procedural fallback (by design — see
`desktop/assets/ASSETS.md`). Dropping correctly-sized PNGs into
`desktop/assets/` and rebuilding converts them via
`tools/img2aiosimg.py`; no C changes are needed.
