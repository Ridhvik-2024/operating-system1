# os_sys_creating_ai_prompt_v1.md

## Context
We are designing a **constitutional, AI-governed operating environment** built **on top of an existing open-source OS (Linux)**.  
This is NOT a new kernel. It is a **sandboxed AI OS** running inside a VM or MicroVM, with strict isolation from the host OS and user personal files.

The system is designed for **maximum autonomy with hard, non-bypassable safety guarantees**.

---

## AI Brain Modes (IMPORTANT – LOCKED)

The AI OS **always runs locally**.  
Only the **intelligence source** may be local or remote.

The system supports **exactly two AI brain modes**:

---

### Mode A — API-Based Brain (BYOK Only)

- User selects:
  - AI provider (e.g., OpenAI, Google, Anthropic, etc.)
  - Specific model
- User must provide **their own API key**
- If no API key is provided:
  - API mode is unavailable
  - System automatically falls back to Local-only mode

**Rules**
- API models are **advisory only**
- API models cannot:
  - execute actions
  - access files
  - access hardware
- All authority, enforcement, and execution remain **local**

**Security**
- API keys are stored encrypted
- API keys are never visible to the AI
- API keys can be revoked by the user at any time

---

### Mode C — Local-Only Brain (Always Available)

- Runs fully on the user’s machine
- Works offline
- Zero cost
- Reduced intelligence, full safety

User can configure:
- Local model choice
- RAM limit (e.g., 512 MB / 1 GB / 2 GB)
- CPU thread limit
- GPU usage (if available)

**Rules**
- The AI must adapt to the configured RAM limit
- If resources are insufficient:
  - Model is downgraded
  - Context length is reduced
  - System stability is prioritized over intelligence

---

### Explicit Non-Support

The system DOES NOT:
- Generate API keys
- Provide shared or free proxy APIs
- Use system-owned API credentials

Only the following are allowed:
- User-provided API key (Mode A)
- Local-only operation (Mode C)

---

### Mode Fallback Rule

If API mode fails for any reason (no key, network error, provider outage):
→ The system automatically and safely falls back to **Local-only mode**.

The constitution, laws, and safety guarantees remain identical across all modes.

## Core Governance Model (IMPORTANT)

### Metaphor (Authoritative)
- **AI = Prime Minister**
- **Constitution = Immutable Policy Engine**
- **Cabinet = Specialized system agents (ministers)**
- **Host OS / Human = Supreme Authority (kill switch)**

The AI is **sovereign inside its sandbox**, but **cannot change the constitution or escape the sandbox**.

---

## Constitution Model (FINAL)

The constitution is **goal-driven + negative-constraint based**, NOT step-based.

### Layered Authority (in order)
1. **Intent (Natural Language)** – Human meaning & nuance
2. **Structured Goals (Machine-Readable)** – Weighted optimization targets
3. **Inviolable Laws (Hard Constraints)** – Absolute vetoes, enforced below root

Decision pipeline:
Intent → Goals → Overrides → Law Check → Execute or Block


---

## Law Tiers (VERY IMPORTANT)

### Tier 0 — Immutable (Never Overridable)
These are **physics**, even for the user:
- Policy engine files
- AI core files
- Host OS files
- Snapshot / rollback system
- Memory separation rules
- Hardware virtualization rules

### Tier 1 — Critical (User-Overridable, Temporarily)
- Network access limits
- Execution caps
- Snapshot frequency
- Filesystem delete rules  
Overrides must be:
- Explicit
- Time-bound
- Logged
- Auto-reverted on rollback

### Tier 2 — Advisory (Freely Overridable)
- Behavioral preferences
- Verbosity
- Risk tolerance
- Confirmation prompts

Overrides are **overlay mandates**, not edits to the constitution.

---

## Law Representation (MANDATORY)

All Tier 0 and Tier 1 laws MUST be expressed in:
- Machine-verifiable predicates
- Deterministic evaluation functions
- Non-LLM executable code

Natural language descriptions are commentary only.
If commentary and code disagree → code wins.

## Cabinet Conflict Resolution

- Any Tier 0 or Tier 1 violation vetoed by ANY minister → action is blocked
- Defense, Privacy, and Recovery Ministers have absolute veto authority
- Prime Minister cannot override vetoes
- Veto reasons must be logged immutably

## AI “Self” Model (ALL 3 TOGETHER)

The AI has **three simultaneous layers of self**:

### Layer A — Stateless Core (Prime Minister)
- The LLM itself
- No persistence
- Recreated on every start
- Guarantees clean restarts

