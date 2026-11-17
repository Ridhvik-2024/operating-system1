# 07-FREE-TODO-FORMAT — your own scratch list

Copy one block per idea. Keep the file flat; move finished items to `00-MASTER.md`
→ **Done**. This file is for *your* items that are not in the numbered lists yet.

```markdown
## <ID> — <one-line title>
size: S | M | L        files: <the 1–3 files this touches>
why:  <one sentence: what becomes better/true>
do:   1. ...
      2. ...
      3. ...
prove: <command or test that must pass>
risk: <what could break: a suite, a GUI check, boot>
```

Rules of thumb:

- **S** = one file, under an hour, no API change.
- **M** = one feature in one subsystem, needs a test, under a day.
- **L** = changes an interface or crosses subsystems; needs a spec first.
- If a card touches more than 3 files, split it.
- If you cannot name the test, the card is not ready — write the test name first.

---

## My list

### TODO-01 — <title>
size: ·
files: ·
why: ·
do: ·
prove: ·
risk: ·

---

## Ideas parked (not scheduled)

| Idea | Why parked |
|---|---|
| `mesh` — TCP/vsock transport for the bus | needs `L1` (socket broker) first |
| `cockpit` — mission-control dashboard | needs `M6` (audit chain) to display |
| voice input | constitution §10: optional, must route through the same pipeline |
| multi-user | constitution §13: needs `M7`/`L2` identity split first |
| package manager | needs `L4` (persistent root) |
| GPU acceleration for the desktop | `bochs` is a plain framebuffer; no DRM accel path yet |
| A real filesystem driver in userspace | `ext2` kernel module + overlay is enough until `L4` |
| Moving `aiosh` to Rust | violates RULES §1.1 (C11, zero deps) |
