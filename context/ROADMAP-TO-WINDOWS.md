# ROADMAP-TO-WINDOWS — from AI-OS 1.1 to desktop-capability parity

**Status:** plan, not a promise. Every estimate below is for **one experienced
systems developer, AI-assisted**, and every phase has a measurable exit test.
**Written:** 2026-11-17 · **Baseline:** `1.1.0-m10` (see `04-BUILD-VERIFY.md`).

---

## 0. Two definitions first, because they decide everything

### 0.1 "Windows capability" = five layers, not one

Windows is not an app suite with an OS attached. It is five layers that each
have to work:

| # | Layer | What "Windows-level" means concretely |
|---|---|---|
| **L1** | **Platform** | boots on real hardware; persistent writable root; virtual memory + swap; users, permissions, ACLs; TCP/IP + DHCP + DNS + TLS; USB; audio; printing; power management (sleep/hibernate); removable media |
| **L2** | **Shell** | a compositor; scalable fonts + Unicode; multi-monitor + DPI; resize/minimize/maximize/snap; clipboard; drag & drop; notifications; taskbar; file dialogs; settings; themes; keyboard layouts; accessibility |
| **L3** | **Apps** | the suite: editor, files, image viewer, media player, browser, mail, PDF, docs/sheets, calc, archive, screenshot, print, terminal, settings |
| **L4** | **App platform** | an SDK + stable ABI; third-party apps; a package format and repository; updates; crash isolation per app |
| **L5** | **Ops & security** | installer; updates with rollback; recovery; multi-user login; encryption; sandboxing per app; audit; backup/restore |

We are at: **L1 ~15 %** (only virtio block + ext2 + fb + input), **L2 ~20 %**
(framebuffer, 5 windows, one bitmap font), **L3 ~10 %** (5 apps),
**L4 0 %**, **L5 ~5 %** (durable store, no users, no updates).

### 0.2 Capability parity ≠ binary compatibility

We are **not** chasing Win32 / `.exe` compatibility, a registry, or Office file
fidelity. Those are ecosystem artifacts of a 40-year proprietary platform and
would poison every design decision this project has made. We chase
**capability parity**: a person can do everything they do on Windows, with our
apps and our kernel base.

That single decision is what makes this roadmap ~2 years instead of ~20.

---

## 1. The honest starting point (measured, not estimated)

| Fact | Value | Source |
|---|---|---|
| Kernel modules shipped | **22** (4 storage: `virtio_blk`, `mbcache`, `ext2`, + `aios_intent`; 18 fb/DRM/input) | `boot/dl/modules*/`, `boot/Makefile` |
| Audio / USB / network / ACPI code | **none** — zero references in `src/`, `apps/`, `boot/init.c` | grep |
| Kernel | Alpine `linux-virt` 6.6.142 — a **virtual-machine** kernel config | `boot/dl/kernel/` |
| Persistent storage | 8 MiB ext2 side disk, `/data` | `boot/init.c` durable phase |
| Root filesystem | **RAM initramfs** (2.4 MB xz, 103 entries); nothing persists outside `/data` | unpacked image |
| Users / permissions | one uid (root in the image), no login, no ACLs | `boot/init.c` |
| Networking | `-nic none`, seccomp kills `socket()` | `boot/run-qemu.sh`, `src/hardening.c` |
| Text | **one** 8×16 bitmap font, ASCII 32–126 only, no Unicode | `src/font8x16.h`, `src/fb.c` |
| Images | `.aiosimg` raw RGBA only; PNG/JPEG/BMP unsupported at runtime | `src/fb.c` |
| Apps | **5** (terminal, files, agents, memory, autopilot) | `apps/aios-desktop.c` |
| Shell builtins | 28 | `src/shell.c` |
| The mediator (policy/staging/rollback/audit) | **0 %** — the constitution is unimplemented | `02-WHAT-IS-MISSING.md` |

### The insight that shapes the whole plan

**The Linux kernel already contains Windows-class subsystems.** TCP/IP, USB,
ALSA, DRM, ACPI/S3/S4, ext4/NTFS3/exFAT, netfilter, namespaces, cgroups, PSI,
TPM, printing — all in mainline. We ship 22 modules out of thousands.

So this roadmap is **not** "write an operating system". It is:

1. **Supply** the kernel subsystems we already inherit (module + config work),
2. **Build the userland** that makes them usable (the real work),
3. **Govern** it all with the mediator (the differentiator, and non-negotiable).

