# os_sys_open.md

## Purpose
This document records **all remaining open, undecided, or intentionally flexible decisions** in the AI-governed operating system project.

Anything listed here:
- Is **not finalized**
- May change during implementation
- Is safe to experiment with
- Requires explicit future decisions

Anything **not listed here** should be treated as **locked** (see `os_sys_decided.md`).

---

## 1. Host Control Plane Implementation Details (OPEN)

### Open Questions
- Which base kernel strategy?
  - Minimal Linux (initramfs-style)
  - Microkernel + Linux userland
- Boot chain design:
  - Secure Boot only
  - Measured boot + attestation
- How updates to the control plane are delivered and verified
- Whether the control plane is immutable or updatable in-place

### Open Risk
- Too much logic in host → complexity
- Too little logic → weak enforcement

---

## 2. Shared VM OS Choice & Customization (OPEN)

### Open Questions
- Base OS for shared VM:
  - Minimal Linux desktop
  - Custom distro
- Desktop stack:
  - Wayland-only vs X11 compatibility
- How much of the desktop is AI-aware vs standard apps
- Whether package installation is unrestricted or mediated

---

## 3. Sandbox Technology Selection (OPEN)

### Open Questions
- Which sandbox mechanisms to support:
  - Containers
  - MicroVMs
  - WASM runtimes
- How many sandbox types exist (code, file, system)
- Maximum sandbox lifetime
- Parallel sandbox execution limits

### Open Tradeoff
- Fidelity vs speed vs memory usage

---

## 4. Resource Management & Limits (OPEN)

### Open Questions
- Default RAM split between:
  - Host
  - Shared VM
  - Sandbox
- Dynamic reallocation strategies
- CPU core pinning vs shared scheduling
- GPU access models:
  - Shared
  - Exclusive
  - Disabled for sandbox

---

## 5. AI Brain Configuration (PARTIALLY OPEN)

### Locked
- Local-first intelligence
- Optional API-based intelligence with BYOK only
- Local authority always wins

### Still Open
- Recommended local models for ≤ 8 GB RAM
- Automatic model downscaling rules
- Context window management
- How model switching is surfaced to the user

---

## 6. Policy Engine Representation (OPEN)

### Open Questions
- Internal policy format:
  - Declarative rules
  - DSL
  - Hybrid compiled model
- How policies are validated before activation
- How conflicts are reported to the user
- Whether formal verification is applied to Tier 0 laws

---

## 7. Cognitive Memory Details (OPEN)

### Open Questions
- When memory is written:
  - After success
  - After failure
  - After rollback
- Memory decay rules
- Confidence scoring mechanisms
- Conflict resolution between memories
- How memory can be inspected or audited

---

## 8. User Preference Memory UX (OPEN)

### Open Questions
- How preferences are reviewed and edited
- Whether preferences are versioned
- Import/export support
- How conflicting preferences are resolved
- UI vs voice-only preference management

---

## 9. Input Arbitration Parameters (OPEN)

### Open Questions
- AI input rate limits
- Cooldown durations after human override
- Visual indicators design (cursor, overlay, color)
- Voice-based overrides vs physical overrides
- Multi-input-device handling

---

## 10. Voice Engine Integration (OPEN)

### Open Questions
- Exact boundary between:
  - Voice-to-intent (host)
  - Intent-to-action (AI)
- Latency tolerance
- Offline-only vs optional cloud STT
- Voice authentication or speaker recognition
- Multiple users / voices handling

---

## 11. Network Model Details (OPEN)

### Open Questions
- Default network isolation level
- DNS resolution model
- Traffic logging (summary vs none)
- Local-only networking support
- Sandbox network restrictions

---

## 12. Update & Upgrade Strategy (OPEN)

### Open Questions
- How the OS updates itself safely
- Whether rollback applies to updates
- How AI core updates differ from OS updates
- Whether updates are mandatory or optional
- Key management for signing updates

---

## 13. Multi-User & Identity Model (OPEN)

### Open Questions
- Single-user only vs multi-user support
- Whether AI identity persists across users
- How permissions differ per user
- Guest mode or ephemeral sessions
- Whether identity resets across reinstall

---

## 14. Observability & Debugging (OPEN)

### Open Questions
- How much internal state is visible to the user
- Debug modes (developer vs normal)
- How logs are exposed safely
- Whether AI explanations are mandatory or optional
- Forensic tooling after rollbacks

---

## 15. Hardware Support Scope (OPEN)

### Open Questions
- Minimum supported hardware
- GPU passthrough vs virtual GPU
- Camera / mic permission UX
- External storage handling
- Specialized devices (tablets, touch, pen)

---

## Guiding Rule for All Open Decisions

> Any decision that increases **power** must also increase **reversibility**, **auditability**, or **containment**.

If it doesn’t, it should be rejected.

---

## Status Summary

- Core architecture: **LOCKED**
- Safety invariants: **LOCKED**
- Authority model: **LOCKED**
- UX philosophy: **LOCKED**
- Implementation choices: **OPEN**

This document exists to keep future discussions **focused, honest, and disciplined**.