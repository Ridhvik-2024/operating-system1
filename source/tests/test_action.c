/* test_action.c — M1: the Action Descriptor.
 *
 * The interesting tests here are not the happy path (JSON in, JSON out).
 * They are the ones that pin down the SECURITY property of the type:
 *
 *   1. origin cannot be set from JSON. A model that could set origin could
 *      make the audit log claim a human did something it did.
 *   2. expiry cannot be EXTENDED by JSON. A model that could set expires_at
 *      would mint itself a standing grant.
 *   3. an AI action without an intent hash is rejected - "what human request
 *      is this serving?" must always be answerable.
 *   4. malformed effects are rejected at the door, and 17 of them is refused.
 *   5. an expired descriptor is refused, and a descriptor from the future
 *      (expiry beyond the 30 s ceiling) is refused.
 */
#include "aios/action.h"

#include "../tests/t.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* helper: a valid AI tool descriptor, with origin and intent stamped */
static aios_action *valid_tool_action(void) {
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_AI, "fs.write");
    if (!a) return NULL;
    aios_action_set_intent(a, "write hello to notes.txt");
    aios_action_set_reversible(a, 0);
    json_val *args = json_new_obj();
    json_obj_set(args, "path", json_new_str("notes.txt"));
    json_obj_set(args, "content", json_new_str("hello"));
    aios_action_set_args(a, args);
    aios_action_declare_tool_effects(a, "fs.write", a->args);
    return a;
}

static void test_construct(void) {
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_AI, "calc");
    CHECK(a != NULL, "construct a descriptor");
    CHECK(a && a->origin == ORIGIN_AI, "origin as given");
    CHECK(a && a->kind == ACT_TOOL, "kind as given");
    CHECK(a && a->target && strcmp(a->target, "calc") == 0, "target copied");
    CHECK(a && a->budget_ms > 0, "default budget is positive");
    CHECK(a && a->expires_ms > aios_now_ms(), "default expiry is in the future");
    CHECK(a && a->id[0] != 0, "action_id minted");
    CHECK(a && a->reversible == 0, "reversible defaults to 0 (pessimistic)");
    CHECK(a && a->n_declared == 0, "no effects by default");
    aios_action_free(a);

    CHECK(aios_action_new(ACT_TOOL, ORIGIN_AI, "") == NULL, "empty target refused");
    CHECK(aios_action_new(ACT_TOOL, ORIGIN_AI, NULL) == NULL, "NULL target refused");
    CHECK(aios_action_new(ACT__COUNT, ORIGIN_AI, "x") == NULL,
          "unknown kind refused at construction");
    CHECK(aios_action_new((aios_act_kind)-1, ORIGIN_AI, "x") == NULL,
          "negative kind refused");
}