And one hard prerequisite: `linux-virt` is a **VM kernel**. For real hardware we
need a full kernel config (`linux-lts` or a custom config) plus firmware blobs
(hundreds of MB — which is why W1 storage work comes before W4 hardware work).

---

## 2. Milestone overview

| ID | Version | Theme | Exit criterion (measurable) | Est. | Depends on |
|---|---|---|---|---|---|
| **W0** | 1.2 | **Trustworthy base** | CI green on 2 distros + in-QEMU boot test; mediator M1–M8 landed; governor supervised; 0 known S1 gaps | 6–8 wk | — |
| **W1** | 2.0 | **Real platform** | boots from disk (not initramfs-only), 4 users, swap works, `ping`/DNS/TLS work through a mediated stack, A/B updates | 4–6 mo | W0 |
| **W2** | 2.5 | **Desktop shell** | 5 apps run at 3 resolutions + 2 monitors, TTF text at 3 sizes, clipboard/DnD/notifications/taskbar, settings app | 3–4 mo | W1 |
| **W3** | 3.0 | **App suite + SDK** | 20 first-party apps; a third-party app builds against a published SDK and installs from a package | 4–6 mo | W2 |
| **W4** | 3.5 | **Hardware & devices** | boots + runs on 3 real laptops; audio plays; USB hotplug; webcam; printer; wifi; sleep/hibernate | 4–6 mo | W1, W2 |
| **W5** | 4.0 | **Multi-user & security** | login screen, per-user encryption, per-app sandbox via the mediator, audit UI, backup/restore | 2–3 mo | W0, W1, W3 |
| **W6** | 5.0 | **Ecosystem & polish** | package repo with 50+ packages, driver model, accessibility, enterprise management | ongoing | W3, W4, W5 |

**Total to single-user desktop parity (W0–W5): ~18–24 months.**
Ecosystem parity (W6) is open-ended, by nature.

### Two cross-cutting tracks (run in parallel, never as a milestone)

| Track | What | Why it cannot be a milestone |
|---|---|---|
| **A — The mediator** (`02-OS-CORE.md` M1–M8) | descriptor → policy → staging → promote/rollback → audit chain | every W1+ phase *creates* effects that must be mediated; retrofitting is worse than threading it in. Complete by end of W1, mandatory before W3 third-party apps |
| **B — AI capability** (the differentiator) | in-OS model, tool surface, multimodal input, autonomy levels | the whole point of the OS. Runs parallel and is what makes this not-a-Windows-clone. Detail in §6 |

---

## 3. W0 — Trustworthy base (v1.2) · 6–8 weeks

**Goal:** make the OS **provable** before making it bigger. Nothing here is
user-visible; all of it is what lets everything else land safely.

### Phase W0-A — Build & truth (1–2 wk)
- **Features:** CI on Ubuntu 22.04 + 24.04 (`make && make test`, `-Werror`);
  boot CI running `boot-test.sh`; `LICENSE`; repo-layout doc; kill the stale
  doc claims.
- **Capability unlocked:** *"a clean clone builds and proves itself"* — today it
  does not (font + glibc bugs were live for weeks).
- **Cards:** `H1` ✅ done · `H2` · `H3` · `H4` · `H5` · `H6`.
- **Exit:** red CI blocks a deliberately broken PR; boot job green in < 15 min.

### Phase W0-B — The mediator spine (3–5 wk) ← *the constitution becomes real*
- **Features:** `aios_action` descriptor · `policy_eval()` fail-closed law table ·
  staging + diff + `promote`/`discard` · worker effects document · snapshot +
  `rollback` (PID 1 owned) · hash-chained audit + `audit verify` · origin tagging
  (`HUMAN`/`AI`) · promote UX on console and desktop.
- **Capability unlocked:** *"the AI cannot change reality without a mediated,
  recorded, reversible decision"*. Without this, every later phase is a
  liability — an app suite on an unmediated OS is exactly what this project
  exists to avoid.
- **Cards:** `02-OS-CORE.md` `M1`…`M8`.
- **Exit:** an agent that writes outside `/workspace` is denied, the denial is in
  a verified hash chain, `rollback` restores the bytes, and a boot test proves it
  survives a reboot.