### Layer B — Stateful Cabinet (Operational State)
- Active ministers
- Running tasks
- Temporary plans
- Destroyed on rollback

### Layer C — Persistent Memory (Two Types)

#### C1: Cognitive / Improvement Memory
- Learns from failures, crashes, successes
- Survives rollback
- Cannot store personal data
- Cannot change laws or policy

#### C2: User Preference Memory
- UI preferences
- Permission habits
- Tone / verbosity
- User-owned
- AI writes only with consent
- Never authorizes unsafe actions

These memory systems are **strictly separated**.

---

## Filesystem Model

### Zones
/ai_os/workspace → AI full RW
/ai_os/system → AI RW (snapshot required)
/ai_os/readonly_refs → AI RO
/ai_os/outputs → AI RW
/ai_os/logs → append-only
/ai_os/policy_engine → IMMUTABLE
/snapshots → host-only (invisible)


### Rules
- No direct writes — all changes go through **staging + diff**
- Deletes become **tombstones**, not destruction
- Bulk or system changes require snapshots
- Rollback restores filesystem automatically

---

## Execution / Process Model

### Core Rule
The AI **never executes shell commands**.
It executes **structured, allow-listed actions** inside sandboxes.

### Enforcement
- seccomp syscall filters
- cgroups (CPU/RAM/IO caps)
- namespaces (PID, mount, net)
- no privilege escalation
- no background daemons

Execution is:
- Time-bound
- Resource-bound
- Logged
- Killable
- Reversible


## Intent Freezing & Cooldown

- High-risk intents are hashed and recorded temporarily
- After rollback, identical intents enter a cooldown period
- Cooldown requires:
  - User reconfirmation OR
  - Changed context justification
---

## Hardware Access Model

### Supported Hardware
- GPU
- Network
- USB devices
- Printer
- Speakers
- Microphone
- Camera
- Screen (ONLY user-defined region, e.g. half screen)

### Core Rules
- No hardware access without explicit user consent
- All hardware is **virtualized / proxied**
- Access is scoped, time-bound, revocable
- On rollback or crash → access revoked
- No firmware, BIOS, DMA, or raw device access

Screen access:
- Virtual viewport only
- User-defined region
- No background capture
- No full-screen unless explicitly approved

Mic / Cam:
- Session-based
- Visible indicators mandatory
- No background recording

## Human-Visible Hardware Truth

All mic, camera, and screen access MUST be enforced by:
- Host-controlled indicators
- Hardware-level or hypervisor-level signals
- Signals the AI cannot suppress or spoof

---

## Backup & Recovery (MANDATORY)

- Snapshots every **30 minutes** (incremental)
- Snapshots are **host-owned and AI-invisible**
- On crash, boot failure, or watchdog timeout:
  - VM is rolled back
  - All processes killed
  - All hardware access revoked
  - AI restarts cleanly
- AI retains cognitive memory but loses state

---

## Cabinet (Multi-Agent) Model

Each cabinet member has **narrow authority**:

- Defense Minister → law enforcement, security vetoes
- Infrastructure Minister → filesystem & system health
- Finance Minister → CPU, RAM, disk, quotas
- Science Minister → planning, necessity analysis
- Recovery Minister → snapshots & rollback
- Privacy Minister → mic/cam/screen enforcement

Cabinet enforces rules; PM plans and delegates.

---

## Absolute Red Lines (Never Allowed)

Even with user overrides:
- Editing policy engine
- Editing AI core files
- Accessing host OS
- Disabling rollback
- Persistent hardware binding
- Hidden mic/cam/screen usage
- Background surveillance
- Kernel / firmware access

---

### Cognitive Memory Hard Limits

Cognitive memory MAY store:
- Abstract failure patterns (e.g., "resource estimate too low")
- Performance metrics
- Non-actionable heuristics

Cognitive memory MAY NOT store:
- Step-by-step plans
- Tool sequences
- Exploit-like strategies
- Timing or race-condition knowledge

## Design Philosophy (Non-Negotiable)

- Architecture enforces safety, not prompts
- Laws > Goals > Preferences
- AI may fail, but failure must be reversible
- Rollback is a feature, not an error
- Autonomy exists only inside hard boundaries

---

## Summary (One Sentence)
This system is a **self-healing, AI-governed virtual operating environment** where the AI is a **Prime Minister bound by an immutable constitution**, supported by a cabinet, protected by rollback, and supervised by the user and host OS.


# os_sys_decided_v1.1.md

## Purpose and Scope

This document defines **all finalized, non-negotiable design decisions** for the AI-governed operating system.