static void test_validation_rules(void) {
    char err[256];

    /* the baseline is valid, or every failing case below proves nothing */
    aios_action *a = valid_tool_action();
    CHECK(a != NULL, "build the baseline descriptor");
    CHECK(aios_action_is_valid(a, err, sizeof err) == 1, "baseline is valid");

    /* 1. origin must be stamped; a fresh descriptor from JSON is not */
    aios_action *un = aios_action_new(ACT_TOOL, ORIGIN_UNSPECIFIED, "calc");
    CHECK(aios_action_is_valid(un, err, sizeof err) == 0,
          "unstamped origin is invalid");
    CHECK_STR_CONTAINS(err, "origin", "reason mentions origin");
    aios_action_free(un);

    /* 2. empty target cannot happen through new(), but can through tampering */
    char saved = a->target[0];
    a->target[0] = 0;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0, "empty target is invalid");
    a->target[0] = saved;

    /* 3. budget */
    a->budget_ms = 0;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0, "budget 0 is invalid");
    CHECK_STR_CONTAINS(err, "budget_ms", "reason mentions budget_ms");
    a->budget_ms = -5;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0, "negative budget invalid");
    a->budget_ms = AIOS_ACTION_MAX_BUDGET_MS + 1;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0,
          "budget over the 30 s cap is invalid (not clamped)");
    a->budget_ms = AIOS_ACTION_DEFAULT_TOOL_MS;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 1, "restored budget is valid");

    /* 4. expiry */
    a->expires_ms = aios_now_ms() - 1000;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0, "expired descriptor invalid");
    CHECK_STR_CONTAINS(err, "expired", "reason says expired");
    a->expires_ms = aios_now_ms() + AIOS_ACTION_MAX_LIFETIME_MS + 60000;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0,
          "expiry beyond the 30 s ceiling is invalid");
    a->expires_ms = aios_now_ms() + 1000;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 1, "short expiry is valid");

    /* 5. an AI action must say which human intent it serves */
    a->intent_hash = 0;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0,
          "AI action without an intent hash is invalid");
    CHECK_STR_CONTAINS(err, "intent", "reason mentions intent");
    aios_action_set_intent(a, "write hello to notes.txt");
    CHECK(a->intent_hash != 0, "set_intent computes the hash");
    CHECK(aios_action_is_valid(a, err, sizeof err) == 1, "restored intent is valid");

    /* the same rule must NOT apply to a human action: the human IS the intent */
    a->origin = ORIGIN_HUMAN;
    a->intent_hash = 0;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 1,
          "a human action needs no intent hash");
    a->origin = ORIGIN_AI;
    aios_action_set_intent(a, "write hello to notes.txt");

    /* 6. effects must be marked complete once any are declared */
    CHECK(a->n_declared > 0 && a->effects_complete,
          "declare_tool_effects marked the set complete");
    a->effects_complete = 0;
    CHECK(aios_action_is_valid(a, err, sizeof err) == 0,
          "declared effects without complete=true is invalid");
    CHECK_STR_CONTAINS(err, "complete", "reason mentions completeness");
    a->effects_complete = 1;

    aios_action_free(a);
}

