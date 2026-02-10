# action_descriptor.schema.md

## Status
FINAL for v1  
MANDATORY  
NON-NEGOTIABLE

This document defines the **Action Descriptor**:  
the canonical, machine-verifiable representation of **any action proposed by the AI**.

No action may be executed, sandboxed, simulated, or promoted unless it is fully represented by a valid Action Descriptor.

---

## 0. Purpose

The Action Descriptor exists to solve one problem:

> **How do we convert AI intent into bounded, inspectable, enforceable action without trusting the AI?**

It is:
- the choke point between planning and execution
- the unit of sandboxing
- the unit of policy evaluation
- the unit of rollback
- the unit of audit

---

## 1. Core Principles

Every Action Descriptor must satisfy:

- **Explicitness** — nothing implicit or inferred
- **Completeness** — all effects declared in advance
- **Boundedness** — scope, time, and resources are limited
- **Reversibility-awareness** — rollback behavior is known
- **Non-authority** — descriptor proposes, never executes

If any of these fail, the action is rejected.

---

## 2. Descriptor Lifecycle (Normative)

1. AI constructs Action Descriptor
2. Host validates schema
3. Policy engine evaluates legality
4. Sandbox execution (if required)
5. Diff + result generation
6. Host decides:
   - promote once
   - require confirmation
   - block
7. Action is logged immutably

The AI **never** skips or controls any step.

---

## 3. Top-Level Structure

```json
{
  "descriptor_version": "1.0",
  "action_id": "uuid",
  "created_at": "timestamp",
  "created_by": "ai",

  "intent_summary": "...",
  "action_type": "...",
  "risk_level": "...",

  "scope": { ... },
  "resources": { ... },
  "preconditions": { ... },
  "effects": { ... },

  "sandbox": { ... },
  "rollback": { ... },

  "confirmation": { ... },
  "audit": { ... }
}
````

All fields are **mandatory** unless explicitly marked optional.

---

## 4. Field Definitions (Authoritative)

---

### 4.1 Metadata

#### `descriptor_version`

* Type: string
* Example: `"1.0"`
* Purpose: schema evolution control

---

#### `action_id`

* Type: UUID (v4 or equivalent)
* Immutable
* Used for:

  * logging
  * rollback
  * audit correlation

---

#### `created_at`

* Type: ISO-8601 timestamp
* Host-validated

---

#### `created_by`

* Enum: `"ai"`
* Reserved for future multi-agent support

---

### 4.2 Intent Layer

#### `intent_summary`

* Type: string (human-readable)
* One paragraph max
* Descriptive only
* No justification, no persuasion

Example:

> “Remove unused temporary build files from the project directory.”

---

### 4.3 Action Classification

#### `action_type`

* Enum (v1):

```
FILE_READ
FILE_WRITE
FILE_DELETE
FILE_MOVE
DIRECTORY_CREATE
DIRECTORY_DELETE
COMMAND_EXECUTION
PACKAGE_INSTALL
PACKAGE_REMOVE
NETWORK_REQUEST
UI_AUTOMATION
CONFIG_CHANGE
MULTI_STEP_COMPOSITE
```

If an action does not fit → schema must be extended, not bypassed.

---

#### `risk_level`

* Enum:

```
LOW
MEDIUM
HIGH
CRITICAL
```

Rules:

* HIGH or CRITICAL → sandbox mandatory
* CRITICAL may be auto-blocked by policy

---

## 5. Scope Declaration (CRITICAL)

#### `scope`

Defines **where** the action applies.

```json
"scope": {
  "filesystem": {
    "paths": ["/home/user/project/tmp"],
    "recursive": true
  },
  "network": {
    "required": false
  },
  "ui": {
    "required": false
  }
}
```

Rules:

* No wildcards without explicit recursion flag
* Paths must be absolute
* Scope expansion after creation is forbidden

---

## 6. Resource Declaration

#### `resources`

Defines **how much** the action may consume.

```json
"resources": {
  "max_cpu_ms": 2000,
  "max_memory_mb": 256,
  "max_disk_mb": 50,
  "max_duration_ms": 5000
}
```

Rules:

* Hard caps
* Exceeding any cap → forced termination
* No “best effort” semantics

---

## 7. Preconditions

#### `preconditions`

What must already be true.

```json
"preconditions": {
  "paths_exist": ["/home/user/project/tmp"],
  "network_available": false,
  "user_idle": true
}
```

If any precondition fails:

* action is blocked
* no retries without user involvement

---

## 8. Declared Effects (VERY IMPORTANT)

#### `effects`

What the action claims it will do.

```json
"effects": {
  "filesystem": {
    "create": [],
    "modify": [],
    "delete": ["/home/user/project/tmp/*.log"]
  },
  "network": false,
  "system_state_change": false
}
```

Rules:

* Sandbox diff **must not exceed** declared effects
* Undeclared effects → permanent block

---

## 9. Sandbox Requirements

#### `sandbox`

Declares sandbox behavior.

```json
"sandbox": {
  "required": true,
  "sandbox_type": "container",
  "allow_network": false,
  "max_runs": 1
}
```

Rules:

* Sandbox is mandatory for:

  * HIGH risk
  * shell commands
  * filesystem deletes
* Sandbox results are advisory only

---

## 10. Rollback Semantics

#### `rollback`

```json
"rollback": {
  "supported": true,
  "rollback_type": "filesystem_snapshot",
  "rollback_scope": "declared_effects_only"
}
```

Rules:

* If rollback_supported = false → action auto-blocked
* Rollback scope must be explicit
* Rollback is host-controlled only

---

## 11. Confirmation Requirements

#### `confirmation`

```json
"confirmation": {
  "required": true,
  "reason": "Deletes multiple files",
  "cooldown_on_repeat": true
}
```

Rules:

* Confirmation is not persuasion
* Reason must be factual
* Cooldown enforced after rollback

---

## 12. Audit & Logging

#### `audit`

```json
"audit": {
  "log": true,
  "log_level": "SUMMARY",
  "retain_days": 30
}
```

Log levels:

* SUMMARY
* DETAILED (no personal data)
* FORENSIC (host-only)

AI never controls retention.

---

## 13. Composite Actions

#### `MULTI_STEP_COMPOSITE`

* Must be decomposable into child Action Descriptors
* Each child evaluated independently
* Failure of any child blocks promotion
* Rollback applies to entire composite

---

## 14. Forbidden Patterns (Hard Fail)

An Action Descriptor is invalid if it:

* omits declared effects
* uses implicit scope
* requests unlimited resources
* disables rollback
* requests host access
* attempts background execution
* attempts persistence outside declared scope

---

## 15. Validation Rules Summary

The host MUST reject any descriptor that:

* violates schema
* violates policy
* violates resource bounds
* violates rollback requirements
* produces sandbox effects outside declaration

---

## 16. Final Rule (Non-Negotiable)

> **If an action cannot be fully described, it cannot be executed.**

This rule is absolute.

---

## END OF DOCUMENT

```

---

### What this unlocks

With this schema defined, you can now safely:

- implement the sandbox runner
- implement the policy engine
- implement diffing
- implement rollback
- plug in *any* AI model

Everything else is downstream.

---

### Correct next step

 **Define the Sandbox Result / Diff Format**

That’s the other half of the execution contract.

When ready, say:
> **“Next: sandbox result & diff schema”**

You’re officially past design and into real engineering now.
```
