# M10 `desktop` — artwork drop spec

The M10 desktop draws everything procedurally EXCEPT the art listed
below. This file is the contract between the OS and the artist: exact
folder, exact file names, exact pixel sizes, formats.

## Where the files go

    aios/desktop/assets/          <- the drop folder (committed to git)
    ├── wallpaper-default.png
    ├── logo-boot.png
    ├── logo-small.png
    ├── icon-terminal.png
    ├── icon-files.png
    ├── icon-agent.png
    ├── icon-memory.png
    ├── icon-autopilot.png
    ├── icon-settings.png
    └── cursor-default.png

## How to hand them over

Upload the files in this chat, named EXACTLY as below (lowercase,
hyphens). They will be placed into `aios/desktop/assets/`, committed,
and wired into the build. Missing files are non-blocking: the desktop
ships a procedural fallback for every asset, so artwork never holds up
a build.

## Formats

- **PNG** (RGB or RGBA), sRGB, non-interlaced.
- Icons / logo / cursor: **transparent background** (RGBA).
- Keep files small: wallpaper <= 500 KB, everything else <= 150 KB.
- **BMP** (24-bit, uncompressed) is accepted as a fallback if PNG is
  inconvenient — same names, `.bmp` extension.
- At build time a Python step converts each image to a packed raw-RGBA
  runtime format (`*.aiosimg`) so the C side stays zero-dependency —
  that format is internal; you never see or need it.
- Exact pixel sizes matter (the framebuffer is not DPI-aware). If a
  size differs we resize at build time (nearest-neighbour), but
  matching exactly gives the best result.

## Required assets

| #  | file name               | what it is                              | size (px) | notes                                                        |
|----|-------------------------|-----------------------------------------|-----------|--------------------------------------------------------------|
| 1  | `wallpaper-default.png` | desktop background                      | 1024x768  | flat/gradient art reads best on a framebuffer; avoid fine detail |
| 2  | `logo-boot.png`         | boot splash mark (drawn centered)       | 512x512   | transparent background, square                                |
| 3  | `logo-small.png`        | small mark: title bar, about box        | 64x64     | transparent                                                   |
| 4  | `icon-terminal.png`     | aiosh terminal app icon                 | 64x64     | transparent                                                   |
| 5  | `icon-files.png`        | files app icon                          | 64x64     | transparent                                                   |
| 6  | `icon-agent.png`        | agents/jobs monitor icon                | 64x64     | transparent                                                   |
| 7  | `icon-memory.png`       | memory panel icon (remember/recall)     | 64x64     | transparent                                                   |
| 8  | `icon-autopilot.png`    | scheduler panel icon                    | 64x64     | transparent                                                   |
| 9  | `icon-settings.png`     | settings app icon                       | 64x64     | transparent                                                   |
| 10 | `cursor-default.png`    | mouse pointer (arrow)                   | 32x32     | transparent; if skipped we draw one procedurally              |

## Optional (nice to have)

| file name            | what it is                                | size (px) |
|----------------------|-------------------------------------------|-----------|
| `cursor-busy.png`    | busy/wait pointer                         | 32x32     |
| `wallpaper-alt-1.png`| second wallpaper (settings can cycle)     | 1024x768  |
| `icon-shutdown.png`  | shutdown button icon                      | 64x64     |
| `icon-network.png`   | placeholder for a future networking panel | 64x64     |

## Style guidance (not enforced)

- Flat colours and simple shapes survive palette reduction best.
- One accent colour that pops against the wallpaper is worth it: the UI
  chrome (title bars, launcher) reuses it programmatically.
- The OS's own vibe is dark background, monospaced accents, scheduler-
  green highlights — but your wallpaper and logo set the tone, so feel
  free to override that.

## Canonical copy

This file (in the repo) is authoritative. An artist-facing copy was
previously handed over as `download/m10-assets-spec.md`; if you make one,
generate it from this file rather than editing it separately.
