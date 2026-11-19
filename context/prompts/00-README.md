# prompts/ — named, copy-paste prompts for AI-OS

Each file is one prompt with a **name**, **when to use it**, and the text to
paste. Keep them short; they assume the recipient has read
`context/CONTEXT.md` (or `CONTEXT.chatgpt.md` / `CONTEXT.agent.md`).

| Name | File | Use when |
|---|---|---|
| `BOOTSTRAP` | `01-bootstrap-new-session.md` | starting a brand-new chat with any AI |
| `ARCHITECT` | `02-architect-review.md` | you have a design and want it attacked against the constitution |
| `MEDIATOR-SPEC` | `03-mediator-spec.md` | turning the constitution into M11 (descriptor/policy/staging/rollback) |
| `BUILD-FIX` | `04-build-and-fix.md` | a coding agent should make the tree build + test green |
| `LINUX-ADOPT` | `05-linux-primitives.md` | deciding *which* Linux subsystem replaces which invention |
| `AUDIT-CONSTITUTION` | `06-constitution-audit.md` | are we still compliant? periodic review |
| `DESKTOP-APP` | `07-desktop-app.md` | adding a window/app to `aios-desktop` |
| `HANDOFF` | `08-handoff-update.md` | end of a session: update the context folder |
| `SINGLE-CARD` | `09-single-card.md` | handing **one** task card to ChatGPT for a reviewable diff |

**Rules for all prompts**

1. Always name the **files** the AI should read before answering.
2. Always ask for **acceptance criteria** — a command or a test, not an opinion.
3. Never let the AI edit the constitution (`os_sys_decided.md`, the four
   contracts). Those change only with an explicit "new major version" decision.
4. Ask the AI to state what it **verified** vs **assumed**.
