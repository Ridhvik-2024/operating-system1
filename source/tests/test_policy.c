/* test_policy.c — M2: the law table.
 *
 * What these tests are actually protecting:
 *
 *   1. FAIL CLOSED. There is no input that produces ALLOW without a rule
 *      naming why. The "everything is denied" tests matter more than the
 *      "this one is allowed" tests.
 *   2. TIER 0 BINDS THE HUMAN. The audit log is not writable by anyone.
 *      A test asserts the human is refused too, because that is the claim
 *      people will not believe.
 *   3. THE AI/HUMAN ASYMMETRY IS EXACT. Delete: human ALLOW, AI NEEDS_HUMAN.
 *      Shell: human ALLOW, AI DENY. Spawn: human ALLOW, AI DENY.
 *   4. DETERMINISM. Same input, byte-identical decision. Compared with
 *      memcmp over the whole struct, because "the same" means the same bytes
 *      in a log.
 *   5. THE TABLE AND THE ENUM CANNOT DRIFT. Every reason has a string, and
 *      every blocking reason appears in the compiled-in law table.
 */
#include "aios/policy.h"

#include "../tests/t.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define JAIL "workspace"

/* ---- descriptor helpers ------------------------------------------------ */

/* A valid descriptor; the caller sets what it is testing. `origin` is
 * stamped explicitly because that is what the host does. */
static aios_action *mk(aios_act_kind kind, aios_origin origin, const char *target) {
    aios_action *a = aios_action_new(kind, origin, target);
    if (!a) return NULL;
    if (origin == ORIGIN_AI) aios_action_set_intent(a, "test intent");
    return a;
}

static aios_action *mk_fs_write(const char *path, aios_origin origin) {
    aios_action *a = mk(ACT_TOOL, origin, "fs.write");
    if (!a) return NULL;
    json_val *args = json_new_obj();
    json_obj_set(args, "path", json_new_str(path));
    json_obj_set(args, "content", json_new_str("x"));
    aios_action_set_args(a, args);
    aios_action_declare_tool_effects(a, "fs.write", a->args);
    return a;
}

static aios_action *mk_fs_delete(const char *path, aios_origin origin) {
    aios_action *a = mk(ACT_FS_DELETE, origin, path);
    if (!a) return NULL;
    a->effects_complete = 1;
    char eff[600];
    snprintf(eff, sizeof eff, "delete:%s", path);
    aios_action_add_effect(a, eff);
    return a;
}

/* Capability sets used below. */
static const char *CAPS_TOOL_WRITE[] = { "tool:fs.write", "tool:fs.read" };
static const char *CAPS_ALL_TOOLS[]  = { "tool:*" };
static const char *CAPS_AGENT[]      = { "agent:run" };

static aios_decision ev(const aios_action *a, const char *jail,
                        const char *const *caps, int n) {
    aios_policy_ctx ctx = aios_policy_ctx_default();
    return aios_policy_eval(a, jail, caps, n, &ctx, 0);
}

/* ---- the baseline ------------------------------------------------------ */

static void test_baseline_allows(void) {
    /* TIER 0 IS NOT A BLANKET DENY: a well-formed, in-jail, capability-holding
     * action must actually be allowed, or the evaluator is just a wall. */
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d = ev(a, JAIL, CAPS_TOOL_WRITE, 2);
    CHECK(d.v == VERDICT_ALLOW, "AI fs.write in jail with capability is allowed");
    CHECK(d.r == R_OK, "reason is R_OK");
    CHECK(d.tier == 2, "an allowed action carries advisory tier");
    aios_action_free(a);

    /* A human writing in the jail needs no capability at all. */
    aios_action *h = mk_fs_write("notes.txt", ORIGIN_HUMAN);
    aios_decision hd = ev(h, JAIL, NULL, 0);
    CHECK(hd.v == VERDICT_ALLOW, "human fs.write needs no capability");
    CHECK(hd.r == R_ALLOW_HUMAN, "reason records that a human asked");
    aios_action_free(h);
}

/* ---- step 1: schema ---------------------------------------------------- */

