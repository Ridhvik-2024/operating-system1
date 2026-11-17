/* aios/policy.h — M2: the law table.
 *
 * The constitution's central mechanism, per policy_engine.interface.md, and
 * the reason that document exists:
 *
 *   "It evaluates rules. That's all."
 *   "The AI has ZERO authority over this process."
 *   "If intent and law conflict, law always wins."
 *
 * aios_policy_eval() is a PURE function: no I/O, no allocation, no globals, no
 * clock. Everything it cannot know is passed in. Same inputs always produce
 * byte-identical output, which is what makes the verdict auditable rather than
 * merely trusted.
 *
 * ---------------------------------------------------------------------------
 * THE ONE ASYMMETRY THAT DEFINES THIS FILE
 *
 * The same rule is enforced for a human and a machine, except where the
 * constitution says otherwise. A human may do things an AI may not:
 *
 *   - edit the audit log, the policy table, PID 1's socket      human: DENY
 *                                                               AI:    DENY
 *   - write outside the jail                                    human: DENY
 *                                                               AI:    DENY
 *   - delete a file                                             human: ALLOW
 *                                                               AI:    NEEDS_HUMAN
 *   - run a raw shell command                                   human: ALLOW
 *                                                               AI:    DENY
 *   - spawn a background job                                    human: ALLOW
 *                                                               AI:    DENY
 *   - write more than 8 files                                   human: ALLOW
 *                                                               AI:    NEEDS_HUMAN
 *   - write inside /workspace                                   human: ALLOW
 *                                                               AI:    ALLOW
 *
 * The first line is the important one: Tier 0 is a veto that binds the HUMAN
 * too ("never overridable, not even by the human"). Those rules are not
 * about trusting a user; they are about the system staying auditable at all.
 * Writing the audit log is not a privilege anyone is granted.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS NO LAW FILE, NO ENV VAR, AND NO CONFIG
 *
 * The table below is compiled in. There is no way to edit a running system's
 * law, no override file to drop, no environment variable to set. The only way
 * to change what is permitted is to change the source and ship a new image,
 * which is exactly the property a constitution needs and a config file cannot
 * have. Tier 1 overrides (time-bound, logged, auto-expiring) are a later
 * mechanism; they are not a file this evaluator reads.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS DOES NOT DO YET (be honest)
 *
 *   - No capability issuance: `caps` is a supplied list, not a minted token.
 *   - No Tier 1 overrides and no emergency mode (both are policy CONTEXT, and
 *     context is passed in as one struct so it can never leak in silently).
 *   - No sandbox-result checking (that is M4; a required-but-missing result
 *     is handled here as DENY, the fail-closed default).
 *   - Rollback feasibility (M5) is reported, not enforced.
 */
#ifndef AIOS_POLICY_H
#define AIOS_POLICY_H

#include <stddef.h>

#include "aios/action.h"
#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- verdicts ---------------------------------------------------------- */

typedef enum {
    VERDICT_DENY        = 0,   /* 0 on purpose: a zeroed decision is a DENY  */
    VERDICT_ALLOW       = 1,
    VERDICT_NEEDS_HUMAN = 2
} aios_verdict;

/* Ordering matters: DENY must not be reordered after ALLOW by any future
 * "downgrade" helper. Keep DENY = 0 so memset(0) is the safe answer. */

/* ---- reason codes ------------------------------------------------------
 * Never reuse a number. A reason is what gets logged, and a log that renames
 * a reason after the fact is not an audit trail. */
