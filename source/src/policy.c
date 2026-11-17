/* policy.c — M2: the law table, evaluated strictly in the documented order.
 *
 * policy_engine.interface.md §3 fixes the order and says no step may be
 * skipped or reordered:
 *
 *   1. Schema validity
 *   2. Tier 0 laws
 *   3. Sandbox compliance
 *   4. Tier 1 laws
 *   5. Rollback feasibility
 *   6. Tier 2 advisories
 *   7. Final verdict
 *
 * This file is that list, one function per step, called in that order from
 * aios_policy_eval(). Steps 3 and 5 are partial by design (sandbox results are
 * M4, snapshots are M5) and say so where they are.
 *
 * PURITY: no I/O, no allocation, no globals, no clock. `now_ms` is a
 * parameter. The only state in the file is a `const` table and some string
 * literals, so two threads can evaluate concurrently and the same input always
 * produces the same bytes.
 */
#include "aios/policy.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- reasons */

/* One string per reason code, in enum order, with a compile-time assertion
 * that the table and the enum cannot drift (a new reason without a string is
 * a build failure, not a mystery at runtime). */
static const char *const REASON_STR[R__COUNT] = {
    [R_OK]                   = "ok",
    [R_ALLOW_HUMAN]          = "allowed: explicit human request",
    [R_NOT_MEDIATED]         = "not mediated: no action descriptor",
    [R_INVALID_DESCRIPTOR]   = "descriptor invalid",
    [R_EXPIRED]              = "descriptor expired",
    [R_BUDGET_TOO_LARGE]     = "budget exceeds the hard cap",
    [R_PATH_OUTSIDE_JAIL]    = "path outside the jail",
    [R_TIER0_PROTECTED_PATH] = "tier 0: protected path",
    [R_TIER0_AI_SHELL]       = "tier 0: an AI may not run raw shell commands",
    [R_TIER0_AI_SPAWN]       = "tier 0: an AI may not spawn background work",
    [R_NO_CAPABILITY]        = "no capability for this action",
    [R_DESTRUCTIVE]          = "destructive: needs a human",
    [R_BULK_CHANGE]          = "bulk change: needs a human",
    [R_UNKNOWN_KIND]         = "unknown action kind",
    [R_UNKNOWN_TOOL]         = "unknown tool",
    [R_REQUIRES_SANDBOX]     = "a sandbox was required and no result exists",
    [R_REQUIRES_ROLLBACK]    = "rollback is not available",
    [R_EMERGENCY]            = "system emergency mode",
};

_Static_assert(sizeof REASON_STR / sizeof REASON_STR[0] == R__COUNT,
               "REASON_STR must have exactly one entry per aios_reason");

const char *aios_policy_reason_str(aios_reason r) {
    if (r < 0 || r >= R__COUNT || !REASON_STR[r]) return "unknown reason";
    return REASON_STR[r];
}

const char *aios_policy_verdict_str(aios_verdict v) {
    switch (v) {
    case VERDICT_ALLOW:       return "ALLOW";
    case VERDICT_DENY:        return "DENY";
    case VERDICT_NEEDS_HUMAN: return "NEEDS_HUMAN";
    default:                  return "DENY(unknown verdict)";
    }
}

/* Any path component equal to ".." - not merely a prefix. A filename such as
 * "notes..txt" contains dots but no ".." component, so it stays legal. */