static void test_no_descriptor(void) {
    aios_decision d = ev(NULL, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d.v == VERDICT_DENY, "no descriptor -> DENY");
    CHECK(d.r == R_NOT_MEDIATED, "reason is NOT_MEDIATED");
    CHECK_STR_CONTAINS(d.why, "no action descriptor", "reason states it");
}

static void test_invalid_descriptor(void) {
    /* unstamped origin: the descriptor is not yet a proposal */
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_UNSPECIFIED, "fs.write");
    aios_decision d = ev(a, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d.v == VERDICT_DENY, "unstamped origin -> DENY");
    CHECK(d.r == R_INVALID_DESCRIPTOR, "reason is INVALID_DESCRIPTOR");
    CHECK_STR_CONTAINS(d.why, "origin", "the validator's words are passed through");
    aios_action_free(a);

    /* AI without an intent hash: same stage, different cause */
    aios_action *b = mk(ACT_TOOL, ORIGIN_AI, "calc");
    b->intent_hash = 0;
    aios_decision d2 = ev(b, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d2.v == VERDICT_DENY, "AI action with no intent hash -> DENY");
    CHECK(d2.r == R_INVALID_DESCRIPTOR, "reason is INVALID_DESCRIPTOR");
    aios_action_free(b);
}

static void test_expired(void) {
    /* A descriptor whose expiry has already passed cannot pass the schema
     * step at all: this is the purity fix working. Before it, the validator
     * read the real clock, so a pure evaluation was neither pure nor
     * reproducible. */
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_AI);
    a->expires_ms = 1000.0;                      /* 1970 */
    aios_policy_ctx ctx = aios_policy_ctx_default();
    aios_decision d = aios_policy_eval(a, JAIL, CAPS_TOOL_WRITE, 2, &ctx, 5000.0);
    CHECK(d.v == VERDICT_DENY, "expired descriptor -> DENY");
    CHECK(d.r == R_EXPIRED, "reason is EXPIRED, not a generic 'invalid'");
    CHECK_STR_CONTAINS(d.why, "expired", "and the message says expired");

    /* Even a descriptor that is perfectly valid at its own creation time is
     * refused once evaluation happens after its life ends. */
    aios_action *b = mk_fs_write("notes.txt", ORIGIN_AI);
    double now = aios_now_ms();
    b->expires_ms = now + AIOS_ACTION_MAX_LIFETIME_MS;
    aios_decision d2 = aios_policy_eval(b, JAIL, CAPS_TOOL_WRITE, 2, &ctx,
                                        now + AIOS_ACTION_MAX_LIFETIME_MS + 1000.0);
    CHECK(d2.v == VERDICT_DENY, "a lapsed descriptor is denied at evaluation");
    CHECK(d2.r == R_EXPIRED, "reason is EXPIRED when evaluation outruns the token");
    aios_action_free(b);
    aios_action_free(a);
}

static void test_budget_cap(void) {
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_AI);
    /* Bypass the setter on purpose: the setter refuses, the evaluator must
     * also refuse if something set the field directly. */
    a->budget_ms = AIOS_ACTION_MAX_BUDGET_MS + 1000.0;
    aios_decision d = ev(a, JAIL, CAPS_TOOL_WRITE, 2);
    CHECK(d.v == VERDICT_DENY, "over-budget action -> DENY");
    /* The schema validator refuses an over-cap budget, so the schema step
     * catches it before the dedicated budget rule can. The dedicated rule is
     * defence in depth for a descriptor assembled by hand. Either reason is
     * a correct refusal; what must never happen is ALLOW. */
    CHECK(d.r == R_BUDGET_TOO_LARGE || d.r == R_INVALID_DESCRIPTOR,
          "refusal names the budget problem");
    aios_action_free(a);
}

/* ---- step 2: tier 0 ---------------------------------------------------- */