### Phase W0-C — Supervise itself (1–2 wk)
- **Features:** governor respawn (today it dies silently — `init.c` never tracks
  it) · `flock` on the shared store · boot log ring buffer + `log`/`audit`
  builtins · selftest opt-in for release images.
- **Capability unlocked:** *"services stay up; failures are visible"*.
- **Cards:** `C1`, `C3`, `C4`, `C5` (`05-LARGE-TASKS.md`, `04-SMALL-TASKS.md`).
- **Exit:** kill the governor in a running OS → it comes back and says so; kill
  a tool worker mid-write → the store is intact.

**W0 total: 6–8 weeks. It produces no new feature a user can see, and it is the
highest-leverage work in this document.**

---

## 4. W1 — Real platform (v2.0) · 4–6 months

**Goal:** stop being a demo that only exists in RAM. This is the milestone that
turns "an impressive prototype" into "a system you can install".

### Phase W1-A — Persistent root & storage (4–6 wk)
- **Features:** read-only root image (`erofs`/`squashfs`) on disk · **overlayfs**
  upper for `/usr`,`/etc` · ext4 (or NTFS3) data partition instead of 8 MiB ext2 ·
  `/home` per user · **swap** · `fsck`-on-boot · removable media (`usb-storage`,
  `exfat`, `vfat`, `iso9660`, `udf`) · mount manager + `/etc/fstab` equivalent.
- **Capability unlocked:** install/update software; keep files; plug in a USB
  stick; survive a full disk without dying.
- **Exit:** install to a 16 GB virtual disk, fill it, reboot 20×, `fsck` clean;
  a USB stick mounts and copies.
- **Risk:** the 8 MiB → GB jump changes init's whole boot model. Budget the
  whole phase for it.

### Phase W1-B — Memory & process model (2–3 wk)
- **Features:** swap-backed virtual memory tuning (`vm.swappiness`,
  overcommit policy) · `memory.max`/`pids.max` per cgroup class · OOM policy that
  **never kills the shell or the desktop** · per-app process trees · zombie/child
  reaping audit.
- **Capability unlocked:** run a browser-class memory hog next to the desktop
  without the OS falling over.
- **Exit:** 3 concurrent agents + a memory hog on a 2 GB VM → desktop stays
  responsive, the hog is the one that dies, and the decision is logged.

### Phase W1-C — Identity & permissions (3–4 wk)
- **Features:** multiple users (`/etc/passwd` + shadow) · login on console and
  desktop · groups · file modes enforced (`umask`, ownership) · **ACLs** on the
  data partition · `sudo`-equivalent (time-boxed capability grant) · the AI runs
  as its **own uid** (this is `L2`'s uid split, arriving here because everything
  else needs it).
- **Capability unlocked:** *"the human's authority is a kernel credential, not a
  convention"* — the missing half of the mediator.
- **Exit:** the AI uid cannot read another user's home or write `/usr`; the human
  can; both are kernel denials, not application checks.

### Phase W1-D — Networking stack (4–6 wk)
- **Features (in order):** `virtio-net` + `e1000` drivers · `ip`/`dhcp` client ·
  DNS resolver · `netd` in PID 1 as **the only socket holder** (per constitution)
  · `http`/`https` client with TLS (`mbedtls`-class, vendored once, or a tiny
  client + `getrandom`) · per-domain allowlist + visible network indicator ·
  firewall (`nftables` or our own policy) · time sync (SNTP).
- **Capability unlocked:** a browser becomes possible; package updates become
  possible; the "big model" question gets a real answer.
- **Exit:** a mediated `GET https://…` returns bytes; a non-allowlisted domain is
  denied **and** audited; `socket()` inside the sandbox still dies by seccomp.
- **Decision required first:** air-gapped (vsock) vs mediated network — see
  `05-DECISIONS.md` C3. This phase assumes **mediated**.

### Phase W1-E — Installer & updates (3–4 wk)
- **Features:** an installer (partition, format, install root, create user) ·
  **A/B slots** · `aios-update` writing the inactive slot · `fs-verity` on
  binaries · bootloader switch + rollback on failed boot (3-strike) · factory
  reset.
- **Capability unlocked:** *"I can install this on a machine and get it back if
  an update breaks it"* — table stakes for every real OS.
- **Exit:** install → update → boot new slot → force a failed boot → automatic
  fallback to the old slot, with the store and user files intact.

**W1 total: 4–6 months.** After W1 you can hand someone a machine.

---

