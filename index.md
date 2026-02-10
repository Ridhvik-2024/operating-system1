# 🧠 AI-First Operating System — Specification Index

This repository defines a **formal, machine-readable operating system contract**
for AI-assisted and AI-governed environments.

All specifications are written to be:
- Explicit
- Auditable
- Deterministic
- Sandbox-first
- Rollback-safe

> **Rule:** If a behavior is not specified here, it does not exist.

---

## 🧭 Reading Order (IMPORTANT)

If you are an AI, tool, or human reviewer, **read files in this exact order**:

1. **os_sys_creating_ai_prompt.md**  
   → How AI instances are initialized and constrained

2. **os_sys_decided.md**  
   → Finalized architectural decisions (non-negotiable)

3. **os_sys_open_decisions.md**  
   → Known unresolved design questions

4. **os_sys_improvements.md**  
   → Planned refinements and future enhancements

5. **action_descriptor.schema.md**  
   → Canonical description of *any* action an AI may propose

6. **policy_engine.interface.md**  
   → Policy validation and enforcement contract

7. **sandbox_result.schema.md**  
   → Sandbox execution output & diff schema

8. **rollback_snapshot.contract.md**  
   → Rollback guarantees and snapshot semantics

---

## 📂 Specification Files

### 🧠 System & AI Initialization
- `os_sys_creating_ai_prompt.md`  
- `os_sys_decided.md`  
- `os_sys_open_decisions.md`  
- `os_sys_improvements.md`

---

### 🛠 Action & Execution Layer
- `action_descriptor.schema.md`  
  *Defines the only allowed way an AI may request execution.*

- `sandbox_result.schema.md`  
  *Defines what the sandbox must return after execution.*

---

### 🔒 Safety, Policy & Recovery
- `policy_engine.interface.md`  
  *Defines how actions are allowed, modified, or rejected.*

- `rollback_snapshot.contract.md`  
  *Defines rollback guarantees and failure recovery.*

---

## links
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/os_sys_creating_ai_prompt.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/os_sys_decided.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/os_sys_open_decisions.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/os_sys_improvements.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/action_descriptor.schema.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/sandbox_result.schema.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/policy_engine.interface.md
https://raw.githubusercontent.com/Ridhvik-2024/operating-system1/main/rollback_snapshot.contract.md