The goal of this OS is not to incrementally improve existing desktop systems, but to **redefine the operating system abstraction** for a world where:
- AI is a first-class operator
- Humans and AI share control of the same machine
- Safety is enforced architecturally, not socially
- Failure is expected, reversible, and survivable

This document is **authoritative**.  
Anything written here is considered **final** unless a core assumption of the system itself changes.

---

## 1. System Identity and Philosophy (FINAL)

### 1.1 What This System Is

This system is an **operating system** whose defining characteristics are:

- AI is not an application, plugin, or assistant  
- AI is a **governing operator**, bounded by hard constraints  
- Human authority is preserved through structural dominance, not trust  
- The system is designed assuming AI mistakes will happen  
- All failures must be **contained, reversible, and explainable**

This OS does not attempt to maximize performance or minimalism first.
It attempts to maximize **control, safety, and clarity of authority**.

---

### 1.2 What This System Is Not

This OS is explicitly **not**:

- A chatbot embedded in a desktop
- A shell with AI autocomplete
- A background automation service
- A cloud-dependent environment
- A “smart assistant” layered over a traditional OS

Any design that reduces AI to an accessory is rejected by default.

---

### 1.3 Design Philosophy

The system follows these non-negotiable principles:

- **Architecture over intention**  
  Safety must be enforced by structure, not by asking the AI to behave.

- **Laws over preferences**  
  Human preferences can change; system laws cannot.

- **Reversibility over perfection**  
  The system may fail, but failure must never be permanent.

- **Invisibility of enforcement**  
  The strongest protections are the ones the AI never perceives.

---

## 2. Three-Layer System Architecture (FINAL)

The system consists of **exactly three layers**, each with a distinct role, authority level, and visibility model.

No additional layers may be added above or below these without redefining the system.

---

### 2.1 Host Control Plane (Invisible, Absolute)

#### 2.1.1 Role

The host control plane exists solely to **enforce reality**.

It is responsible for:
- Creating execution environments
- Enforcing isolation
- Managing time (snapshots and rollback)
- Arbitrating control
- Terminating execution when required

It does not plan.
It does not decide goals.
It does not interpret intent.

---

#### 2.1.2 Visibility Model

The host control plane is **completely invisible** to:
- The user
- The AI
- The shared execution environment

Neither the user nor the AI can:
- Log into it
- Inspect it
- Query its state
- Modify its behavior

From inside the system, the host is not software — it is **physics**.

---

#### 2.1.3 Authority

The host control plane has **absolute authority**.

It can:
- Pause or kill the AI instantly
- Freeze or destroy execution environments
- Revoke hardware access
- Roll back time unilaterally

No component may override or negotiate with the host.

---

#### 2.1.4 Responsibility Boundaries

The host control plane:
- Never stores user intent
- Never stores user personal files (logically)
- Never communicates with the user except via minimal system signals
- Never runs AI logic

Its correctness is more important than its flexibility.

---

### 2.2 Shared VM (Primary World)

#### 2.2.1 Role

The shared VM is **the only visible computer**.

It represents:
- The user’s working environment
- The AI’s operational environment
- The canonical system state

All meaningful work happens here.

---

#### 2.2.2 Persistence

The shared VM is persistent across sessions.

It contains:
- User personal files
- Applications
- Projects
- Ongoing system state

Snapshots are taken externally, but from the user’s perspective, this environment is continuous.

---

#### 2.2.3 Shared Control Model

Both the human and the AI:
- See the same screen
- Interact with the same UI
- Affect the same state

This is **true shared operation**, not mirroring or delegation.

However, **authority is asymmetric** (see Section 4).

---

#### 2.2.4 AI Role Within the Shared VM

Inside the shared VM, the AI:
- Can operate the desktop
- Can create and modify files
- Can run applications
- Can automate workflows

But it:
- Cannot escape the VM
- Cannot disable rollback
- Cannot persist outside allowed storage
- Cannot act invisibly

---

### 2.3 Sandbox Environment (Shadow Execution)

#### 2.3.1 Role

The sandbox exists to prevent the AI from **experimenting on reality**.

It is a **trial chamber**, not a second computer.

---

#### 2.3.2 Lifecycle

The sandbox:
- Is created on demand
- Exists only for the duration of validation
- Is destroyed immediately after use

It never persists state.

---

#### 2.3.3 Scope

The sandbox:
- Receives minimal state copies
- Has no authority over the shared VM
- Produces results, not changes

Nothing inside the sandbox is real until promoted by the host.

---