## 5. W2 — Desktop shell (v2.5) · 3–4 months

**Goal:** make the GUI something a normal person can use for an hour without
being taught. Today: 5 fixed windows, one bitmap font, no resize, no clipboard.

### Phase W2-A — Text, fonts, i18n (3–4 wk)
- **Features:** TrueType/OpenType rasteriser (stb_truetype-class, vendored) ·
  font fallback chain · **Unicode** (UTF-8 everywhere; today the shell and the fb
  are byte-oriented) · 3 UI sizes + DPI scaling · bidi/RTL ready (not shipped) ·
  keyboard layout tables (US/UK/DE/FR/ES/IN-Devanagari) · IME seam.
- **Capability unlocked:** readable text at any size; the 60 % of humanity that
  does not read ASCII-only English can use the machine.
- **Exit:** the same UI at 100 %/150 %/200 % with crisp glyphs; 5 layouts; a
  Devanagari string renders.
- **Note:** this retires `font8x16.h` as the *only* font — keep it as the
  zero-asset fallback.

### Phase W2-B — Compositor & window management (3–4 wk)
- **Features:** damage-driven compositor with per-window layers · resize,
  minimize, maximize, **snap** (halves/quarters) · tiling option · multi-monitor
  (`drm` connectors, per-output fb) · focus/task model · smooth scrolling ·
  wallpaper/theme engine · window animations (optional, cheap) · GPU accel later
  (W4).
- **Capability unlocked:** everyday window juggling.
- **Exit:** 8 windows across 2 virtual outputs, snap + maximize + restore, no
  tearing, damage-bounded flush still holds (perf gate: idle CPU < 3 %).
- **Cards:** `UI-02` (resize) and `UI-05` (minimize/taskbar) are the first slices.

### Phase W2-C — Shell UX (3–4 wk)
- **Features:** taskbar with running/closed apps · start menu / app grid ·
  **clipboard** with history · **drag & drop** between windows · **notifications**
  (centre + per-app) · file-open/save **dialogs** · context menus · keyboard
  shortcuts + a shortcut editor · screenshot tool · lock screen.
- **Capability unlocked:** the muscle memory of a desktop OS.
- **Cards:** `UI-03` (mods), `UI-04` (clipboard), `UI-07` (AI-driving HUD —
  mandatory before any AI-driven GUI input).

### Phase W2-D — Settings, theming, accessibility (2–3 wk)
- **Features:** settings app (display, sound, network, users, power, time,
  keyboard, apps) · themes (light/dark, accent) · font scaling · high contrast ·
  screen reader seam (a11y API + focus order) · sticky keys.
- **Capability unlocked:** the system is configurable without editing files.
- **Exit:** change resolution, layout, theme, and user from the GUI only.

**W2 total: 3–4 months.** After W2 it looks and feels like a computer.

---

## 6. W3 — App suite + SDK (v3.0) · 4–6 months (parallelisable)

**Goal:** the apps. This is the phase most people mean by "Windows-level", and
the one where AI-OS can be *better* rather than merely equal.

### The app inventory (all first-party, ordered by dependency)

| # | App | Family | Needs from platform | Size | Milestone |
|---|---|---|---|---|---|
| 1 | **terminal** | system | shell (exists) | S | W2 (done) |
| 2 | **files** | system | W1-A mounts, dialogs | M | W2 |
| 3 | **settings** | system | W2-D | M | W2 |
| 4 | **notepad / text editor** | text | TTF text, clipboard | M | W2–W3 |
| 5 | **image viewer** | media | PNG/JPEG/BMP decode, zoom | M | W3 |
| 6 | **PDF reader** | media | PDF (own renderer or mupdf-class) | L | W3 |
| 7 | **archive manager** | util | zip/tar/gz/xz | S | W3 |
| 8 | **calculator** | util | — | S | W3 |
| 9 | **screenshot / recorder** | util | compositor hooks | S | W2–W3 |
| 10 | **clock / calendar** | util | time sync | S | W3 |
| 11 | **music player** | media | audio (W4-B) | M | W4 |
| 12 | **video player** | media | audio + codecs + GPU | L | W4 |
| 13 | **photos** (album/editor-lite) | media | image codecs, crop/rotate | M | W3–W4 |
| 14 | **docs** (rich text, .md/.html/.odt-lite) | office | TTF, tables, print | L | W3+ |
| 15 | **sheets** (grid, formulas, CSV) | office | docs' text engine | L | W4 |
| 16 | **slides** | office | docs engine | M | W5 |
| 17 | **mail** | comms | network (W1-D), TLS | L | W4 |
| 18 | **browser** | comms | network, TLS, HTML/CSS/JS (huge) | XL | W5–W6 |
| 19 | **chat / video call** | comms | network, camera, audio | L | W5 |
| 20 | **print** | system | CUPS-class + spooler (W4-D) | M | W4 |
| 21 | **backup / restore** | system | snapshots (W0-B), storage | M | W5 |
| 22 | **package manager UI** | system | W3-E | M | W3 |
| 23 | **task manager / monitor** | system | cgroups + intents | S | W3 |
| 24 | **passwords / vault** | security | encryption (W5-B) | M | W5 |
| 25 | **camera** | media | UVC (W4-C) | S | W4 |

