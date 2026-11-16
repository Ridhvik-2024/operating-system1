# os_sys_open.md

## Status
OPEN / INTENTIONALLY UNDECIDED  
NON-AUTHORITATIVE  
SAFE FOR EXPERIMENTATION

This document records **all remaining undecided, flexible, or implementation-dependent choices**
in the AI-governed operating system.

Anything listed here:
- may change
- may be experimented with
- may be replaced
- must not violate `os_sys_decided.md`

Anything **not listed here** is considered **locked**.

---

## 0. Purpose of This Document

This document exists to answer one critical meta-question:

> **Where are we still allowed to think?**

It prevents two failure modes:
- premature locking of implementation details
- accidental erosion of core guarantees

This file is the **only place** where ambiguity is allowed.

---

## 1. Host Control Plane Implementation (OPEN)

### 1.1 What Is Decided
- The host control plane exists
- It has absolute authority
- It is invisible to the AI and user
- It enforces isolation, rollback, and arbitration

### 1.2 Open Questions
- Kernel strategy:
  - minimal Linux kernel
  - microkernel + Linux userland
- Userland size:
  - initramfs-only
  - minimal service layer
- Boot chain:
  - Secure Boot only
  - Measured boot + attestation
- Update strategy:
  - immutable image replacement
  - A/B slot switching
  - staged rollout

### 1.3 Constraints
The host control plane must:
- remain auditable by hash
- remain replaceable without AI involvement
- never depend on AI correctness
- never expose its internal state

### 1.4 Red Lines
- No host logic inside the shared VM
- No host updates initiated by the AI
- No host introspection APIs

---

## 2. Shared VM OS Choice & Customization (OPEN)

### 2.1 What Is Decided
- There is exactly one shared VM
- It is the only visible computer
- It is persistent
- Both human and AI operate inside it

### 2.2 Open Questions
- Base OS:
  - mainstream Linux distro
  - custom minimal distro
- Desktop stack:
  - Wayland-only
  - Wayland + X11 compatibility
- Application model:
  - unrestricted installs
  - mediated installs via sandbox
- Degree of AI awareness:
  - fully standard desktop
  - light AI hooks (status, focus, intent indicators)

### 2.3 Constraints
- No “magic” AI-aware applications
- No invisible automation
- No dependency on obscure desktop components

### 2.4 Red Lines
- No second visible desktop
- No background AI-only UI
- No application-level authority escalation

---

## 3. Sandbox Technology Selection (OPEN)

### 3.1 What Is Decided
- Sandbox exists
- Sandbox is non-persistent
- Sandbox has no authority
- Sandbox produces results only

### 3.2 Open Questions
- Technology choices:
  - containers
  - microVMs
  - WASM runtimes
- Sandbox classes:
  - code execution
  - filesystem mutation
  - system-level actions
- Lifetime limits
- Parallel sandbox execution caps

### 3.3 Tradeoffs
- Fidelity vs speed
- Isolation vs memory usage
- Startup latency vs realism

### 3.4 Constraints
- Sandbox must not learn
- Sandbox must not influence policy
- Sandbox must be disposable

---

## 4. Resource Management & Limits (OPEN)

### 4.1 What Is Decided
- Resources are finite
- Human experience is prioritized
- Sandbox is lowest priority

### 4.2 Open Questions
- Default RAM split:
  - host
  - shared VM
  - sandbox
- Dynamic reallocation strategies
- CPU scheduling:
  - shared
  - pinned cores
- GPU models:
  - shared
  - exclusive
  - disabled for sandbox

### 4.3 Constraints
- AI must never starve the user
- Sandbox must never starve the shared VM
- GPU must not become a side-channel

---

## 5. AI Brain Configuration (PARTIALLY OPEN)

### 5.1 Locked
- Local-first intelligence
- Optional API-based intelligence (BYOK)
- Authority is always local
- Safety identical across modes

### 5.2 Open Questions
- Recommended local models for ≤8 GB RAM
- Automatic downscaling rules
- Context window shrink behavior
- How model switches are surfaced to the user

### 5.3 Constraints
- No silent degradation
- No pretending unchanged capability
- No model-based authority differences

---

## 6. Policy Engine Representation (OPEN)

### 6.1 What Is Decided
- Tier 0 and Tier 1 laws are code
- Laws are machine-verifiable
- Commentary is non-authoritative

### 6.2 Open Questions
- Internal representation:
  - declarative rules
  - restricted DSL
  - hybrid compiled model
- Validation pipeline
- Conflict reporting format
- Formal verification scope