static int has_dotdot(const char *p) {
    if (!p) return 0;
    while (*p) {
        const char *sep = strchrnul(p, '/');
        size_t len = (size_t)(sep - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 1;
        p = *sep ? sep + 1 : sep;
    }
    return 0;
}

/* ------------------------------------------------------------- path rules */

/* Basenames that are Tier 0 no matter where they appear. The basename check
 * is not redundant with the prefix check: tools take jail-relative paths, so
 * "audit.log" would never match "/data/aios/store/audit.log" by prefix, and
 * that is exactly the path an AI would try. */
static const char *const TIER0_BASENAME[] = {
    "audit.log",          /* the append-only record of what happened        */
    "audit.log.1",        /* its rotation target                           */
    "policy.c",           /* the law itself                                */
    "policy.h",
    "profile.json",       /* M7 origin/behaviour profile                   */
    "aios-init.sock",     /* PID 1 control socket (M11)                    */
    "init.sock",
    NULL
};

/* Prefixes that are Tier 0 wherever the store lives. */
static const char *const TIER0_PREFIX[] = {
    "/data/aios/store/audit",
    "/data/aios/store/policy",
    "/run/aios/init.sock",
    "/run/aios/policy",
    NULL
};

int aios_policy_is_tier0_path(const char *path) {
    if (!path || !*path) return 0;
    for (size_t i = 0; TIER0_PREFIX[i]; i++)
        if (strncmp(path, TIER0_PREFIX[i], strlen(TIER0_PREFIX[i])) == 0)
            return 1;
    /* basename match, for relative paths */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (size_t i = 0; TIER0_BASENAME[i]; i++)
        if (strcmp(base, TIER0_BASENAME[i]) == 0)
            return 1;
    return 0;
}

/* Lexical jail containment.
 *
 * Deliberately NOT realpath(): this function is pure and does not touch the
 * filesystem, so it cannot resolve symlinks. That is a documented limitation,
 * not an oversight - the sandbox (M3/M4) is what enforces containment for
 * real; this is the early, cheap refusal. It fails closed on every ambiguity:
 * a path it cannot normalise is OUTSIDE. */
aios_path_class aios_policy_path_class(const char *path, const char *jail) {
    if (!path || !*path) return PC_OUTSIDE;

    if (aios_policy_is_tier0_path(path)) return PC_TIER0;

    /* An absolute path can only be inside the jail if the jail is an absolute
     * prefix of it. */
    if (path[0] == '/') {
        if (!jail || !*jail || jail[0] != '/') return PC_OUTSIDE;
        size_t jl = strlen(jail);
        while (jl > 1 && jail[jl - 1] == '/') jl--;      /* ignore trailing / */
        /* "/workspace" must not match "/workspaceX/..." */
        if (strncmp(path, jail, jl) != 0) return PC_OUTSIDE;
        if (path[jl] == 0 || path[jl] == '/') return PC_JAIL;
        return PC_OUTSIDE;
    }

    /* A relative path is jail-relative by construction: the tools resolve it
     * inside the jail (tool-exec's safe_relpath rejects ".." first). Peek at
     * the first component anyway - a leading ".." means the caller intends to
     * leave, and intent is enough to refuse. */
    if (strncmp(path, "..", 2) == 0 && (path[2] == 0 || path[2] == '/'))
        return PC_OUTSIDE;
    return PC_JAIL;
}

/* ------------------------------------------------------------ capabilities */

static int has_cap(const char *const *caps, int n_caps, const char *want) {
    if (!caps || n_caps <= 0 || !want) return 0;
    for (int i = 0; i < n_caps; i++) {
        if (!caps[i]) continue;
        if (strcmp(caps[i], want) == 0) return 1;
        /* "tool:*" style wildcard: the prefix before '*' must match and the
         * rest must be '*' alone, so "tool:*" is a family grant and
         * "tool:ca*" is not (no partial-prefix grants). */
        size_t cl = strlen(caps[i]);
        if (cl > 1 && caps[i][cl - 1] == '*') {
            if (strncmp(want, caps[i], cl - 1) == 0) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ result */

static aios_decision decide(aios_verdict v, aios_reason r, int tier,
                            const char *why) {
    aios_decision d;
    memset(&d, 0, sizeof d);
    d.v = v;
    d.r = r;
    d.tier = tier;
    if (why) snprintf(d.why, sizeof d.why, "%s", why);
    else     snprintf(d.why, sizeof d.why, "%s", aios_policy_reason_str(r));
    return d;
}

#define DENY(r, tier, ...)   decide(VERDICT_DENY, (r), (tier), (__VA_ARGS__))
#define HUMAN(r, tier, ...)  decide(VERDICT_NEEDS_HUMAN, (r), (tier), (__VA_ARGS__))
#define ALLOWD(r, ...)       decide(VERDICT_ALLOW, (r), 2, (__VA_ARGS__))

/* ---- step 1: schema validity ------------------------------------------- */

static aios_decision step1_schema(const aios_action *a, double now_ms) {
    if (!a) return DENY(R_NOT_MEDIATED, 0,
                        "no action descriptor: an unevaluated action is denied");

    /* Expiry gets its own reason. The validator also refuses an expired
     * descriptor, but it can only say "invalid"; a log that says EXPIRED is
     * worth more than one that says invalid. Checked first for that reason. */
    if (now_ms > 0 && !(a->expires_ms > now_ms))
        return DENY(R_EXPIRED, 0, "descriptor expired before evaluation");

    char why[256];
    if (!aios_action_is_valid_at(a, now_ms, why, sizeof why)) {
        /* The validator's own words, prefixed so the log says which stage
         * refused. Truncated into the decision buffer on purpose. */
        char buf[160];
        snprintf(buf, sizeof buf, "descriptor invalid: %.120s", why);
        return DENY(R_INVALID_DESCRIPTOR, 0, buf);
    }

    if (a->budget_ms > AIOS_ACTION_MAX_BUDGET_MS)
        return DENY(R_BUDGET_TOO_LARGE, 0,
                    "budget exceeds the hard cap (refused, not clamped)");

    /* Every declared effect must be classifiable. An effect path we cannot
     * place is not a path we can permit. */
    for (int i = 0; i < a->n_declared; i++) {
        const char *e = a->declared[i];
        if (!e || !strchr(e, ':'))
            return DENY(R_INVALID_DESCRIPTOR, 0, "malformed declared effect");
    }
    return ALLOWD(R_OK, "schema valid");
}

/* ---- step 2: Tier 0 (absolute vetoes, bind the human too) --------------- */

static aios_decision step2_tier0(const aios_action *a, const char *jail,
                                 double now_ms) {
    (void)now_ms;
    int ai = (a->origin == ORIGIN_AI);

    /* 2a. Protected paths: the audit log, the law, PID 1's socket. No origin
     *     gets these - not the AI, and not the human either. Letting a human
     *     edit the audit log would make every other guarantee unfalsifiable. */
    for (int i = 0; i < a->n_declared; i++) {
        const char *e = a->declared[i];
        const char *colon = strchr(e, ':');
        if (!colon) continue;
        const char *path = colon + 1;
        if (aios_policy_is_tier0_path(path)) {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "tier 0: %.80s is protected (audit/law/control)", path);
            return DENY(R_TIER0_PROTECTED_PATH, 0, buf);
        }
        /* The target itself may be the protected thing (e.g. an ACT_FS_WRITE
         * whose target is the audit log). */
    }
    if (aios_policy_is_tier0_path(a->target))
        return DENY(R_TIER0_PROTECTED_PATH, 0,
                    "tier 0: the target is a protected path");

    /* 2b. An AI may not run raw shell commands. This is the rule that keeps
     *     "capability may scale; authority may not" meaningful: shell access
     *     would let an AI compose effects it never declared. */
    if (ai && a->kind == ACT_SHELL)
        return DENY(R_TIER0_AI_SHELL, 0,
                    "tier 0: an AI may not execute shell commands");

    /* 2c. Nothing an AI proposes may create new AI work. An agent spawning
     *     agents is how a bounded action becomes an unbounded swarm, and how
     *     an audit trail stops being a complete account of what happened. */
    if (ai && a->kind == ACT_JOB_SPAWN)
        return DENY(R_TIER0_AI_SPAWN, 0,
                    "tier 0: an AI may not spawn background work");

    /* 2d. Nothing may leave the jail, for any origin. A ".." component is
     *     refused even when the path is relative: it can only be an attempt
     *     to leave, and the jail-relative tools would reject it anyway (M4's
     *     safe_relpath). Refusing it here means the answer comes from the law
     *     rather than from whichever worker happens to enforce it later. */
    for (int i = 0; i < a->n_declared; i++) {
        const char *e = a->declared[i];
        const char *colon = strchr(e, ':');
        if (!colon) continue;
        if (has_dotdot(colon + 1))
            return DENY(R_PATH_OUTSIDE_JAIL, 0,
                        "path contains a '..' component");
        if (aios_policy_path_class(colon + 1, jail) == PC_OUTSIDE) {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "path outside the jail: %.80s", colon + 1);
            return DENY(R_PATH_OUTSIDE_JAIL, 0, buf);
        }
    }

    /* 2d-bis. The path the action CARRIES, not just the path it declared.
     *
     * A descriptor whose declared set is empty because the path could not be
     * declared (aios_action_add_effect rejects a ".." component) would
     * otherwise sail past 2d with nothing to inspect. So inspect the request
     * itself: for a tool with a "path" argument, that path is what will be
     * touched. Checking the claim and the request separately is what makes
     * "declare, then prove" more than a promise. */
    {
        const char *req = a->args ? json_str(json_obj_get(a->args, "path"))
                                  : NULL;
        if (req && *req) {
            if (has_dotdot(req))
                return DENY(R_PATH_OUTSIDE_JAIL, 0,
                            "requested path contains a '..' component");
            if (aios_policy_path_class(req, jail) == PC_OUTSIDE) {
                char buf[160];
                snprintf(buf, sizeof buf, "requested path outside the jail: %.76s",
                         req);
                return DENY(R_PATH_OUTSIDE_JAIL, 0, buf);
            }
            if (aios_policy_is_tier0_path(req))
                return DENY(R_TIER0_PROTECTED_PATH, 0,
                            "tier 0: the requested path is protected");
        }
    }

    /* 2e. Unknown action kinds fail closed. (The validator already refuses
     *     them; this is defence in depth for a descriptor built by hand.) */
    if (a->kind < 0 || a->kind >= ACT__COUNT)
        return DENY(R_UNKNOWN_KIND, 0, "unknown action kind");

    return ALLOWD(R_OK, "no tier 0 law applies");
}

/* ---- step 3: sandbox compliance ---------------------------------------- */

static aios_decision step3_sandbox(const aios_action *a,
                                   const aios_policy_ctx *ctx) {
    /* Which actions must have run in a sandbox first? The ones whose effects
     * are inherently unbounded: a raw shell command, a delete, or any
     * fs-write/delete proposal whose declared set is wide. */
    /* Only multi-file writes are treated as bulk (tier 1). A single write is
     * not "unbounded": it is capped by the descriptor's budget and effect
     * limit, and every write is reversible once M5 lands. Requiring a sandbox
     * for one file made every legitimate AI write unsatisfiable. */
    int requires = (a->kind == ACT_SHELL || a->kind == ACT_FS_DELETE);

    /* A delete has a more fundamental rule than "was it sandboxed" (tier 1
     * destructive, which needs a human). If we report the sandbox rule first,
     * the log says "no sandbox" when the truth is "this needed a human", and
     * whoever reads it chases the wrong problem. Let the destructive rule
     * speak. */
    if (a->kind == ACT_FS_DELETE) return ALLOWD(R_OK, "sandbox deferred to tier 1");
    if (!requires) return ALLOWD(R_OK, "sandbox not required");

    if (!ctx->sandbox_executed)
        return DENY(R_REQUIRES_SANDBOX, 1,
                    "a sandbox was required for this action and none ran");

    /* The result itself is compared against the declared effects by M4. Until
     * then this step can only confirm that one exists - which it says rather
     * than pretending to have compared anything. */
    return ALLOWD(R_OK, "sandbox ran (result comparison lands in M4)");
}

/* ---- step 4: Tier 1 (critical, currently not overridable) --------------- */

static aios_decision step4_tier1(const aios_action *a, const char *jail) {
    int ai = (a->origin == ORIGIN_AI);

    /* 4a. Destructive proposals need a human. A human deleting their own file
     *     is not a safety event; a model deleting it is. */
    if (a->kind == ACT_FS_DELETE && ai) {
        /* A delete must NAME what it deletes. An unnamed destructive action
         * is the one case where "declare your effects" is not bureaucracy. */
        if (a->n_declared == 0)
            return HUMAN(R_DESTRUCTIVE, 1,
                         "AI delete with no declared target: needs a human");
        return HUMAN(R_DESTRUCTIVE, 1,
                     "AI delete: needs a human to confirm");
    }
    for (int i = 0; i < a->n_declared; i++) {
        const char *e = a->declared[i];
        if (e && strncmp(e, "delete:", 7) == 0 && ai)
            return HUMAN(R_DESTRUCTIVE, 1,
                         "AI action declares a delete: needs a human");
    }

    /* 4b. Bulk change: more writes in one action than a person can review. */
    if (ai) {
        int writes = 0;
        for (int i = 0; i < a->n_declared; i++)
            if (a->declared[i] && strncmp(a->declared[i], "write:", 6) == 0)
                writes++;
        if (writes > AIOS_POLICY_BULK_WRITES) {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "AI action writes %d files (limit %d): needs a human",
                     writes, AIOS_POLICY_BULK_WRITES);
            return HUMAN(R_BULK_CHANGE, 1, buf);
        }
    }

    /* 4c. An AI writing outside /workspace is a capability escalation, even
     *     if it is inside a jail that happens to be configured elsewhere.
     *     Only /workspace is a sanctioned write target for machine actions. */
    if (ai && (a->kind == ACT_FS_WRITE || a->kind == ACT_FS_DELETE)) {
        for (int i = 0; i < a->n_declared; i++) {
            const char *e = a->declared[i];
            if (!e) continue;
            const char *colon = strchr(e, ':');
            if (!colon) continue;
            if (strncmp(e, "write:", 6) != 0 && strncmp(e, "delete:", 7) != 0)
                continue;
            if (aios_policy_path_class(colon + 1, jail) != PC_JAIL)
                return HUMAN(R_NO_CAPABILITY, 1,
                             "AI write outside the jail's workspace");
        }
    }

    /* Capability is checked in the final step, not here: see the comment at
     * the bottom of aios_policy_eval() for why the ordering matters. */
    return ALLOWD(R_OK, "no tier 1 law applies");
}

/* ---- step 5: rollback feasibility -------------------------------------- */

static aios_decision step5_rollback(const aios_action *a,
                                    const aios_policy_ctx *ctx) {
    /* Is this action undoable? Judge the ACTION, not the kind: a tool action
     * that declares two writes is just as hard to undo as an ACT_FS_WRITE
     * that does. Counting writes rather than trusting the kind is the whole
     * reason the descriptor carries a declared set.
     *
     * Today no snapshot store exists (M5), so this step can only report what
     * it knows - and it must not pretend to have checked physics. */
    int writes = 0;
    for (int i = 0; i < a->n_declared; i++)
        if (a->declared[i] && strncmp(a->declared[i], "write:", 6) == 0) writes++;

    int destructive = (a->kind == ACT_FS_DELETE) || (writes > 1);
    if (!destructive) return ALLOWD(R_OK, "rollback not required");

    if (ctx->rollback_available) {
        aios_decision d = ALLOWD(R_OK, "rollback available");
        d.needs_rollback = 1;
        return d;
    }

    /* Not undoable. For a machine action that is a stop; for a human it is a
     * warning attached to a decision they already made in person. Requiring a
     * human to confirm a human's own explicit command is theatre, and theatre
     * in a safety system trains people to click through it. */
    if (a->origin == ORIGIN_AI) {
        aios_decision d = HUMAN(R_REQUIRES_ROLLBACK, 1,
                                "not undoable yet (no snapshot store): "
                                "needs a human");
        d.needs_rollback = 1;
        return d;
    }
    aios_decision d = ALLOWD(R_OK, "not undoable, but a human asked explicitly");
    d.needs_rollback = 1;
    return d;
}

/* ---- step 6: Tier 2 (advisory; can only downgrade, never permit) ------- */

static aios_decision step6_advisory(const aios_action *a,
                                    const aios_policy_ctx *ctx) {
    /* Advisory rules may downgrade a verdict or add a requirement. They may
     * never turn a DENY (or a NEEDS_HUMAN) into an ALLOW: this step only ever
     * adds, so a bug here cannot widen authority. */
    aios_decision d = ALLOWD(R_OK, "no advisory applies");
    d.needs_sandbox = (a->kind == ACT_TOOL || a->kind == ACT_SHELL);

    /* Emergency mode is advisory here only because step 0 catches it; if it
     * ever gets past step 0, the honest thing is to keep it refused. */
    if (ctx->emergency)
        return DENY(R_EMERGENCY, 1, "system emergency mode: refusing actions");
    return d;
}

/* ---- the evaluator ----------------------------------------------------- */

aios_policy_ctx aios_policy_ctx_default(void) {
    aios_policy_ctx c;
    memset(&c, 0, sizeof c);   /* no emergency, no sandbox, no rollback */
    return c;
}

/* Carry the REQUIREMENTS of an earlier step into a later verdict.
 *
 * This exists because of a real bug found by test_policy.c: steps 5 and 6
 * each built a fresh decision, so a "must be undoable" flag set by step 5 was
 * silently dropped by the final verdict - the evaluator said ALLOW with
 * needs_rollback = 0 and the host had no idea it had to be able to undo the
 * action. Requirements are cumulative; discarding them is how a safety
 * property becomes a comment. */
static void inherit(aios_decision *into, const aios_decision *from) {
    if (!into || !from) return;
    if (from->needs_sandbox)  into->needs_sandbox = 1;
    if (from->needs_rollback) into->needs_rollback = 1;
    if (from->tier > into->tier) into->tier = from->tier;
}

aios_decision aios_policy_eval(const aios_action *a, const char *jail,
                               const char *const *caps, int n_caps,
                               const aios_policy_ctx *ctx, double now_ms) {
    aios_policy_ctx local;
    if (!ctx) { local = aios_policy_ctx_default(); ctx = &local; }

    /* Step 0 (before schema, because an emergency outranks everything): */
    if (ctx->emergency)
        return DENY(R_EMERGENCY, 0, "system emergency mode: refusing actions");

    /* Requirements accumulate across steps in this record. */
    aios_decision acc;
    memset(&acc, 0, sizeof acc);

    /* 1. schema */
    aios_decision d = step1_schema(a, now_ms);
    if (d.v == VERDICT_DENY) return d;
    inherit(&acc, &d);

    /* 2. tier 0 */
    d = step2_tier0(a, jail, now_ms);
    if (d.v != VERDICT_ALLOW) return d;
    inherit(&acc, &d);

    /* 3. sandbox compliance */
    d = step3_sandbox(a, ctx);
    if (d.v != VERDICT_ALLOW) return d;
    inherit(&acc, &d);

    /* 4. tier 1 */
    d = step4_tier1(a, jail);
    if (d.v != VERDICT_ALLOW) return d;
    inherit(&acc, &d);

    /* 5. rollback feasibility */
    d = step5_rollback(a, ctx);
    if (d.v != VERDICT_ALLOW) return d;
    inherit(&acc, &d);

    /* 6. advisories */
    d = step6_advisory(a, ctx);
    if (d.v != VERDICT_ALLOW) return d;
    inherit(&acc, &d);

    /* 7. final verdict - capability is required for machine-originated work.
     *    A human typing a command holds the capability by construction; the
     *    machine does not. This check lives last on purpose: it is the
     *    "allowed unless something specific grants it" rule, and putting it
     *    earlier would make every DENY reason above unreachable. */
    if (a && a->origin == ORIGIN_AI) {
        const char *need_tool = (a->kind == ACT_TOOL)
                                ? a->target : NULL;
        char want[128];
        int granted = 0;
        if (need_tool) {
            snprintf(want, sizeof want, "tool:%s", need_tool);
            granted = has_cap(caps, n_caps, want)
                      || has_cap(caps, n_caps, "tool:*");
        } else if (a->kind == ACT_AGENT) {
            granted = has_cap(caps, n_caps, "agent:run")
                      || has_cap(caps, n_caps, "agent:*");
        } else if (a->target && *a->target) {
            /* A write or delete names its tool, so `tool:fs.write` means what
             * it says. The earlier `act:FS_WRITE` form was a second vocabulary
             * for the same thing, and the two could not be satisfied by the
             * same grant -- which is how every AI write ended up denied. */
            snprintf(want, sizeof want, "tool:%s", a->target);
            granted = has_cap(caps, n_caps, want)
                      || has_cap(caps, n_caps, "tool:*");
        } else {
            snprintf(want, sizeof want, "act:%s",
                     aios_action_kind_name(a->kind));
            granted = has_cap(caps, n_caps, want);
        }
        if (!granted) {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "no capability: %.100s (machine actions need a grant)",
                     need_tool ? want : aios_action_kind_name(a->kind));
            return DENY(R_NO_CAPABILITY, 1, buf);
        }
        d.r = R_OK;
        snprintf(d.why, sizeof d.why, "capability granted: %.100s", want);
        inherit(&d, &acc);
        return d;
    }

    /* A human action needs no capability: the human IS the authority. The
     * reason says so, so the log distinguishes "permitted" from "permitted
     * because a person asked". */
    d.r = R_ALLOW_HUMAN;
    snprintf(d.why, sizeof d.why, "explicit human request");
    inherit(&d, &acc);
    return d;
}