**Honest sizing note:** the browser is bigger than W0+W1+W2 combined. It is the
one item that realistically means "embed a proven engine" (Servo/Gecko/Blink
class) rather than "write it". Everything else is ours.

### Phase W3-A — App runtime & SDK (4–5 wk)
- **Features:** a stable app ABI (a C ABI + a small client library) · app
  lifecycle supervised by PID 1 (start/stop/crash-isolate/restart) · per-app
  capability manifest (this is the mediator at app scale) · per-app data dirs ·
  IPC to the shell/agents (sembus over a socket, `L1`) · an `aios-sdk` with
  headers, examples and a `aios-build` wrapper · internationalisation helpers.
- **Capability unlocked:** *"someone else can write an app"* — without this the
  suite is the ceiling.
- **Exit:** a third-party hello-world app (built from the SDK, not in-tree)
  installs, runs sandboxed, and is killed cleanly when it misbehaves.

### Phase W3-B — Text & documents (4–6 wk)
Editor (tabs, search/replace, syntax highlight, undo) → markdown/HTML viewer →
docs engine (paragraphs, styles, tables, images, print-to-PDF).
**Exit:** write a 20-page document with a table and an image, export PDF, print it.

### Phase W3-C — Media (4–6 wk)
Image codecs (PNG/JPEG/BMP/GIF/WebP-lite) · viewer with zoom/rotate/slideshow ·
archive manager · music player (after W4-B) · video player (needs codecs + GPU).
**Exit:** open a JPEG, view it full-screen, play an MP3, list a zip and extract one file.

### Phase W3-D — Utilities & system apps (2–3 wk)
Calculator · clock/calendar · screenshot · task manager (cgroup + intent view) ·
package-manager UI.
**Exit:** all six work by mouse and keyboard.

### Phase W3-E — Packaging & distribution (3–4 wk)
Package format (signed bundle + manifest) · repository index · install/update/
remove with rollback · dependency-free-by-design policy · `aios-pkg` CLI + UI.
**Exit:** install a package from a local repo, update it, roll it back.

**W3 total: 4–6 months** (app phases parallelise across people; W3-A does not).

---

## 7. W4 — Hardware & devices (v3.5) · 4–6 months

**Goal:** leave the VM. This is where `linux-virt` stops being enough and the
"adopt Linux parts" strategy pays for itself.

### Phase W4-A — Kernel strategy for real hardware (3–4 wk)
- **Features:** a full kernel config (or `linux-lts`) with ACPI, `xhci`/`ehci`,
  HID, `ahci`/`nvme`, Ethernet/WiFi/Bluetooth, ALSA, `uvc`, DRM drivers for
  Intel/AMD/NVIDIA-basic, `snd-hda` · **firmware blobs** (`linux-firmware` subset,
  curated — this is hundreds of MB, hence W1-A first) · module autoloading by
  device id (`modprobe` rules) · a **hardware support tier list** (Tier 1:
  known-good laptops; Tier 2: virtio/qemu; Tier 3: broad).
- **Capability unlocked:** boots on a real machine and finds its own devices.
- **Exit:** boots to the desktop on 3 named laptops, with display, keyboard,
  touchpad, wifi, audio, and sleep.

### Phase W4-B — Audio (3–4 wk)
ALSA in-kernel + a userland mixer (`aios-audio`) · default device, per-app
volume, mute · media keys · notification sounds · recording · a visible indicator
when the mic is armed (constitution §12).
**Exit:** play two sources, adjust per-app volume, record and play back a clip.