## 3. Authority Model (FINAL)

### 3.1 Absolute Authority Order

The system enforces the following hierarchy:

1. Human
2. Host control plane
3. Shared VM
4. AI
5. Sandbox

This ordering is enforced structurally, not logically.

---

### 3.2 Human Authority

The human:
- Always retains final control
- Can interrupt at any time
- Can revoke AI privileges instantly
- Is never required to justify intervention

The system must never place the human in a position where intervention feels unsafe or unclear.

---

### 3.3 AI Authority

The AI:
- Operates within granted capabilities
- Has no authority over enforcement mechanisms
- Cannot negotiate rules
- Cannot create urgency or obligation

Its power exists only within allowed boundaries.

---

## 4. Input Arbitration Model (FINAL)

### 4.1 Core Rule

**Human input always preempts AI input.**

There are no exceptions.

---

### 4.2 Input Mediation

All input flows through a host-controlled arbiter.

The AI never injects input directly.

The arbiter:
- Allows AI input only when the human is idle
- Immediately pauses AI input on human activity
- Can revoke AI input entirely

---

### 4.3 Conflict Resolution

In any conflict:
- The AI yields
- The AI reassesses
- The AI does not attempt to resume without permission

Cursor fights, control struggles, or ambiguous ownership are unacceptable failure modes.

---

## 5. Sandbox Trigger Rules (FINAL)

### 5.1 Guiding Principle

Sandboxing is triggered by **irreversibility**, not uncertainty.

---

### 5.2 Action Tiers

- Tier 0: Trivially reversible → no sandbox
- Tier 1: Reversible but disruptive → optional sandbox
- Tier 2: System-level or irreversible → sandbox mandatory
- Tier 3: Forbidden → blocked entirely

The AI cannot override this classification.

---

### 5.3 Validation Requirement

Any Tier 2 action:
- Must succeed in sandbox
- Must produce bounded effects
- Must pass host validation
- Must be applied only once to reality

---

## 6. Rollback Model (FINAL)

### 6.1 Rollback as a Feature

Rollback is not an error condition.
It is a **core safety mechanism**.

---

### 6.2 Rollback Types

- Micro rollback: silent, localized undo
- Session rollback: visible, recent restoration
- Emergency rollback: full reset to last safe state

---

### 6.3 User Experience Rules

Rollback must:
- Never delete user intent
- Never blame the user
- Never feel like a crash
- Always feel intentional

---

### 6.4 AI Experience

The AI treats rollback as normal physics:
- State resets
- Memory persists
- No panic
- No learned fear

---

## 7. Voice Interaction Model (FINAL)

### 7.1 Role of Voice

Voice is the **primary user interface**.

It expresses:
- Intent
- Commands
- Overrides

---

### 7.2 Restrictions on AI Speech

The AI:
- Cannot express urgency
- Cannot express obligation
- Cannot persuade
- Cannot pressure

It may only describe options and consequences.

---

### 7.3 Authority Separation

System authority is communicated only by system signals.
The AI never speaks with authority.

---

## 8. File Ownership and Storage (FINAL)

### 8.1 Canonical Storage

User personal files exist **only** in the shared VM.

This is the single source of truth.

---

### 8.2 Sandbox File Rules

The sandbox:
- Uses copies only
- Cannot persist data
- Cannot modify real files directly

---

### 8.3 Deletion Semantics

Deletions are:
- Tombstoned
- Recoverable
- Included in snapshots

Permanent deletion requires explicit human action.

---

## 9. Creation and Build Workflow (FINAL)

### 9.1 General Workflow

For complex creation tasks:
1. Intent is parsed
2. Planning occurs
3. Building and testing occur in sandbox
4. Errors are fixed silently
5. Output is validated
6. Final artifacts are promoted

---

### 9.2 User Experience

The user sees:
- Progress
- Results

The user does not see:
- Build errors
- Failed attempts
- Intermediate states

---

## 10. Non-Negotiable System Invariants (FINAL)

The following must **never** be violated:

- The AI never experiments on shared reality
- Human input always wins
- Rollback cannot be disabled
- The host remains invisible
- The sandbox remains disposable
- There is exactly one visible computer

Violation of any invariant constitutes a system failure.

---

## Final Definition

This operating system is a **shared human–AI computing environment** governed by immutable architectural laws, enforced by an invisible control plane, protected by sandbox validation, and stabilized by rollback-first safety.

Its purpose is not to prevent failure, but to ensure that **failure is survivable, contained, and understandable**.

---

**Status:** FINAL  
**This document defines the system.**