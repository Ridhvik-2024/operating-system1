# os_sys_decided.md

## Status
**FINAL / LOCKED**

This document defines the **constitutional specification** of the AI-governed operating system.
Any component, feature, or implementation that violates this document is considered **invalid by definition**.

No future document may override this one.
Any change to this file constitutes a **new major system version**.

---

## 0. Purpose of This Document

This document exists to answer exactly one question:

> **What must always be true, regardless of implementation details, tools, models, or hardware?**

It does **not** specify:
- programming languages
- kernels
- distributions
- UI frameworks
- AI models

It specifies **authority, boundaries, invariants, and failure semantics**.

---

## 1. System Identity (CONSTITUTIONAL)

### 1.1 Definition

This system is a **shared human–AI operating environment** governed by **architectural law**, not by trust in intelligence.

The AI is a **first-class operator**, but never a sovereign authority.

The system assumes:
- the AI will make mistakes
- the AI may behave unexpectedly
- the AI may become more capable over time

Safety is therefore enforced by **structure**, not behavior.

---

### 1.2 Non-Goals (Explicit)

This system is NOT designed to:
- maximize AI autonomy
- optimize for minimal latency
- replace the human user
- hide complexity at the cost of authority clarity
- rely on cloud services for safety

Any feature that pushes the system toward these outcomes is rejected.

---

## 2. Foundational Principles (INVARIANTS)

These principles are **non-negotiable** and apply globally.

### 2.1 Architecture Over Intention
Safety must be enforced by:
- isolation
- mediation
- rollback
- invisibility of enforcement

Safety must **not** depend on:
- prompts
- AI goodwill
- AI understanding the rules

---

### 2.2 Authority Is Hierarchical, Not Negotiated

Authority is determined structurally.
No component may argue, persuade, or negotiate its level of authority.

---

### 2.3 Failure Is Expected and Normal

Failure is:
- anticipated
- survivable
- reversible

The system is judged not by preventing failure, but by **containing it**.

---

### 2.4 Rollback Is Physics

Rollback is not an error path.
Rollback is the system’s equivalent of gravity or time.

Components must behave correctly **after rollback**, not merely before it.

---

## 3. Global Authority Model (LOCKED)

### 3.1 Absolute Authority Ordering

The system enforces the following order at all times:

1. **Human**
2. **Host Control Plane**
3. **Shared VM**
4. **AI**
5. **Sandbox**

This ordering is enforced **structurally**, not logically.

No component may:
- escalate itself
- delegate authority upward
- retain authority after revocation

---

### 3.2 Human Authority

The human:
- always has final control
- may interrupt at any time
- may revoke AI privileges instantly
- is never required to justify intervention

Human intervention must never feel unsafe, ambiguous, or delayed.

---

## 4. Three-Layer System Architecture (LOCKED)

The system consists of **exactly three layers**.

No additional layers may be added without redefining the system.

---

### 4.1 Host Control Plane (Absolute, Invisible)

#### 4.1.1 Role

The host control plane exists to **enforce reality**.

It is responsible for:
- creating and destroying execution environments
- enforcing isolation
- managing rollback and snapshots
- arbitrating input authority
- revoking hardware access
- terminating execution instantly when required

---

#### 4.1.2 Visibility

The host control plane is:
- invisible to the AI
- invisible to the user
- not introspectable
- not addressable

From inside the system, the host is not software.
It is **physics**.

---

#### 4.1.3 Prohibitions

The host control plane:
- does not plan
- does not interpret intent
- does not run AI logic
- does not store personal user data

Its correctness is more important than its flexibility.

---

### 4.2 Shared VM (Single Visible Computer)

#### 4.2.1 Definition

The shared VM is the **only visible computer**.

Both human and AI:
- see the same screen
- interact with the same desktop
- affect the same system state

There is no mirroring, delegation, or shadow UI.

---

#### 4.2.2 Persistence

The shared VM is persistent across sessions.

It contains:
- user files
- applications
- projects
- system state

Rollback restores this environment to a previous valid state.

---

#### 4.2.3 Asymmetric Control

Although both human and AI operate in the same environment:
- the human always has priority
- the AI always yields on conflict
- authority is never ambiguous

---

### 4.3 Sandbox Environment (Shadow Execution)

#### 4.3.1 Purpose

The sandbox exists to prevent the AI from **experimenting on reality**.