### 6.3 Constraints
- No runtime modification of Tier 0 laws
- No human-readable text as enforcement
- No AI inspection of law logic

---

## 7. Cognitive Memory Details (OPEN)

### 7.1 What Is Decided
- Memory exists
- Memory is bounded
- Memory is separated by type
- Memory cannot store procedures

### 7.2 Open Questions
- When memory writes occur:
  - after failure
  - after rollback
  - after correction
- Memory decay or aging rules
- Confidence or weighting mechanisms
- Conflict resolution between memories
- Audit and inspection UX

### 7.3 Constraints
- No step-by-step storage
- No tool sequences
- No timing strategies

---

## 8. User Preference Memory UX (OPEN)

### 8.1 What Is Decided
- Preferences are user-owned
- Preferences never authorize unsafe actions
- Preferences require consent to write

### 8.2 Open Questions
- Editing interface
- Versioning
- Import/export
- Conflict resolution
- Reset-to-default UX

### 8.3 Constraints
- No hidden learning
- No behavioral shaping
- No preference inheritance without explicit action

---

## 9. Input Arbitration Parameters (OPEN)

### 9.1 What Is Decided
- Human input always wins
- AI yields immediately
- Arbitration is host-controlled

### 9.2 Open Questions
- AI input rate limits
- Cooldown after human override
- Visual ownership indicators
- Multi-device handling
- Accessibility considerations

### 9.3 Constraints
- No cursor fights
- No ambiguous control
- No AI “resuming” without permission

---

## 10. Voice Engine Integration (FUTURE, OPEN)

### 10.1 What Is Decided
- Voice is not required for v1
- Voice is not authoritative
- Voice is optional

### 10.2 Open Questions
- Boundary between voice-to-text and intent
- Offline-only vs optional cloud STT
- Latency tolerance
- Speaker identification
- Multi-user voice handling

### 10.3 Constraints
- Voice must route through the same intent pipeline
- No always-on listening
- No authority escalation

---

## 11. Network Model Details (OPEN)

### 11.1 What Is Decided
- Network access is dangerous
- Default must be conservative

### 11.2 Open Questions
- Default isolation level
- DNS behavior
- Traffic logging:
  - none
  - summary only
- Local-only networking
- Sandbox network access rules

### 11.3 Constraints
- No invisible networking
- No background access
- No sandbox internet by default

---

## 12. Update & Upgrade Strategy (OPEN)

### 12.1 What Is Decided
- Updates must be safe
- Updates must be rollbackable
- AI cannot update itself autonomously

### 12.2 Open Questions
- Update cadence
- Mandatory vs optional updates
- Key management
- Host vs shared VM update separation
- AI core update handling

### 12.3 Constraints
- Rollback must always apply
- Mixed-version authority must be avoided

---

## 13. Multi-User & Identity Model (OPEN)

### 13.1 What Is Decided
- v1 may be single-user
- AI must not arbitrate between humans

### 13.2 Open Questions
- Multi-user support scope
- Per-user shared VMs
- Guest or ephemeral sessions
- AI identity persistence across users
- Permission boundaries

### 13.3 Constraints
- No cross-user memory
- No AI-mediated human hierarchy

---

## 14. Observability & Debugging (OPEN)

### 14.1 What Is Decided
- Observability is necessary
- Enforcement must not weaken

### 14.2 Open Questions
- Debug modes
- Log exposure UX
- Forensic tools after rollback
- AI explanation verbosity

### 14.3 Constraints
- Debug ≠ permissive
- Logs must not leak sensitive state
- AI must not control observability

---

## 15. Hardware Support Scope (OPEN)

### 15.1 What Is Decided
- Hardware access is power
- Hardware must be virtualized
- Access must be revocable

### 15.2 Open Questions
- Minimum supported hardware
- GPU passthrough vs virtual GPU
- External storage handling
- Touch, pen, tablets
- Specialized devices

### 15.3 Constraints
- No raw device access
- No firmware access
- No persistent bindings

---

## 16. Guiding Rule for All Open Decisions

Across all open areas, this rule applies:

> **If a decision increases power, it must also increase containment, reversibility, or auditability.**

If it does not, it must be rejected.

---

## 17. Status Summary

- Core architecture: **LOCKED**
- Authority model: **LOCKED**
- Safety invariants: **LOCKED**
- AI operational contract: **LOCKED**
- Implementation details: **OPEN**

This document exists to keep future decisions **focused, honest, and constrained**.

---

## END OF DOCUMENT