### Phase W4-C — USB, cameras, Bluetooth, printing (4–6 wk)
USB hotplug + a device-manager UI · mass storage · UVC webcam capture ·
Bluetooth (HID + audio) · printing (USB + network, spooler, CUPS-class) · scanner
(stretch).
**Exit:** plug in a USB stick, a mouse, and a printer, each works within 5 s.

### Phase W4-D — Power, thermal, battery (2–3 wk)
ACPI sleep (S3) and hibernate (S4) · lid/button handling · battery + thermal UI ·
CPU frequency scaling (schedutil) · the autopilot governor learns a **power**
class alongside its CPU classes.
**Exit:** lid close → sleep → wake in < 3 s with state intact; battery drains
< 8 %/h idle.

### Phase W4-E — Graphics acceleration (4–6 wk, optional-but-wanted)
DRM/KMS modesetting proper · GPU 2D accel (or virtio-gpu venus in VMs) · hardware
video decode · the compositor keeps a software path (constitution: never *require*
a GPU).
**Exit:** video playback at 1080p30 with CPU < 40 %; the software path still boots.

**W4 total: 4–6 months.** After W4 it is a computer you can own.

---

## 8. W5 — Multi-user & security (v4.0) · 2–3 months

**Goal:** the OS grows up about people and trust. Much of the machinery landed
in W1-C and W3-A; this milestone makes it complete, visible and enforced.

### Phase W5-A — Accounts & sessions (3–4 wk)
Login screen · per-user settings, clipboard, history, agents and memory
(namespaced) · fast user switching · guest session · per-user encryption keys ·
parental/managed profiles (stretch).
**Exit:** two users, separate memories and files, switch without logging out.

### Phase W5-B — Encryption & integrity (3–4 wk)
Full-disk encryption (LUKS-class) · per-user home encryption · `fs-verity` +
IMA-style measurement · **measured boot** + attestation seam (TPM) · key
management UI · password vault app.
**Exit:** pull the disk → unreadable; tamper with a binary → boot refuses and says
which file.

### Phase W5-C — App sandboxing at OS scale (3–4 wk)
Every app gets the W0-B mediator + W1-C uid + namespaces + its manifest
capabilities · prompts follow the constitution (`NEEDS_HUMAN` only for
irreversible/bulk) · a per-app permission UI · an audit viewer app.
**Exit:** an app that requests the mic without a grant is denied by the kernel,
and the attempt is in a verified audit chain the user can read in the GUI.

### Phase W5-D — Backups & recovery (2 wk)
Scheduled snapshots of `/home` + store · restore UI · boot-recovery console ·
factory reset (keeps user data on request).
**Exit:** delete the OS, reinstall, restore a user's files and memory.

**W5 total: 2–3 months.**

---

## 9. W6 — Ecosystem & polish (v5.0) · ongoing

Not a project phase; a permanent mode of operation.

- **Package repository** with 50+ packages; third-party developers; SDK docs and
  samples; a seed of community apps.
- **Driver model** for out-of-tree devices (a signed, sandboxed driver story).
- **Accessibility** certification (screen reader, contrast, keyboard-only, motor).
- **Printing/scanning** breadth; **enterprise** management (policy bundles).
- **Performance**: boot < 8 s on Tier-1 hardware; idle RAM < 600 MB with the
  desktop up; battery and thermal targets.
- **Localisation**: 10 languages; RTL; input methods.
- **Reliability**: crash reporting with consent; self-healing (the mediator
  already gives us the mechanism).

---

## 10. Track B — AI capability (the differentiator, not the floor)

Windows parity is table stakes; this is why the project exists. Runs parallel to
W0–W6, gated by the mediator at every step.

| ID | Capability | Depends on | Gate |
|---|---|---|---|
| **B1** | In-OS model (local GGUF over vsock, or a curated small model in-image) | W1-D decision | must run with no network |
| **B2** | Every app exposes a **tool surface** to agents (files, docs, media, settings) | W3-A | every call is a descriptor |
| **B3** | **Vision**: screenshot/model-window → understand → act | W2-B | mediated input only (`UI-07` first) |
| **B4** | Voice in/out | W4-B | never always-on; explicit arm |
| **B5** | Multi-step autonomy with human approval on `NEEDS_HUMAN` | W5-C | rollback must exist before autonomy |
| **B6** | Memory that spans apps (the OS-owned 3-tier store, promoted) | W1-A | user-owned, inspectable, erasable |
| **B7** | "Do it for me" workflows (observed → proposed → staged → promoted) | B2+B5 | never silent; diff always shown |

