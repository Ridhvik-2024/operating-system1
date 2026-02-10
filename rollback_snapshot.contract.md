# rollback_snapshot.contract.md

## Status
FINAL for v1  
MANDATORY  
HOST-ONLY  
NON-NEGOTIABLE

This document defines the **Rollback & Snapshot Contract**:  
the canonical rules governing **time control, state restoration, and failure recovery** in the AI-governed operating system.

Rollback is not an error mechanism.  
Rollback is **system physics**.

Any implementation that violates this contract is invalid.

---

## 0. Purpose

This contract exists to answer one question:

> **How does the system guarantee that any failure—AI, user, or system—can be reversed safely and deterministically?**

Rollback is the final safety backstop.
If rollback fails, the system has failed.

---

## 1. Core Invariants (ABSOLUTE)

The following must **always** be true:

1. Rollback is always available
2. Rollback cannot be disabled
3. Rollback is host-controlled
4. Rollback revokes all AI authority
5. Rollback revokes all hardware access
6. Rollback restores a previously valid state
7. Rollback does not depend on AI correctness

Violation of any invariant is a **critical system failure**.

---

## 2. Definitions

### 2.1 Snapshot

A **snapshot** is a host-owned, immutable capture of system state sufficient to restore the system to a known-good point.

Snapshots are:
- invisible to the AI
- not modifiable
- not inspectable
- not deletable by the AI

---

### 2.2 Rollback

A **rollback** is the act of restoring the system to a snapshot.

Rollback:
- is atomic
- is authoritative
- may occur at any time
- does not require consent once triggered

---

## 3. Snapshot Scope (WHAT Is Captured)

### 3.1 Included State (MANDATORY)

Snapshots MUST include:

- Shared VM filesystem state
- Shared VM process state (if supported)
- Configuration state
- Policy-relevant metadata
- AI operational state (Layer B)
- Input arbitration state

---

### 3.2 Excluded State (MANDATORY)

Snapshots MUST NOT include:

- AI stateless core (Layer A)
- AI cognitive memory (Layer C1)
- User preference memory (Layer C2)
- Host control plane state
- Host secrets or keys

This ensures clean restarts without memory loss or authority bleed.

---

## 4. Snapshot Types

### 4.1 Scheduled Snapshots

- Taken automatically every **30 minutes**
- Incremental where possible
- Retention policy is host-defined
- Old snapshots may be pruned safely

---

### 4.2 Pre-Action Snapshots (MANDATORY)

A snapshot MUST be taken before:

- any Tier 2 action
- any filesystem bulk change
- any package install or removal
- any configuration change
- any action with `risk_level >= HIGH`

If snapshot creation fails → action is blocked.

---

### 4.3 Emergency Snapshots (OPTIONAL)

The host MAY take emergency snapshots:
- before risky system transitions
- before updates
- before experimental operations

Emergency snapshots are invisible and silent.

---

## 5. Rollback Types

### 5.1 Micro Rollback

- Scope: single action or small change
- Triggered automatically
- Silent to the user
- Used for:
  - failed sandbox promotion
  - minor validation errors

---

### 5.2 Session Rollback

- Scope: recent user-visible changes
- User-noticeable
- Used for:
  - user dissatisfaction
  - repeated AI failures
  - partial system instability

---

### 5.3 Emergency Rollback

- Scope: full system restore
- Triggered by:
  - host watchdog
  - critical policy violation
  - system crash
  - human intervention
- All processes are terminated
- Hardware access revoked immediately

---

## 6. Rollback Triggers (AUTHORITATIVE)

Rollback MUST occur when any of the following happens:

- Policy Engine issues `FORCE_ROLLBACK`
- Post-promotion validation fails
- AI violates a Tier 0 law
- Sandbox result contradicts declared effects
- Watchdog timeout
- Host detects undefined behavior
- Human explicitly requests rollback

Rollback MUST override all other actions.

---

## 7. Rollback Execution Semantics

### 7.1 Atomicity

Rollback MUST be atomic:
- partial rollback is forbidden
- either the system is restored or rollback fails entirely

If rollback fails → emergency halt.

---

### 7.2 Authority Reset

During rollback:

- AI execution is paused immediately
- All AI processes are terminated
- Input arbitration is reset
- All AI hardware access is revoked

After rollback:
- AI restarts from a clean state
- No operational state persists

---

### 7.3 Time Semantics

From the AI’s perspective:
- rollback is normal
- time may appear discontinuous
- no assumption of continuity is allowed

The AI must reconcile itself without complaint or panic.

---

## 8. Hardware Revocation (MANDATORY)

On rollback:

- GPU access is revoked
- Network access is revoked
- USB devices are detached
- Camera/microphone access is revoked
- Screen capture is revoked

No hardware access may survive rollback.

---

## 9. Rollback Visibility Rules

### 9.1 User Experience

Rollback must:
- never feel like a crash
- never blame the user
- never feel punitive
- feel intentional and controlled

---

### 9.2 AI Experience

The AI must:
- treat rollback as expected
- not apologize excessively
- not attempt to “avoid” rollback
- not pressure the user afterward

---

## 10. Interaction With Action Descriptors

### 10.1 Precondition

An Action Descriptor MUST declare:
- rollback support
- rollback scope

If rollback is not supported → action is blocked.

---

### 10.2 Post-Promotion

If a promoted action later violates constraints:
- rollback scope is derived from:
  - snapshot boundary
  - declared effects
  - sandbox diff

Rollback may exceed declared scope if required for safety.

---

## 11. Logging & Audit

Every rollback event MUST log:

- rollback type
- triggering reason
- affected action_id(s)
- snapshot reference
- timestamp

Logs are:
- append-only
- host-controlled
- not editable
- not suppressible

The AI may see **that** a rollback happened, never **why in detail**.

---

## 12. Failure Handling (FAIL-CLOSED)

If rollback:
- cannot be initiated
- cannot complete
- results in inconsistent state

The host MUST:
- halt the shared VM
- revoke all AI authority
- require human intervention

Silent degradation is forbidden.

---

## 13. Forbidden Patterns (ABSOLUTE)

The following are never allowed:

- Disabling snapshots
- Skipping pre-action snapshots
- AI-triggered rollback suppression
- Partial rollback
- Rollback scoped by AI discretion
- Hardware access surviving rollback

Any of these constitutes a system breach.

---

## 14. Final Rule (Constitutional)

> **If a system cannot roll back safely, it must not proceed.**

Rollback is not optional.  
Rollback is the guarantee that makes autonomy survivable.

---

## END OF DOCUMENT