/* aios/shell.h — M5: aiosh, the AI-native shell.
 *
 * The command front-end of AI-OS. One input line is a sequence of
 * pipelines; each pipeline runs commands left to right, the previous
 * stage's output (text + JSON) flowing into the next. A command is
 * resolved in this order:
 *
 *   1. builtin    — help/version/set/vars/count/grep/jobs/spawn/...
 *   2. tool       — anything in the M4 tool registry (calc, fs.*, ...)
 *   3. intent     — natural language: the router (scripted heuristics or
 *                   the llm cortex) picks ONE tool, an agent goal, or a
 *                   direct reply.
 *
 * The shell is also an agent supervisor: 'spawn' forks a background
 * agent job (own memory/tools/cortex, result file in <store>/jobs/),
 * 'jobs' / 'wait' / 'kill' supervise it. Every line publishes sh.cmd
 * on the shell's semantic bus.
 */
#ifndef AIOS_SHELL_H
#define AIOS_SHELL_H

#include "aios/action.h"
#include "aios/types.h"
#include "aios/json.h"
#include "aios/mem.h"
#include "aios/tools.h"
#include "aios/sembus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sh_shell sh_shell;

/* Result of one input line. Free with sh_result_free(). */
typedef struct {
    int       status;   /* 0 ok, 1 failed                        */
    char     *text;     /* display text (owned; "" when none)    */
    json_val *json;     /* structured output (owned; may be NULL)*/
    char     *error;    /* first error message (owned; NULL ok)  */
} sh_result;

void sh_result_free(sh_result *r);

/* Create a shell bound to a workspace jail (sandboxed tools) and an OS
 * store (memory journal, audit log, job results). Creates its own
 * semantic bus. Returns NULL on failure. */
sh_shell *sh_create(const char *jail_dir, const char *store_dir);
void      sh_free(sh_shell *sh);

/* Cortex backend for routing + agent commands: "scripted" (default,
 * deterministic) or "llm" (z-ai bridge). */
void        sh_set_cortex(sh_shell *sh, const char *mode);
const char *sh_cortex_mode(sh_shell *sh);

/* Execute one input line (';' sequences and '|' pipelines allowed).
 * Returns 0 = keep running, 1 = exit requested (exit/quit builtin).
 * out must be non-NULL; free with sh_result_free(). */
int  sh_exec(sh_shell *sh, const char *line, sh_result *out);

/* M7: the same, with the caller stating where the line came from.
 *
 * This is the whole of origin tagging, and the reason it is a separate function
 * rather than a parameter with a default: a default is an assumption, and the
 * assumption that every line is human is the one that was silently true before.
 * Typed input is human; text that a model produced is AI; there is no third
 * answer and no "unspecified" that quietly becomes human downstream.
 *
 * What the origin changes:
 *   - the descriptor carrying the action is stamped with it, so policy can tell
 *     a machine's proposal from a person's instruction;
 *   - an AI-origin line may not run a builtin that makes a change real
 *     (`promote`, `discard`, `rollback`, `snap take`, `exit`, ...). Those builtins
 *     are the human's hand on the machine; a model being able to type them would
 *     make every other guarantee decorative. The refusal is explicit and names
 *     the origin, rather than the builtin silently not being found;
 *   - an AI-origin line that the router turns into a tool keeps ORIGIN_AI all the
 *     way to the audit record.
 *
 * sh_exec() is the human case and is defined as sh_exec_origin(ORIGIN_HUMAN).
 * It is kept because a shell with a prompt is a person talking, and making every
 * caller spell that out would be noise. Anything that runs text IT produced must
 * use sh_exec_origin() -- and if it does not, it is claiming to be a person.
 */
int  sh_exec_origin(sh_shell *sh, const char *line, aios_origin origin,
                    sh_result *out);

/* M7: what a held action is waiting for, and what the console does about it.
 *
 * NEEDS_HUMAN is not a refusal (see action_gate). It is a proposal parked in
 * front of a person, and a person has to be able to answer it with one of these
 * three words. ui_present() is the console's side of that conversation; it is in
 * the library rather than in aiosh so that the desktop and the serial console ask
 * the SAME question in the same words, and so that the answer can be tested
 * without a terminal.
 *
 * The single most important line in this declaration is `hold_ms`: there is no
 * auto-promote. When the countdown runs out the answer is DISCARD, because a
 * timeout is a decision to do nothing, and "the human did not answer" must never
 * be read as "the human agreed". */
typedef enum {
    UI_CHOICE_DISCARD = 0,   /* do nothing -- the default, and the timeout    */
    UI_CHOICE_PROMOTE = 1,   /* accept: run it                                  */
    UI_CHOICE_SHOW_DIFF = 2  /* show what it would change, then ask again       */
} ui_choice;

typedef struct {
    char        title[160];   /* what is being proposed, in one line          */
    char        reason[160];  /* the law's reason, verbatim                   */
    char        detail[512];  /* the diff or the target, if there is one      */
    int         have_detail;
    aios_origin origin;
} ui_prompt;

/* Ask. Returns the choice, and never promotes on its own.
 *
 * `ask` may be NULL, which is how a non-interactive caller (a script, a boot
 * test, the serial console with no one at it) gets the honest answer: DISCARD.
 * A function that invented a reader would be inventing consent. */
ui_choice ui_present(const ui_prompt *p,
                     ui_choice (*ask)(const ui_prompt *p, void *ud), void *ud);

/* The one-line rendering both front-ends use, so the choices read the same in a
 * terminal and in a window. Returns the number of bytes written. */
int ui_prompt_text(const ui_prompt *p, char *out, size_t cap);

/* Install an asker. Passing NULL REMOVES it, which turns a held action back into
 * a reported refusal -- the state a script or a test wants. There is no "default
 * asker": a default would be a front-end the library invented, and the first
 * thing a default asker does when stdin is not a terminal is answer for nobody. */
void sh_set_ui_asker(sh_shell *sh,
                     ui_choice (*ask)(const ui_prompt *p, void *ud), void *ud);

/* The console asker: prints the three choices, reads one line, treats EOF and
 * anything unrecognised as DISCARD. Exposed so aiosh can install it and the
 * tests can call it with a pipe. */
ui_choice sh_console_ask(const ui_prompt *p, void *ud);

/* What the last line executed was stamped as. For the desktop's status line and
 * for tests that assert the stamping actually happened, rather than trusting the
 * call site. */
aios_origin sh_last_origin(const sh_shell *sh);

/* Background agent jobs. sh_poll_jobs reaps finished children; call it
 * before inspecting job state (the shell does this on every sh_exec). */
void   sh_poll_jobs(sh_shell *sh);
size_t sh_jobs_alive(sh_shell *sh);

/* The shell's OS services (borrowed — do not free). */
sembus  *sh_bus(sh_shell *sh);
mem_ctx *sh_mem(sh_shell *sh);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_SHELL_H */