**Rule:** B-track work may never ship a capability without its containment in the
same milestone (constitution §13). That is why B5 waits for W5-C.

---

## 11. Sequencing rules (learned the hard way)

1. **Storage before hardware.** Firmware blobs need a real filesystem; a real
   filesystem needs an installer; an installer needs A/B. Do not attempt W4 on
   the 8 MiB ext2 disk.
2. **The mediator before the app platform.** W3-A hands third-party code a
   capability manifest; without W0-B there is nothing to enforce it with.
3. **Identity before networking.** A mediated network stack needs to know *whose*
   request it is. W1-C before W1-D.
4. **Text before apps.** Half the app suite is unreadable at 8×16 ASCII. W2-A
   before W3-B.
5. **Kernel strategy before driver work.** Choose `linux-virt`/`linux-lts`/custom
   *once*, in W4-A, and write the decision down.
6. **Never skip W0.** A bigger OS on an unprovable base is a worse OS.

---

## 12. Non-goals (deliberate, so nobody drifts)

| Non-goal | Why |
|---|---|
| Win32 / `.exe` compatibility | it would dictate the entire architecture; capability parity is the goal |
| Registry semantics | a config-file + JSON world is strictly better and already built |
| Bit-for-bit Office file fidelity | docs/sheets aim at ODF/CSV/Markdown, not `.docx` fidelity |
| Windows drivers | our driver story is Linux modules + (W6) a sandboxed out-of-tree model |
| A general-purpose distro userland | multiplies the attack surface the seccomp allowlist closes |
| systemd / dbus / udev | a second control plane the AI could address (constitution) |
| Kernel patches | stay stock; at most one out-of-tree LKM |
| "AI that needs the cloud" | the OS must be fully functional offline |

---

## 13. Effort model (so expectations are honest)

| Scope | 1 dev, AI-assisted | 2 devs | What is *not* included |
|---|---|---|---|
| W0 | 6–8 wk | 4 wk | — |
| W1 | 4–6 mo | 3 mo | printing, audio |
| W2 | 3–4 mo | 2 mo | GPU accel |
| W3 | 4–6 mo | 3 mo | a real browser |
| W4 | 4–6 mo | 3 mo | every laptop on earth |
| W5 | 2–3 mo | 1.5 mo | enterprise management |
| **W0–W5** | **~18–24 mo** | **~13–16 mo** | ecosystem parity (W6) |

Compare: Windows Vista → 11 is ~20 years and thousands of people, because it
carries Win32, driver ecosystems and enterprise contracts. We are not carrying
those, and we inherit a kernel that already solved the hard parts.

**Honest ceiling:** this plan reaches a usable single-user desktop with a modern
app suite and AI-native governance. It does **not** reach "install anything from
the internet and every peripheral on earth works" — that is W6, and it is
community-shaped, not roadmap-shaped.

---

## 14. How this plugs into the existing task system

- **This file is the map.** Existing cards keep their IDs (`H*`, `M*`, `APP-*`,
  `UI-*`, `L*`, `C*`); each phase above names the cards that move it.
- **New cards for unbuilt phases** get phase-prefixed IDs:
  `W1-A1`, `W1-A2`, … `W3-B4`, one card each, in
  `context/todo/` as their phase starts. Do not pre-write 300 cards — write
  them when the phase opens, so they reflect the platform as it actually is.
- **Every phase ends with a written exit test** in `04-BUILD-VERIFY.md` style:
  command → expected output. A phase without that never closes.
- **Update this roadmap** when a phase reveals that an estimate was wrong. The
  numbers are a tool for planning, not a contract.

---

## 15. What to do Monday morning

1. **W0-A:** merge PR #1, add CI (`H3`) — it would have caught the last two
   breakages automatically.
2. **W0-B:** start `M1` (descriptor) and `M2` (policy) — they are pure,
   unit-testable, and everything else waits on them.
3. In parallel, hand ChatGPT the desktop cards (`APP-03`, `UI-03`, `APP-02`,
   `UI-06`) — they touch `apps/` and `src/fb.c`/`win.c` only, so they cannot
   collide with the mediator work.

**That is W0. Everything after it is sequence, not invention.**