typedef enum {
    R_OK = 0,               /* allowed, nothing notable                     */
    R_ALLOW_HUMAN = 1,      /* allowed because a human asked explicitly      */
    R_NOT_MEDIATED = 2,     /* no descriptor: the action cannot be evaluated */
    R_INVALID_DESCRIPTOR = 3,
    R_EXPIRED = 4,
    R_BUDGET_TOO_LARGE = 5,
    R_PATH_OUTSIDE_JAIL = 6,
    R_TIER0_PROTECTED_PATH = 7,   /* audit log, policy table, PID 1 socket   */
    R_TIER0_AI_SHELL = 8,         /* an AI may not run raw shell commands    */
    R_TIER0_AI_SPAWN = 9,         /* no agent spawns agents / background work*/
    R_NO_CAPABILITY = 10,
    R_DESTRUCTIVE = 11,           /* delete                              */
    R_BULK_CHANGE = 12,           /* more writes than the bulk threshold */
    R_UNKNOWN_KIND = 13,
    R_UNKNOWN_TOOL = 14,
    R_REQUIRES_SANDBOX = 15,
    R_REQUIRES_ROLLBACK = 16,
    R_EMERGENCY = 17,
    R__COUNT = 18
} aios_reason;

/* ---- decision ---------------------------------------------------------- */

typedef struct {
    aios_verdict v;
    aios_reason  r;
    int          tier;      /* 0 = constitutional veto, 1 = critical, 2 = advisory */
    int          needs_sandbox;   /* host must run this in the sandbox first   */
    int          needs_rollback;  /* host must be able to undo it to promote   */
    char         why[160];        /* factual, non-persuasive                   */
} aios_decision;

/* ---- context (everything the evaluator is not allowed to look up) ------ */

/* Grouped into one struct precisely so that adding a field later cannot
 * silently change behaviour: a caller that zeroes it gets the safe defaults,
 * and the safe defaults are DENY-producing, not permissive. */
typedef struct {
    int emergency;          /* system in emergency mode -> refuse everything */
    int sandbox_executed;   /* a sandbox ran and its result is available     */
    int rollback_available; /* a rollback mechanism exists right now         */
} aios_policy_ctx;

/* Defaults: no emergency, no sandbox result, no rollback. */
aios_policy_ctx aios_policy_ctx_default(void);

/* Bulk threshold: writes beyond this need a human. 8, per the M2 card. */
#define AIOS_POLICY_BULK_WRITES 8

/* Path classes the table distinguishes. Exposed so tests can pin the mapping
 * rather than guessing at it. */
typedef enum {
    PC_JAIL = 0,      /* inside the workspace jail                        */
    PC_OUTSIDE = 1,   /* somewhere else on the filesystem                 */
    PC_TIER0 = 2      /* the audit log, the policy table, PID 1's socket  */
} aios_path_class;

/* Classify one effect path. `jail` may be NULL/"", in which case only
 * absolute paths can be proven to be inside it (they cannot), so jail-relative
 * paths classify as PC_JAIL and absolute ones as PC_OUTSIDE. */
aios_path_class aios_policy_path_class(const char *path, const char *jail);

/* True if the path is the audit log / a policy file / a PID 1 socket.
 * Matches on the BASENAME as well as the full path, because a jail-relative
 * path like "audit.log" would otherwise slip past a prefix check. */
int aios_policy_is_tier0_path(const char *path);

/* ---- the evaluator ----------------------------------------------------
 *
 * caps/n_caps: capabilities held by the requester (e.g. "fs.write",
 *              "tool:calc"). A NULL/empty list means "no capabilities", which
 *              is the fail-closed interpretation.
 *
 * now_ms: unix time. Passed in because a deterministic function may not read
 *         the clock. Pass 0 to skip the expiry check (used only by tests that
 *         construct descriptors without a clock).
 *
 * Never returns ALLOW by accident: every path through this function either
 * names a rule that permitted the action or returns DENY. */
aios_decision aios_policy_eval(const aios_action *a, const char *jail,
                               const char *const *caps, int n_caps,
                               const aios_policy_ctx *ctx, double now_ms);

const char *aios_policy_reason_str(aios_reason r);
const char *aios_policy_verdict_str(aios_verdict v);

/* Row count of the compiled-in law table. Exposed so a test can assert that a
 * rule exists for every reason code (the table and the enum cannot drift). */
size_t aios_policy_rule_count(void);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_POLICY_H */