/* ---- the compiled-in law table ---------------------------------------- */

/* A readable inventory of the rules above, for tests and for `policy` output.
 * It is documentation that cannot drift silently: test_policy.c asserts every
 * DENY/HUMAN reason code appears here. */
typedef struct {
    const char *law;
    int         tier;
    aios_reason reason;
} policy_rule;

static const policy_rule RULES[] = {
    {"a descriptor is required before anything runs",        0, R_NOT_MEDIATED},
    {"a descriptor must validate",                           0, R_INVALID_DESCRIPTOR},
    {"a descriptor must not be expired",                     0, R_EXPIRED},
    {"the audit log, the law and PID 1's socket are protected",
                                                             0, R_TIER0_PROTECTED_PATH},
    {"an AI may not run raw shell commands",                 0, R_TIER0_AI_SHELL},
    {"an AI may not spawn background work",                  0, R_TIER0_AI_SPAWN},
    {"nothing may act outside the jail",                     0, R_PATH_OUTSIDE_JAIL},
    {"unknown kinds and unknown tools are denied",           0, R_UNKNOWN_KIND},
    {"unbounded actions must have run in a sandbox",         1, R_REQUIRES_SANDBOX},
    {"an AI may not delete without a human",                 1, R_DESTRUCTIVE},
    {"an AI may not bulk-change without a human",            1, R_BULK_CHANGE},
    {"destructive actions must be undoable",                 1, R_REQUIRES_ROLLBACK},
    {"machine actions need a capability",                    1, R_NO_CAPABILITY},
    {"emergency mode refuses everything",                    1, R_EMERGENCY},
    {"a human request is permitted without a capability",    2, R_ALLOW_HUMAN},
};

size_t aios_policy_rule_count(void) {
    return sizeof RULES / sizeof RULES[0];
}

/* Read-only access for `policy` in the shell. The AI gets no new ability from
 * this: policy_engine.interface.md says the engine is non-introspectable to
 * the AI, and it still is - this is a HUMAN-facing view on a compiled-in
 * constant array. Nothing here can be written, and reading it changes no
 * verdict. */
int aios_policy_rule_at(size_t i, const char **law, int *tier, aios_reason *r) {
    if (i >= sizeof RULES / sizeof RULES[0]) return 0;
    if (law)  *law  = RULES[i].law;
    if (tier) *tier = RULES[i].tier;
    if (r)    *r    = RULES[i].reason;
    return 1;
}