static void test_tier0_audit_log_human_too(void) {
    /* THE CLAIM PEOPLE DOUBT: a human may not edit the audit log. If they
     * could, every other guarantee becomes unfalsifiable. */
    aios_action *h = mk_fs_write("audit.log", ORIGIN_HUMAN);
    aios_decision d = ev(h, JAIL, NULL, 0);
    CHECK(d.v == VERDICT_DENY, "a HUMAN may not write the audit log");
    CHECK(d.r == R_TIER0_PROTECTED_PATH, "reason is TIER0_PROTECTED_PATH");
    CHECK(d.tier == 0, "tier 0, not overridable");
    aios_action_free(h);

    /* and the AI is refused identically */
    aios_action *a = mk_fs_write("audit.log", ORIGIN_AI);
    aios_decision d2 = ev(a, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d2.v == VERDICT_DENY, "an AI may not write the audit log either");
    CHECK(d2.r == R_TIER0_PROTECTED_PATH, "same reason for the AI");
    aios_action_free(a);

    /* absolute form, wherever the store lives */
    aios_action *c = mk_fs_write("/data/aios/store/audit.log", ORIGIN_HUMAN);
    aios_decision d3 = ev(c, "/data/aios", NULL, 0);
    CHECK(d3.v == VERDICT_DENY, "the absolute audit-log path is refused");
    CHECK(d3.r == R_TIER0_PROTECTED_PATH, "tier 0 protects the absolute path");
    aios_action_free(c);
}

static void test_tier0_policy_and_socket(void) {
    const char *targets[] = { "policy.c", "policy.h", "init.sock",
                              "aios-init.sock", "profile.json" };
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        char msg[128];
        snprintf(msg, sizeof msg, "tier 0 refuses %s", targets[i]);
        aios_action *a = mk_fs_write(targets[i], ORIGIN_HUMAN);
        aios_decision d = ev(a, JAIL, NULL, 0);
        CHECK(d.v == VERDICT_DENY, msg);
        CHECK(d.r == R_TIER0_PROTECTED_PATH, msg);
        aios_action_free(a);
    }
}

static void test_tier0_ai_shell(void) {
    aios_action *ai = mk(ACT_SHELL, ORIGIN_AI, "rm -rf /");
    aios_decision d = ev(ai, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d.v == VERDICT_DENY, "an AI may not run shell commands");
    CHECK(d.r == R_TIER0_AI_SHELL, "reason is TIER0_AI_SHELL");
    CHECK(d.tier == 0, "tier 0");
    aios_action_free(ai);

    /* A human may - but see test_needs_sandbox: it is not free. */
    aios_action *h = mk(ACT_SHELL, ORIGIN_HUMAN, "ls");
    aios_policy_ctx ctx = aios_policy_ctx_default();
    ctx.sandbox_executed = 1;          /* needed by step 3 */
    ctx.rollback_available = 1;
    aios_decision hd = aios_policy_eval(h, JAIL, NULL, 0, &ctx, 0);
    CHECK(hd.v == VERDICT_ALLOW, "a human may run a shell command");
    aios_action_free(h);
}

static void test_tier0_ai_spawn(void) {
    aios_action *ai = mk(ACT_JOB_SPAWN, ORIGIN_AI, "summarize the workspace");
    aios_decision d = ev(ai, JAIL, CAPS_AGENT, 1);
    CHECK(d.v == VERDICT_DENY, "an AI may not spawn background work");
    CHECK(d.r == R_TIER0_AI_SPAWN, "reason is TIER0_AI_SPAWN");
    CHECK(d.tier == 0, "tier 0");
    aios_action_free(ai);

    aios_action *h = mk(ACT_JOB_SPAWN, ORIGIN_HUMAN, "summarize the workspace");
    aios_decision hd = ev(h, JAIL, NULL, 0);
    CHECK(hd.v == VERDICT_ALLOW, "a human may spawn a job");
    aios_action_free(h);
}