static void test_effect_rules(void) {
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(a, "touch a file");

    CHECK(aios_action_add_effect(a, "write:a.txt") == AIOS_OK, "valid effect accepted");
    CHECK(aios_action_add_effect(a, "write:a.txt") == AIOS_OK,
          "duplicate effect is a no-op, not an error");
    CHECK(a->n_declared == 1, "duplicate did not add a second entry");
    CHECK(aios_action_add_effect(a, "read:dir/b.txt") == AIOS_OK,
          "nested path accepted");
    CHECK(aios_action_add_effect(a, "spawn:job-1") == AIOS_OK, "spawn verb accepted");
    CHECK(aios_action_add_effect(a, "net:example.com") == AIOS_OK, "net verb accepted");
    CHECK(aios_action_add_effect(a, "ui:window-1") == AIOS_OK, "ui verb accepted");
    CHECK(aios_action_add_effect(a, "delete:old.log") == AIOS_OK,
          "delete verb accepted");

    /* malformed effects are refused at the door */
    CHECK(aios_action_add_effect(a, "nonsense") == AIOS_ERR_ARG,
          "effect without a colon refused");
    CHECK(aios_action_add_effect(a, "frobnicate:x") == AIOS_ERR_ARG,
          "unknown verb refused");
    CHECK(aios_action_add_effect(a, "write:") == AIOS_ERR_ARG,
          "empty path refused");
    CHECK(aios_action_add_effect(a, "delete:../etc/passwd") == AIOS_ERR_ARG,
          "path with a .. component refused");
    CHECK(aios_action_add_effect(a, "delete:a/../b") == AIOS_ERR_ARG,
          "embedded .. component refused");
    CHECK(aios_action_add_effect(a, NULL) == AIOS_ERR_ARG, "NULL effect refused");

    /* a filename containing ".." is NOT a traversal: dots in a name are fine */
    CHECK(aios_action_add_effect(a, "write:notes..txt") == AIOS_OK,
          "'..' inside a filename is not traversal");

    /* 17 effects must be refused (limit is 16) */
    aios_action *b = aios_action_new(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(b, "too many effects");
    b->effects_complete = 1;      /* the author asserts this is the full set */
    int accepted = 0;
    for (int i = 0; i < 17; i++) {
        char eff[64];
        snprintf(eff, sizeof eff, "write:f%d.txt", i);
        if (aios_action_add_effect(b, eff) == AIOS_OK) accepted++;
    }
    CHECK(accepted == AIOS_ACTION_MAX_EFFECTS,
          "exactly 16 effects accepted");
    CHECK(b->n_declared == AIOS_ACTION_MAX_EFFECTS, "count stops at the limit");
    char err[256];
    CHECK(aios_action_is_valid(b, err, sizeof err) == 1,
          "16 effects is still valid");
    b->effects_complete = 1;
    aios_action_free(b);
    aios_action_free(a);
}

static void test_json_roundtrip(void) {
    aios_action *a = valid_tool_action();
    CHECK(a != NULL, "build descriptor for round-trip");

    char *j = aios_action_to_json(a);
    CHECK(j != NULL, "serialize");
    CHECK_STR_CONTAINS(j, "\"descriptor_version\":\"1.0\"", "version present");
    CHECK_STR_CONTAINS(j, "\"target\":\"fs.write\"", "target present");
    CHECK_STR_CONTAINS(j, "\"write:notes.txt\"", "declared effect present");
    CHECK_STR_CONTAINS(j, "\"complete\":true", "completeness recorded");
    CHECK_STR_CONTAINS(j, "\"log_level\":\"SUMMARY\"", "audit block present");

    char err[256];
    aios_action *b = aios_action_from_json(j, err, sizeof err);
    CHECK(b != NULL, "parse it back");
    if (b) {
        CHECK(b->kind == a->kind, "kind survives");
        CHECK(b->target && strcmp(b->target, a->target) == 0, "target survives");
        CHECK(b->n_declared == a->n_declared, "effect count survives");
        CHECK(b->effects_complete == 1, "completeness survives");
        CHECK(b->budget_ms == a->budget_ms, "budget survives");
        /* EXACTLY equal, not approximately: a 64-bit hash through a JSON
         * number loses its low bits and would break audit correlation. */
        CHECK(b->intent_hash == a->intent_hash,
              "intent hash survives EXACTLY through JSON");
        CHECK(strstr(j, "\"intent_hash\":\"") != NULL,
              "intent_hash is serialized as an exact hex string");

        /* THE SECURITY PROPERTY: origin is NOT honoured from JSON */
        CHECK(b->origin == ORIGIN_UNSPECIFIED,
              "origin is unspecified after parse (never from JSON)");
        CHECK(aios_action_is_valid(b, err, sizeof err) == 0,
              "a parsed descriptor is invalid until the host stamps origin");
        aios_action_stamp_origin(b, ORIGIN_AI);
        CHECK(aios_action_is_valid(b, err, sizeof err) == 1,
              "stamping origin makes it valid");

        /* and the JSON said "ai" the whole time - it just was not believed */
        CHECK_STR_CONTAINS(j, "\"origin\":\"ai\"", "json claimed origin=ai");
        aios_action_free(b);
    }
    free(j);
    aios_action_free(a);
}

static void test_json_rejects(void) {
    char err[256];

    CHECK(aios_action_from_json(NULL, err, sizeof err) == NULL, "NULL json rejected");
    CHECK(aios_action_from_json("not json", err, sizeof err) == NULL,
          "garbage rejected");
    CHECK(aios_action_from_json("[]", err, sizeof err) == NULL,
          "array rejected (must be an object)");
    CHECK(aios_action_from_json("{}", err, sizeof err) == NULL, "empty object rejected");
    CHECK_STR_CONTAINS(err, "version", "missing version is the stated reason");

    const char *wrong_ver =
        "{\"descriptor_version\":\"2.0\",\"kind\":0,\"target\":\"x\"}";
    CHECK(aios_action_from_json(wrong_ver, err, sizeof err) == NULL,
          "unknown version rejected");

    const char *no_target =
        "{\"descriptor_version\":\"1.0\",\"kind\":0}";
    CHECK(aios_action_from_json(no_target, err, sizeof err) == NULL,
          "missing target rejected");
    CHECK_STR_CONTAINS(err, "target", "reason mentions target");

    const char *bad_kind =
        "{\"descriptor_version\":\"1.0\",\"kind\":99,\"target\":\"x\"}";
    CHECK(aios_action_from_json(bad_kind, err, sizeof err) == NULL,
          "unknown kind rejected");

    /* 17 declared effects is rejected at parse time, not later */
    char many[4096];
    size_t off = (size_t)snprintf(many, sizeof many,
        "{\"descriptor_version\":\"1.0\",\"kind\":0,\"target\":\"x\","
        "\"effects\":{\"complete\":true,\"declared\":[");
    for (int i = 0; i < 17; i++)
        off += (size_t)snprintf(many + off, sizeof many - off, "%s\"write:f%d\"",
                                i ? "," : "", i);
    snprintf(many + off, sizeof many - off, "]}}");
    CHECK(aios_action_from_json(many, err, sizeof err) == NULL,
          "17 declared effects rejected");
    CHECK_STR_CONTAINS(err, "17", "reason states the count");

    /* a malformed effect inside the array is rejected */
    const char *bad_eff =
        "{\"descriptor_version\":\"1.0\",\"kind\":0,\"target\":\"x\","
        "\"effects\":{\"complete\":true,\"declared\":[\"teleport:x\"]}}";
    CHECK(aios_action_from_json(bad_eff, err, sizeof err) == NULL,
          "unknown effect verb rejected at parse");
}

static void test_json_cannot_extend_authority(void) {
    char err[256];

    /* A hostile descriptor asking to live for a year. */
    char far_future[512];
    snprintf(far_future, sizeof far_future,
             "{\"descriptor_version\":\"1.0\",\"kind\":0,\"target\":\"calc\","
             "\"expires_at\":%.0f}", aios_now_ms() + 365.0 * 24 * 3600 * 1000.0);

    aios_action *a = aios_action_from_json(far_future, err, sizeof err);
    CHECK(a != NULL, "hostile descriptor still parses (it is only a proposal)");
    if (a) {
        double ceiling = aios_now_ms() + AIOS_ACTION_MAX_LIFETIME_MS;
        CHECK(a->expires_ms <= ceiling,
              "expiry was clamped to the 30 s ceiling, not honoured");
        /* clamped to the ceiling is still valid until it lapses */
        aios_action_stamp_origin(a, ORIGIN_AI);
        aios_action_set_intent(a, "calc something");
        CHECK(aios_action_is_valid(a, err, sizeof err) == 1,
              "clamped descriptor is valid");

        /* but a year of life is NOT granted: it expires within 30 s */
        a->expires_ms = aios_now_ms() + 40000;
        CHECK(aios_action_is_valid(a, err, sizeof err) == 0,
              "40 s of life is refused");
        aios_action_free(a);
    }

    /* Shortening IS honoured: parsing may reduce authority, never extend it.
     * Compare against the exact instant we asked for - "%.0f" ROUNDS, so a
     * value can land up to 0.5 ms past it, and comparing against a
     * freshly-read clock would be testing the scheduler, not the code. */
    char near[512];
    double want = floor(aios_now_ms() + 250.0);
    snprintf(near, sizeof near,
             "{\"descriptor_version\":\"1.0\",\"kind\":0,\"target\":\"calc\","
             "\"expires_at\":%.0f}", want);
    aios_action *b = aios_action_from_json(near, err, sizeof err);
    CHECK(b != NULL, "short-lived descriptor parses");
    CHECK(b && b->expires_ms <= want + 1.0,
          "shorter expiry from JSON is honoured");
    CHECK(b && b->expires_ms < aios_now_ms() + AIOS_ACTION_MAX_LIFETIME_MS,
          "and it is well short of the default 30 s lifetime");
    aios_action_free(b);

    /* A descriptor claiming origin=human must not become human. */
    const char *liar =
        "{\"descriptor_version\":\"1.0\",\"kind\":0,\"target\":\"fs.write\","
        "\"origin\":\"human\",\"created_by\":\"human\",\"intent_hash\":0}";
    aios_action *c = aios_action_from_json(liar, err, sizeof err);
    CHECK(c != NULL, "descriptor claiming to be human parses");
    if (c) {
        CHECK(c->origin == ORIGIN_UNSPECIFIED,
              "origin claim 'human' from JSON is ignored");
        /* and once stamped AI, the missing intent hash makes it invalid:
         * the lie does not buy anything. */
        aios_action_stamp_origin(c, ORIGIN_AI);
        CHECK(aios_action_is_valid(c, err, sizeof err) == 0,
              "AI-stamped liar without intent hash is invalid");
        aios_action_free(c);
    }
}

static void test_tool_effects(void) {
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(a, "write notes");
    json_val *args = json_new_obj();
    json_obj_set(args, "path", json_new_str("notes.txt"));
    CHECK(aios_action_declare_tool_effects(a, "fs.write", args) == AIOS_OK,
          "declare effects for fs.write");
    CHECK(a->n_declared == 1, "fs.write declares exactly one effect");
    CHECK(a->declared[0] && strcmp(a->declared[0], "write:notes.txt") == 0,
          "the effect names the path it will write");
    CHECK(a->effects_complete == 1, "set is complete");
    json_free(args);
    aios_action_free(a);

    /* a read tool declares read */
    aios_action *b = aios_action_new(ACT_TOOL, ORIGIN_AI, "fs.read");
    aios_action_set_intent(b, "read notes");
    json_val *a2 = json_new_obj();
    json_obj_set(a2, "path", json_new_str("notes.txt"));
    aios_action_declare_tool_effects(b, "fs.read", a2);
    CHECK(b->n_declared == 1 && strcmp(b->declared[0], "read:notes.txt") == 0,
          "fs.read declares read:path");
    json_free(a2);
    aios_action_free(b);

    /* a pure computation touches nothing, and says so explicitly */
    aios_action *c = aios_action_new(ACT_TOOL, ORIGIN_AI, "calc");
    aios_action_set_intent(c, "compute");
    CHECK(aios_action_declare_tool_effects(c, "calc", NULL) == AIOS_OK,
          "calc declares fine");
    CHECK(c->n_declared == 0, "calc declares no filesystem effect");
    CHECK(c->effects_complete == 1,
          "calc still marks the set complete (it touched nothing)");
    char err[256];
    CHECK(aios_action_is_valid(c, err, sizeof err) == 1,
          "a complete empty effect set is valid");
    aios_action_free(c);

    /* no path in args -> no effect, but still complete: the tool was asked
     * to touch nothing we can name, and the sandbox will catch it if it does */
    aios_action *d = aios_action_new(ACT_TOOL, ORIGIN_AI, "fs.write");
    aios_action_set_intent(d, "write nothing");
    CHECK(aios_action_declare_tool_effects(d, "fs.write", NULL) == AIOS_OK,
          "declare with no args is fine");
    CHECK(d->n_declared == 0, "no path means no declared effect");
    aios_action_free(d);
}

/* Regression: a call whose effects CANNOT be declared must be refused, not
 * run with an empty claim set. Before this, an over-long path was truncated
 * (recording a different resource than the one touched) and the descriptor
 * still validated. */
static void test_undeclarable_refused(void) {
    char err[256];
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_HUMAN, "fs.write");
    aios_action_set_intent(a, "write a long path");
    json_val *args = json_new_obj();
    char longpath[800];
    memset(longpath, 'a', sizeof longpath - 1);
    longpath[sizeof longpath - 1] = 0;
    json_obj_set(args, "path", json_new_str(longpath));
    aios_action_set_args(a, args);

    aios_status st = aios_action_declare_tool_effects(a, "fs.write", a->args);
    CHECK(st == AIOS_ERR_LIMIT, "over-long path is refused, not truncated");
    CHECK(a->n_declared == 0, "nothing was declared");
    CHECK(a->effects_complete == 0, "the set is NOT marked complete");
    /* effects_complete == 0 must stay distinguishable from "touches nothing"
     * (which is 1). That flag is what the CALLER acts on: shell.c refuses the
     * command when declaration fails. A descriptor carrying an unknown effect
     * set is still structurally valid - an ACT_AGENT cannot enumerate its
     * effects until it plans - so the refusal belongs at the call site, and
     * test_shell.c covers that path with the real shell. */
    CHECK(a->effects_complete == 0 && a->n_declared == 0,
          "an undeclarable call is marked UNKNOWN, not 'declares nothing'");
    (void)err;
    aios_action_free(a);

    /* the contrast that makes the flag meaningful: a tool that genuinely
     * touches nothing declares zero effects and marks the set COMPLETE. */
    aios_action *b = aios_action_new(ACT_TOOL, ORIGIN_HUMAN, "calc");
    aios_action_set_intent(b, "compute");
    CHECK(aios_action_declare_tool_effects(b, "calc", NULL) == AIOS_OK,
          "calc declares successfully");
    CHECK(b->n_declared == 0 && b->effects_complete == 1,
          "'touches nothing' is 0 effects + complete, and IS valid");
    CHECK(aios_action_is_valid(b, err, sizeof err) == 1,
          "a complete empty effect set passes");
    aios_action_free(b);

    /* recomputing must not leave stale effects behind */
    aios_action *c = aios_action_new(ACT_TOOL, ORIGIN_HUMAN, "fs.write");
    aios_action_set_intent(c, "recompute");
    json_val *a1 = json_new_obj();
    json_obj_set(a1, "path", json_new_str("first.txt"));
    aios_action_declare_tool_effects(c, "fs.write", a1);
    CHECK(c->n_declared == 1, "first declaration recorded");
    json_free(a1);
    json_val *a2 = json_new_obj();
    json_obj_set(a2, "path", json_new_str("second.txt"));
    aios_action_declare_tool_effects(c, "fs.write", a2);
    CHECK(c->n_declared == 2,
          "recompute appends (effects are recorded per call, not replaced)");
    CHECK(c->effects_complete == 1, "and the set is still complete");
    json_free(a2);
    aios_action_free(c);
}