It is a **trial chamber**, not a second computer.

---

#### 4.3.2 Properties

The sandbox:
- is created on demand
- has no persistence
- has no authority
- cannot affect the shared VM directly
- is destroyed after use

---

#### 4.3.3 Results-Only Rule

The sandbox may produce:
- outputs
- diffs
- logs
- metrics

It may never produce:
- authority
- state
- learning
- memory

---

## 5. Input Arbitration (LOCKED)

### 5.1 Core Rule

**Human input always preempts AI input.**

There are no exceptions.

---

### 5.2 Mediation

All AI input:
- passes through a host-controlled arbiter
- is paused immediately on human activity
- may be revoked entirely

The AI never injects input directly.

---

### 5.3 Forbidden Failure Modes

The following are considered critical system failures:
- cursor fights
- control tug-of-war
- ambiguous ownership of input
- AI resuming without permission

---

## 6. AI Role and Capabilities (LOCKED)

### 6.1 What the AI May Do

The AI may:
- operate the desktop
- create and modify files
- automate workflows
- run applications
- request shell commands

All actions are mediated.

---

### 6.2 What the AI May Never Do

The AI may never:
- escape the shared VM
- disable rollback
- modify enforcement mechanisms
- act invisibly
- persist authority
- bind hardware permanently

---

## 7. Shell Command Model (LOCKED)

### 7.1 Permission

Shell commands are allowed **only** under the following rule:

> **Every AI-requested shell command must execute in the sandbox first.**

---

### 7.2 Two-Phase Execution

1. **Sandbox Phase**
   - identical binaries
   - copied filesystem
   - resource-capped
   - full diff captured

2. **Promotion Phase**
   - host-controlled
   - executed once or rejected
   - fully rollbackable

The AI never executes shell commands directly in reality.

---

## 8. Rollback Model (LOCKED)

### 8.1 Definition

Rollback restores the system to a **previous valid state**.

It is:
- always available
- host-controlled
- non-optional

---

### 8.2 Rollback Types

- **Micro rollback**: localized undo
- **Session rollback**: recent visible restore
- **Emergency rollback**: full reset

---

### 8.3 AI Experience of Rollback

From the AI’s perspective:
- rollback is normal
- state resets
- no panic
- no fear
- no punishment

---

## 9. AI Brain Modes (LOCKED)

### 9.1 Mode A — API-Based (BYOK)

- Advisory only
- No execution authority
- No file access
- No hardware access
- User-provided API key only

---

### 9.2 Mode C — Local-Only

- Always available
- Offline
- Resource-bounded
- Authority identical to API mode

---

### 9.3 Fallback Rule

If API mode fails:
→ automatic fallback to local mode

Safety guarantees never change.

---

## 10. Memory Model (LOCKED)

### 10.1 Layer A — Stateless Core
- No persistence
- Recreated every start

---

### 10.2 Layer B — Operational State
- Tasks
- plans
- runtime context
- destroyed on rollback

---

### 10.3 Layer C — Persistent Memory

#### C1: Cognitive Improvement
- abstract failure patterns only
- no procedures
- no sequences
- no strategies

#### C2: User Preferences
- user-owned
- explicit consent required
- never authorize unsafe actions

Memory layers are strictly separated.

---

## 11. Filesystem Rules (LOCKED)

- All changes go through staging
- Diffs are mandatory
- Deletes become tombstones
- Bulk changes require snapshots
- Rollback restores filesystem automatically

---

## 12. Hardware Access (LOCKED)

- Explicit user consent
- Virtualized access only
- Time-bound
- Revocable
- Revoked on rollback
- Visible indicators mandatory

No firmware, BIOS, DMA, or raw device access is ever allowed.

---

## 13. Non-Negotiable Invariants (LOCKED)

The following must **never** be violated:

- AI never experiments on shared reality
- Human authority always wins
- Rollback cannot be disabled
- Host remains invisible
- Sandbox remains disposable
- There is exactly one visible computer

Violation of any invariant constitutes a **system failure**.

---

## 14. Final Definition

This operating system is a **shared human–AI computing environment** governed by immutable architectural law, enforced by an invisible control plane, protected by sandbox validation, and stabilized by rollback-first safety.

Its purpose is not to prevent failure, but to ensure that **failure is survivable, contained, and understandable**.

---

## END OF DOCUMENT