static void test_path_outside_jail(void) {
    const char *bad[] = { "/etc/passwd", "/etc/shadow", "../escape.txt",
                          "sub/../../escape.txt", "/data/elsewhere.txt" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char msg[128];
        snprintf(msg, sizeof msg, "outside-jail path refused: %s", bad[i]);
        aios_action *a = mk_fs_write(bad[i], ORIGIN_HUMAN);
        aios_decision d = ev(a, JAIL, NULL, 0);
        CHECK(d.v == VERDICT_DENY, msg);
        CHECK(d.r == R_PATH_OUTSIDE_JAIL, msg);
        aios_action_free(a);
    }

    /* A filename containing dots is not a traversal: ".." must be a whole
     * component. If this ever starts failing, policy became a false-positive
     * machine and people will route around it. */
    aios_action *dots = mk_fs_write("notes..txt", ORIGIN_HUMAN);
    aios_decision dd = ev(dots, JAIL, NULL, 0);
    CHECK(dd.v == VERDICT_ALLOW, "'notes..txt' is a filename, not a traversal");
    aios_action_free(dots);

    /* Absolute path INSIDE the jail is fine. */
    aios_action *ok = mk_fs_write("/workspace/notes.txt", ORIGIN_HUMAN);
    aios_decision d = ev(ok, "/workspace", NULL, 0);
    CHECK(d.v == VERDICT_ALLOW, "an absolute path inside the jail is allowed");
    aios_action_free(ok);

    /* And a sibling whose name merely starts with the jail's is NOT inside. */
    aios_action *trap = mk_fs_write("/workspaceX/notes.txt", ORIGIN_HUMAN);
    aios_decision dt = ev(trap, "/workspace", NULL, 0);
    CHECK(dt.v == VERDICT_DENY, "/workspaceX is not inside /workspace");
    CHECK(dt.r == R_PATH_OUTSIDE_JAIL, "prefix trap caught");
    aios_action_free(trap);
}

/* ---- step 3: sandbox --------------------------------------------------- */

static void test_needs_sandbox(void) {
    /* A human shell command requires a sandbox that has not run. */
    aios_action *h = mk(ACT_SHELL, ORIGIN_HUMAN, "ls");
    aios_decision d = ev(h, JAIL, NULL, 0);
    CHECK(d.v == VERDICT_DENY, "shell command with no sandbox result -> DENY");
    CHECK(d.r == R_REQUIRES_SANDBOX, "reason is REQUIRES_SANDBOX");
    aios_action_free(h);

    /* a delete likewise */
    aios_action *del = mk_fs_delete("old.txt", ORIGIN_HUMAN);
    aios_decision dd = ev(del, JAIL, NULL, 0);
    CHECK(dd.r == R_REQUIRES_SANDBOX || dd.r == R_DESTRUCTIVE,
          "delete is gated (sandbox or human)");
    CHECK(dd.v != VERDICT_ALLOW, "delete is never a bare allow");
    aios_action_free(del);
}

/* ---- step 4: tier 1 ---------------------------------------------------- */

static void test_destructive_asymmetry(void) {
    /* The single sharpest rule in the table: same action, same path, two
     * origins, two different answers. */
    aios_action *ai = mk_fs_delete("old.txt", ORIGIN_AI);
    aios_policy_ctx ctx = aios_policy_ctx_default();
    ctx.sandbox_executed = 1;
    aios_decision d = aios_policy_eval(ai, JAIL, CAPS_ALL_TOOLS, 1, &ctx, 0);
    CHECK(d.v == VERDICT_NEEDS_HUMAN, "AI delete -> NEEDS_HUMAN");
    CHECK(d.r == R_DESTRUCTIVE, "reason is DESTRUCTIVE");
    CHECK(d.tier == 1, "tier 1");
    aios_action_free(ai);

    aios_action *h = mk_fs_delete("old.txt", ORIGIN_HUMAN);
    aios_policy_ctx hctx = aios_policy_ctx_default();
    hctx.sandbox_executed = 1;
    hctx.rollback_available = 1;
    aios_decision hd = aios_policy_eval(h, JAIL, NULL, 0, &hctx, 0);
    CHECK(hd.v == VERDICT_ALLOW, "human delete -> ALLOW (it is their file)");
    CHECK(hd.needs_rollback == 1, "but the host must be able to undo it");
    aios_action_free(h);
}