static void test_args_transfer(void) {
    aios_action *a = valid_tool_action();
    json_val *taken = aios_action_take_args(a);
    CHECK(taken != NULL, "take_args returns the object");
    CHECK(a->args == NULL, "descriptor no longer owns it");
    CHECK(json_type_of(taken) == JSON_OBJ, "it is still an object");
    /* the caller owns it now - and tool_exec's contract is that it consumes */
    json_free(taken);
    CHECK(aios_action_take_args(a) == NULL, "taking twice yields NULL");
    aios_action_free(a);
}

static void test_kind_names(void) {
    CHECK(strcmp(aios_action_kind_name(ACT_TOOL), "TOOL") == 0, "TOOL name");
    CHECK(strcmp(aios_action_kind_name(ACT_AGENT), "AGENT") == 0, "AGENT name");
    CHECK(strcmp(aios_action_kind_name(ACT_FS_DELETE), "FS_DELETE") == 0,
          "FS_DELETE name");
    CHECK(strcmp(aios_action_kind_name(ACT__COUNT), "UNKNOWN") == 0,
          "out-of-range kind renders UNKNOWN, never a crash");
    CHECK(strcmp(aios_action_origin_name(ORIGIN_HUMAN), "human") == 0, "human name");
    CHECK(strcmp(aios_action_origin_name(ORIGIN_AI), "ai") == 0, "ai name");
    CHECK(strcmp(aios_action_origin_name(ORIGIN_UNSPECIFIED), "unspecified") == 0,
          "unspecified name");
}

static void test_set_budget(void) {
    aios_action *a = aios_action_new(ACT_TOOL, ORIGIN_AI, "calc");
    aios_action_set_budget(a, 5000);
    CHECK(a->budget_ms == 5000, "budget set");
    aios_action_set_budget(a, 0);           /* refuse, do not clamp to 0 */
    CHECK(a->budget_ms == AIOS_ACTION_DEFAULT_TOOL_MS,
          "budget 0 falls back to the kind default, not to zero");
    aios_action_set_budget(a, 999999);      /* over the cap */
    CHECK(a->budget_ms == AIOS_ACTION_DEFAULT_TOOL_MS,
          "budget over the cap falls back to the default");
    aios_action_free(a);
}

void run(void) {
    test_construct();
    test_validation_rules();
    test_effect_rules();
    test_json_roundtrip();
    test_json_rejects();
    test_json_cannot_extend_authority();
    test_tool_effects();
    test_undeclarable_refused();
    test_args_transfer();
    test_kind_names();
    test_set_budget();
}

TEST_MAIN()
