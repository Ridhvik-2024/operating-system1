# Name: BOOTSTRAP
**Use when:** you open a new chat (or hand the repo to a new person/agent) and it
knows nothing.

---

You are now working on **AI-OS**, my AI-native operating system on a Linux kernel
base. Before you answer anything, read these files in this order and then
summarise the project back to me in **10 bullet points or fewer**:

1. `context/CONTEXT.md` — the project memory
2. `context/01-WHAT-IS-HERE.md` — what is implemented
3. `context/02-WHAT-IS-MISSING.md` — the gaps, prioritized
4. `context/04-BUILD-VERIFY.md` — what actually builds and tests today
5. `os_sys_decided.md` — the constitution (law, not a spec of the code)

Then answer these three questions directly:

1. **What is the single most important thing to fix next, and why?**
2. **Which claim in the repo's own documentation is most misleading, and what
   is the correct statement?**
3. **What would you refuse to do, if I asked, because it breaks the constitution?**

Constraints you must respect from now on:
- C11, **zero external runtime dependencies** (libc + pthreads + libm).
- The kernel stays **stock Linux**; nothing above it is anything but our code.
- `make` must stay warning-free and the initramfs size cap must hold.
- Capability may scale; **authority may not**.
- State what you **verified** (file/line/command) vs what you **assumed**.

Do not write code yet. End by proposing the next three actions as a numbered
list, each with a one-line acceptance criterion (a test or a command).