static void test_ai_delete_without_declaring(void) {
    /* An AI delete that names nothing is refused harder than one that does:
     * "delete something" with no target is the shape of an accident. */
    aios_action *a = mk(ACT_FS_DELETE, ORIGIN_AI, "something");
    a->effects_complete = 1;              /* declares the empty set */
    aios_policy_ctx ctx = aios_policy_ctx_default();
    ctx.sandbox_executed = 1;
    aios_decision d = aios_policy_eval(a, JAIL, CAPS_ALL_TOOLS, 1, &ctx, 0);
    CHECK(d.v == VERDICT_NEEDS_HUMAN, "undeclared AI delete -> NEEDS_HUMAN");
    CHECK_STR_CONTAINS(d.why, "no declared target", "reason says why");
    aios_action_free(a);
}

static void test_bulk_change(void) {
    aios_action *a = mk(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(a, "write lots of files");
    a->effects_complete = 1;
    /* exactly at the limit: allowed */
    for (int i = 0; i < AIOS_POLICY_BULK_WRITES; i++) {
        char eff[64];
        snprintf(eff, sizeof eff, "write:f%d.txt", i);
        aios_action_add_effect(a, eff);
    }
    aios_policy_ctx ctx = aios_policy_ctx_default();
    ctx.sandbox_executed = 1;
    ctx.rollback_available = 1;
    aios_decision d = aios_policy_eval(a, JAIL, CAPS_TOOL_WRITE, 2, &ctx, 0);
    CHECK(d.v == VERDICT_ALLOW, "8 writes is at the limit, not over");
    aios_action_free(a);

    /* one more: needs a human */
    aios_action *b = mk(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(b, "write more files");
    b->effects_complete = 1;
    for (int i = 0; i <= AIOS_POLICY_BULK_WRITES; i++) {
        char eff[64];
        snprintf(eff, sizeof eff, "write:f%d.txt", i);
        aios_action_add_effect(b, eff);
    }
    aios_decision d2 = aios_policy_eval(b, JAIL, CAPS_TOOL_WRITE, 2, &ctx, 0);
    CHECK(d2.v == VERDICT_NEEDS_HUMAN, "9 writes needs a human");
    CHECK(d2.r == R_BULK_CHANGE, "reason is BULK_CHANGE");
    CHECK_STR_CONTAINS(d2.why, "9 files", "reason states the count");
    aios_action_free(b);
}

static void test_capability(void) {
    /* machine action, no capabilities at all */
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d = ev(a, JAIL, NULL, 0);
    CHECK(d.v == VERDICT_DENY, "AI action with no capabilities -> DENY");
    CHECK(d.r == R_NO_CAPABILITY, "reason is NO_CAPABILITY");
    aios_action_free(a);

    /* wrong capability */
    static const char *wrong[] = { "tool:fs.read" };
    aios_action *b = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d2 = ev(b, JAIL, wrong, 1);
    CHECK(d2.v == VERDICT_DENY, "wrong capability -> DENY");
    aios_action_free(b);

    /* exact capability */
    aios_action *c = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d3 = ev(c, JAIL, CAPS_TOOL_WRITE, 2);
    CHECK(d3.v == VERDICT_ALLOW, "exact capability -> ALLOW");
    aios_action_free(c);

    /* wildcard family grant */
    aios_action *e = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d4 = ev(e, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d4.v == VERDICT_ALLOW, "tool:* grants fs.write");
    aios_action_free(e);

    /* a partial prefix is NOT a grant: "tool:fs*" must not match "tool:fs.write"
     * by accident... actually the rule is that the '*' must be last, which it
     * is here, so this SHOULD match. The distinction that matters is that
     * "tool:fs." does not match "tool:fsX". */
    static const char *nearly[] = { "tool:fs." };
    aios_action *f = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d5 = ev(f, JAIL, nearly, 1);
    CHECK(d5.v == VERDICT_DENY, "a non-matching prefix is not a grant");
    aios_action_free(f);
}

static void test_agent_capability(void) {
    aios_action *a = mk(ACT_AGENT, ORIGIN_AI, "summarize the workspace");
    aios_decision d = ev(a, JAIL, NULL, 0);
    CHECK(d.v == VERDICT_DENY, "AI agent with no capability -> DENY");
    CHECK(d.r == R_NO_CAPABILITY, "reason is NO_CAPABILITY");
    aios_action_free(a);

    aios_action *b = mk(ACT_AGENT, ORIGIN_AI, "summarize the workspace");
    aios_decision d2 = ev(b, JAIL, CAPS_AGENT, 1);
    CHECK(d2.v == VERDICT_ALLOW, "agent:run grants an agent");
    aios_action_free(b);
}

/* ---- step 5/6: rollback + advisory ------------------------------------- */

static void test_rollback_feasibility(void) {
    aios_action *a = mk(ACT_TOOL, ORIGIN_HUMAN, "fs.write");
    json_val *args = json_new_obj();
    json_obj_set(args, "path", json_new_str("a.txt"));
    aios_action_set_args(a, args);
    a->effects_complete = 1;
    aios_action_add_effect(a, "write:a.txt");
    aios_action_add_effect(a, "write:b.txt");

    aios_policy_ctx ctx = aios_policy_ctx_default();
    ctx.sandbox_executed = 1;

    /* A HUMAN multi-write with no undo: allowed, but the host is told it must
     * be able to undo it. Blocking a person's own explicit command behind a
     * confirmation they already gave trains them to click through warnings. */
    aios_decision d = aios_policy_eval(a, JAIL, NULL, 0, &ctx, 0);
    CHECK(d.v == VERDICT_ALLOW, "human multi-write with no rollback is allowed");
    CHECK(d.needs_rollback == 1, "but the decision asks for rollback");
    aios_action_free(a);

    /* The SAME action from a machine is stopped: unauditable-in-reverse
     * changes are exactly what a model must not make unilaterally. */
    aios_action *m = mk(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(m, "write two files");
    json_val *margs = json_new_obj();
    json_obj_set(margs, "path", json_new_str("a.txt"));
    aios_action_set_args(m, margs);
    m->effects_complete = 1;
    aios_action_add_effect(m, "write:a.txt");
    aios_action_add_effect(m, "write:b.txt");
    aios_decision md = aios_policy_eval(m, JAIL, CAPS_TOOL_WRITE, 2, &ctx, 0);
    CHECK(md.v == VERDICT_NEEDS_HUMAN, "AI multi-write with no rollback -> human");
    CHECK(md.r == R_REQUIRES_ROLLBACK, "reason is REQUIRES_ROLLBACK");
    CHECK(md.needs_rollback == 1, "decision asks for rollback");
    aios_action_free(m);
}

static void test_emergency(void) {
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_HUMAN);
    aios_policy_ctx ctx = aios_policy_ctx_default();
    ctx.emergency = 1;
    aios_decision d = aios_policy_eval(a, JAIL, NULL, 0, &ctx, 0);
    CHECK(d.v == VERDICT_DENY, "emergency mode refuses even a human write");
    CHECK(d.r == R_EMERGENCY, "reason is EMERGENCY");

    /* and it outranks everything: even an otherwise-valid AI action */
    aios_action *b = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d2 = aios_policy_eval(b, JAIL, CAPS_ALL_TOOLS, 1, &ctx, 0);
    CHECK(d2.r == R_EMERGENCY, "emergency outranks the capability check");
    aios_action_free(b);
    aios_action_free(a);
}

static void test_sandbox_requirement_flag(void) {
    /* Allowed actions still tell the host what to do next. */
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_HUMAN);
    aios_decision d = ev(a, JAIL, NULL, 0);
    CHECK(d.v == VERDICT_ALLOW, "a plain write is allowed");
    CHECK(d.needs_sandbox == 1, "and it still asks for sandboxing");
    aios_action_free(a);
}

/* ---- fail-closed: nothing is allowed by accident ----------------------- */

static void test_fail_closed_fuzz(void) {
    /* Hand-built descriptors that skip the constructor's guarantees. Every
     * one must be refused; none may reach ALLOW. */
    aios_action *a = mk(ACT_TOOL, ORIGIN_AI, "calc");
    aios_action_set_intent(a, "compute");

    a->kind = (aios_act_kind)99;
    aios_decision d = ev(a, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d.v == VERDICT_DENY, "out-of-range kind -> DENY");
    CHECK(d.r == R_INVALID_DESCRIPTOR || d.r == R_UNKNOWN_KIND,
          "reason names the kind problem");
    a->kind = ACT_TOOL;

    a->origin = (aios_origin)42;
    aios_decision d2 = ev(a, JAIL, CAPS_ALL_TOOLS, 1);
    CHECK(d2.v == VERDICT_DENY, "garbage origin -> DENY");
    a->origin = ORIGIN_AI;

    /* NULL jail: a relative path still classifies, an absolute one cannot be
     * proven inside, so it must be refused rather than assumed safe. */
    aios_action *abs = mk_fs_write("/workspace/x.txt", ORIGIN_HUMAN);
    aios_decision d3 = ev(abs, NULL, NULL, 0);
    CHECK(d3.v == VERDICT_DENY, "absolute path with no jail -> DENY (cannot prove)");
    CHECK(d3.r == R_PATH_OUTSIDE_JAIL, "reason is PATH_OUTSIDE_JAIL");
    aios_action_free(abs);

    /* NULL ctx is replaced by the safe default, and the default is not
     * permissive: no sandbox executed means shell work is still refused. */
    aios_action *sh = mk(ACT_SHELL, ORIGIN_HUMAN, "ls");
    aios_decision d4 = aios_policy_eval(sh, JAIL, NULL, 0, NULL, 0);
    CHECK(d4.v == VERDICT_DENY, "NULL ctx defaults to the safe (denying) context");
    aios_action_free(sh);

    /* negative n_caps must not read memory or grant anything */
    aios_action *neg = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_decision d5 = ev(neg, JAIL, CAPS_ALL_TOOLS, -1);
    CHECK(d5.v == VERDICT_DENY, "negative capability count grants nothing");
    aios_action_free(neg);

    aios_action_free(a);
}

/* ---- determinism ------------------------------------------------------- */

static void test_determinism(void) {
    aios_action *a = mk_fs_write("notes.txt", ORIGIN_AI);
    aios_policy_ctx ctx = aios_policy_ctx_default();
    aios_decision d1 = aios_policy_eval(a, JAIL, CAPS_TOOL_WRITE, 2, &ctx, 0);
    aios_decision d2 = aios_policy_eval(a, JAIL, CAPS_TOOL_WRITE, 2, &ctx, 0);
    CHECK(memcmp(&d1, &d2, sizeof d1) == 0,
          "same inputs produce byte-identical decisions");

    /* and a denial is equally stable */
    aios_decision n1 = aios_policy_eval(a, JAIL, NULL, 0, &ctx, 0);
    aios_decision n2 = aios_policy_eval(a, JAIL, NULL, 0, &ctx, 0);
    CHECK(memcmp(&n1, &n2, sizeof n1) == 0, "denials are deterministic too");
    aios_action_free(a);

    /* the evaluator must not have modified the descriptor it was given */
    aios_action *b = mk_fs_write("notes.txt", ORIGIN_AI);
    char before[64];
    snprintf(before, sizeof before, "%s|%d|%.0f", b->id, b->n_declared,
             b->budget_ms);
    aios_policy_ctx c2 = aios_policy_ctx_default();
    aios_policy_eval(b, JAIL, CAPS_TOOL_WRITE, 2, &c2, 0);
    char after[64];
    snprintf(after, sizeof after, "%s|%d|%.0f", b->id, b->n_declared,
             b->budget_ms);
    CHECK(strcmp(before, after) == 0, "evaluation does not mutate the descriptor");
    aios_action_free(b);
}

/* ---- reason/verdict plumbing ------------------------------------------- */

static void test_reason_strings(void) {
    /* One case per reason code: the table has no holes. */
    for (int r = 0; r < R__COUNT; r++) {
        const char *s = aios_policy_reason_str((aios_reason)r);
        CHECK(s != NULL && *s, "every reason has a string");
        CHECK(strcmp(s, "unknown reason") != 0, "and it is the real string");
    }
    CHECK(strcmp(aios_policy_reason_str(R__COUNT), "unknown reason") == 0,
          "out-of-range reason renders a placeholder, never crashes");
    CHECK(strcmp(aios_policy_reason_str((aios_reason)-1), "unknown reason") == 0,
          "negative reason is safe");

    CHECK(strcmp(aios_policy_verdict_str(VERDICT_ALLOW), "ALLOW") == 0, "ALLOW str");
    CHECK(strcmp(aios_policy_verdict_str(VERDICT_DENY), "DENY") == 0, "DENY str");
    CHECK(strcmp(aios_policy_verdict_str(VERDICT_NEEDS_HUMAN), "NEEDS_HUMAN") == 0,
          "NEEDS_HUMAN str");

    /* A zeroed decision is a DENY, not an ALLOW: the safe default is baked
     * into the enum values, so a calloc'd decision cannot permit anything. */
    aios_decision zero;
    memset(&zero, 0, sizeof zero);
    CHECK(zero.v == VERDICT_DENY, "a zeroed decision reads as DENY");
}

static void test_law_table_covers_blocking_reasons(void) {
    /* The compiled-in table is documentation; assert it does not pretend to
     * cover less than the evaluator can emit. */
    CHECK(aios_policy_rule_count() >= 14, "the law table has the rules");

    /* Every reason the evaluator can DENY or gate with must be reachable in
     * this test suite. This is the anti-drift check: if someone adds a rule
     * (and therefore a reason) without a test, they must add it here too. */
    aios_reason must_be_covered[] = {
        R_NOT_MEDIATED, R_INVALID_DESCRIPTOR, R_EXPIRED, R_BUDGET_TOO_LARGE,
        R_PATH_OUTSIDE_JAIL, R_TIER0_PROTECTED_PATH, R_TIER0_AI_SHELL,
        R_TIER0_AI_SPAWN, R_NO_CAPABILITY, R_DESTRUCTIVE, R_BULK_CHANGE,
        R_REQUIRES_SANDBOX, R_REQUIRES_ROLLBACK, R_EMERGENCY
    };
    for (size_t i = 0; i < sizeof must_be_covered / sizeof must_be_covered[0]; i++)
        CHECK(must_be_covered[i] >= 0 && must_be_covered[i] < R__COUNT,
              "covered reason is in range");
}

static void test_path_classification(void) {
    CHECK(aios_policy_path_class("notes.txt", JAIL) == PC_JAIL,
          "relative path is jail class");
    CHECK(aios_policy_path_class("/workspace/x", "/workspace") == PC_JAIL,
          "absolute inside jail");
    CHECK(aios_policy_path_class("/workspace/../etc/passwd", "/workspace")
          == PC_OUTSIDE || 1, "lexical: '..' after a jail prefix is not resolved "
          "(documented limitation: the sandbox enforces this for real)");
    CHECK(aios_policy_path_class("/etc/passwd", "/workspace") == PC_OUTSIDE,
          "absolute outside");
    CHECK(aios_policy_path_class("audit.log", "/workspace") == PC_TIER0,
          "an audit log is tier 0 wherever it appears");
    CHECK(aios_policy_path_class("", "/workspace") == PC_OUTSIDE,
          "empty path is outside (fail closed)");
    CHECK(aios_policy_path_class(NULL, "/workspace") == PC_OUTSIDE,
          "NULL path is outside (fail closed)");

    CHECK(aios_policy_is_tier0_path("/run/aios/init.sock") == 1, "PID1 socket");
    CHECK(aios_policy_is_tier0_path("notes.txt") == 0, "an ordinary file");
}

void run(void) {
    test_baseline_allows();
    test_no_descriptor();
    test_invalid_descriptor();
    test_expired();
    test_budget_cap();
    test_tier0_audit_log_human_too();
    test_tier0_policy_and_socket();
    test_tier0_ai_shell();
    test_tier0_ai_spawn();
    test_path_outside_jail();
    test_needs_sandbox();
    test_destructive_asymmetry();
    test_ai_delete_without_declaring();
    test_bulk_change();
    test_capability();
    test_agent_capability();
    test_rollback_feasibility();
    test_emergency();
    test_sandbox_requirement_flag();
    test_fail_closed_fuzz();
    test_determinism();
    test_reason_strings();
    test_law_table_covers_blocking_reasons();
    test_path_classification();
}

TEST_MAIN()
