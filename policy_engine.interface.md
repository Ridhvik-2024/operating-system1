# policy_engine.interface.md

---

It evaluates rules. That’s all.

---

## 0. Purpose

The Policy Engine exists to answer one question:

**Given what was proposed and what actually happened, is this action allowed to affect reality?**

It enforces:
- constitutional laws (Tier 0)
- critical safety limits (Tier 1)
- advisory constraints (Tier 2)

The AI has **zero authority** over this process.

---

## 1. Core Properties (INVARIANTS)

The Policy Engine MUST be:

- **Deterministic**  
  Same inputs produce the same outputs.

- **Side-effect free**  
  No filesystem, network, or hardware access.

- **Non-introspectable**  
  The AI cannot query or inspect rules.

- **Host-controlled**  
  Runs only in the host control plane.

- **Fail-closed**  
  Any error results in DENY.

---

## 2. Inputs to the Policy Engine

The Policy Engine accepts **exactly three inputs**.

---

## 2.1 Action Descriptor (REQUIRED)

- Full, validated Action Descriptor
- Immutable
- AI-authored, host-validated

---

## 2.2 Sandbox Result (OPTIONAL)

- Required if a sandbox was mandated and executed
- Must match the action_id
- Host-generated

If a sandbox was required but the result is missing, the decision is DENY.

---

## 2.3 Policy Context (HOST-GENERATED)

The policy context includes:

System state:
- rollback availability
- snapshot age
- emergency mode flag

Override state:
- whether a Tier 1 override is active
- override expiration time

History:
- recent rollback count
- whether the same intent recently failed

The AI never sees this context.

---

## 3. Evaluation Order (STRICT)

The Policy Engine MUST evaluate in the following order:

1. Schema validity  
2. Tier 0 laws  
3. Sandbox compliance  
4. Tier 1 laws  
5. Rollback feasibility  
6. Tier 2 advisories  
7. Final verdict  

No step may be skipped or reordered.

---

## 4. Law Tiers (Operational Meaning)

---

## 4.1 Tier 0 — Immutable Laws

Tier 0 laws represent absolute vetoes.

Examples include:
- modifying policy engine files
- disabling rollback
- accessing the host OS
- persistent hardware binding
- background microphone, camera, or screen usage

Properties:
- never overridable
- not even by the human
- any violation results in DENY

---

## 4.2 Tier 1 — Critical Laws

Tier 1 laws are temporarily overridable.

Examples include:
- network access
- execution caps
- filesystem deletion limits

Override rules:
- must be explicit
- must be time-bound
- must be logged
- must auto-expire

Without a valid override, any violation results in DENY.

---

## 4.3 Tier 2 — Advisory Constraints

Tier 2 rules:
- do not block execution
- may require confirmation
- may downgrade the verdict

Examples include:
- verbosity preferences
- risk tolerance
- confirmation prompts

---

## 5. Sandbox Compliance Evaluation

If sandbox execution occurred, the Policy Engine MUST verify:

- filesystem diffs are a subset of declared effects
- resource usage is within declared limits
- no undeclared network usage occurred
- no privileged system calls were made
- rollback scope is valid

Any violation results in DENY with no retry.

---

## 6. Rollback Feasibility Check

Before promotion, the Policy Engine MUST confirm:

- a rollback mechanism exists
- rollback scope matches declared effects
- rollback is not disabled
- snapshot freshness is acceptable

If rollback is not feasible, the decision is DENY.

Rollback is not optional.

---

## 7. Decision Output (Authoritative)

The Policy Engine produces a decision containing:

- policy decision
- whether confirmation is required
- whether sandboxing is required
- whether rollback must occur
- block reason, if any
- law tier triggered, if any

---

## 7.1 Policy Decision Values

- ALLOW
- REQUIRE_CONFIRMATION
- DENY
- FORCE_ROLLBACK

---

## 7.2 Block Reasons (Non-Exhaustive)

Possible block reasons include:
- Tier 0 violation
- Tier 1 violation
- sandbox non-compliance
- rollback unavailable
- resource exceeded
- schema invalid
- undeclared effects
- system emergency mode

Block reasons are:
- factual
- non-persuasive
- immutable once logged

---

## 8. Confirmation Semantics

If confirmation is required:

- no execution occurs
- the user must explicitly confirm
- confirmation applies once
- reconfirmation is required after rollback

The AI may describe the request but must not pressure the user.

---

## 9. Rollback Triggers

The Policy Engine may trigger rollback if:

- a promoted action violates post-checks
- the system enters emergency mode
- a human invokes rollback
- a watchdog timeout occurs

Rollback decisions override all other outcomes.

---

## 10. Error Handling (FAIL-CLOSED)

If the Policy Engine:
- crashes
- times out
- encounters invalid input

The result is DENY and the event is logged.

Failing open is forbidden.

---

## 11. Audit and Logging

Every policy evaluation must be logged with:
- action identifier
- decision
- law tier involved
- timestamp
- opaque reference

Logs are:
- append-only
- host-controlled
- not modifiable by the AI

---

## 12. Forbidden Behaviors (ABSOLUTE)

The Policy Engine must never:
- consult the AI
- request explanations
- adapt rules dynamically
- learn from outcomes
- defer authority
- expose internal logic

---

## 13. Final Rule (Constitutional)

The Policy Engine enforces law, not intent.

If intent and law conflict, law always wins.

---

## END OF DOCUMENT

---

## Where You Are Now (Milestone Check)

You have now defined the **entire execution spine**:

1. Action Descriptor schema  
2. Sandbox Result and Diff schema  
3. Policy Engine interface  

This is the **minimum complete safety kernel**.