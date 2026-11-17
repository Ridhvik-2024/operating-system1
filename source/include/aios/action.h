/* aios/action.h — M1: the Action Descriptor.
 *
 * THE CHOKE POINT. Every action an agent (or the router, or a script) wants
 * to take is represented by one aios_action BEFORE it happens. It is the
 * unit of policy evaluation (M2), the unit of sandboxing (M3/M4), the unit
 * of rollback (M5) and the unit of audit (M6). Nothing executes without one.
 *
 * Design law that matters more than the field list:
 *
 *   1. CAPABILITY MAY SCALE; AUTHORITY MAY NOT.
 *      A descriptor *proposes*. It never executes, never grants, never
 *      decides. Everything privileged is decided by host code that read it.
 *
 *   2. TRUST FIELDS NEVER COME FROM PARSED JSON.
 *      `origin`, `intent_hash`, `reversible` and `expires_ms` are stamped by
 *      the host, never deserialized. If a model could set `origin`, the audit
 *      log would lie about who acted; if it could set `expires_ms` far in the
 *      future it would mint itself a durable grant. aios_action_from_json()
 *      therefore leaves origin UNSPECIFIED (so validation fails until the
 *      host stamps one) and CLAMPS expiry to now + 30 s.
 *
 *      The asymmetry is deliberate: parsing may silently *reduce* authority
 *      (shorter life) but never *extend* it.
 *
 *   3. DECLARE, THEN PROVE.
 *      `declared[]` is the effect set the action claims it will have. It is a
 *      claim, not a permission. The sandbox reports what actually happened
 *      (M4) and undeclared effects are blocked. Under-declaring does not
 *      hide anything; it only makes the mismatch visible.
 *
 * JSON shape (descriptor_version "1.0", see action_descriptor.schema.md):
 *
 *   { "descriptor_version":"1.0", "action_id":"...", "created_at":1700000000000,
 *     "created_by":"ai"|"human",
 *     "action_type":"TOOL", "kind":0,
 *     "intent_summary":"...", "target":"fs.write",
 *     "args":{...}, "effects":{"complete":true,"declared":["write:a.txt"]},
 *     "resources":{"max_duration_ms":10000},
 *     "rollback":{"supported":true},
 *     "origin":"ai"            <- emitted for the record, IGNORED on parse
 *   }
 */
#ifndef AIOS_ACTION_H
#define AIOS_ACTION_H

#include <stddef.h>

#include "aios/types.h"
#include "aios/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_ACTION_VERSION        "1.0"
#define AIOS_ACTION_MAX_EFFECTS    16
#define AIOS_ACTION_MAX_BUDGET_MS  30000.0   /* hard cap on declared wall clock */
#define AIOS_ACTION_MAX_LIFETIME_MS 30000.0  /* how long a descriptor stays valid */
#define AIOS_ACTION_MAX_TARGET     512
#define AIOS_ACTION_MAX_ARGS_JSON  4096
#define AIOS_ACTION_MAX_EFFECT     600
#define AIOS_ACTION_DEFAULT_TOOL_MS  10000.0

/* What kind of thing is being proposed. ACT_TOOL and ACT_SHELL cover
 * "run a registered tool" and "run a shell command"; the rest exist so the
 * policy table (M2) can key on the *intent*, not on a tool name string. */
typedef enum {
    ACT_TOOL = 0,      /* one registered tool from the M4 registry   */
    ACT_AGENT,         /* hand a goal to the agent runtime           */
    ACT_FS_WRITE,      /* write/replace a path                       */
    ACT_FS_DELETE,     /* remove a path                              */
    ACT_JOB_SPAWN,     /* background job                             */
    ACT_SHELL,         /* shell command execution                    */
    ACT__COUNT
} aios_act_kind;

/* Who is accountable for this action.
 *
 * This is NOT "who typed the line". A human typing a natural-language
 * sentence makes the ROUTER choose the tool, so the action is AI. A human
 * typing an explicit `fs.write path=a content=b` named the tool and the
 * arguments themselves, so it is HUMAN. The distinction is the whole point
 * of M7's origin tagging, and it is decided by the code path that built the
 * descriptor - never by the model. */
typedef enum {
    ORIGIN_UNSPECIFIED = -1,   /* not yet stamped -> descriptor is INVALID */
    ORIGIN_HUMAN       = 0,
    ORIGIN_AI          = 1
} aios_origin;

typedef struct {
    aios_act_kind kind;
    aios_origin   origin;              /* host-stamped; UNSPECIFIED is invalid */

    char         *target;              /* tool name / goal text / path        */
    json_val     *args;                /* tool args or payload (owned, may be
                                          NULL). json_obj_take()s it away.   */

    /* Effects claimed in advance, "verb:path" — verb in {read,write,delete,
     * spawn,net,ui}. At most AIOS_ACTION_MAX_EFFECTS. */
    char         *declared[AIOS_ACTION_MAX_EFFECTS];
    int           n_declared;

    /* 1 = the author asserts declared[] is the COMPLETE effect set.
     * Without this, "no effects" and "forgot to declare" are
     * indistinguishable, and the schema calls that a hard fail. */
    int           effects_complete;

    double        budget_ms;           /* wall clock this may take, (0,30000] */
    int           reversible;          /* 1 = fully undoable TODAY (see M3)   */
    uint64_t      intent_hash;         /* aios_fnv1a of the human intent text */
    double        expires_ms;          /* void after this (unix ms)           */

    char          id[40];              /* action_id, for audit correlation    */
} aios_action;

/* Construct. origin may be ORIGIN_UNSPECIFIED; then is_valid() fails until
 * aios_action_stamp_origin() is called. Returns NULL on bad args / no mem. */
aios_action *aios_action_new(aios_act_kind kind, aios_origin origin,
                             const char *target);
void         aios_action_free(aios_action *a);

/* Stamp the trusted fields. Only host code calls these - a model never can,
 * because they are not reachable from from_json(). */
void  aios_action_stamp_origin(aios_action *a, aios_origin o);
void  aios_action_set_intent(aios_action *a, const char *intent_text);
void  aios_action_set_reversible(aios_action *a, int reversible);
void  aios_action_set_budget(aios_action *a, double ms);
void  aios_action_set_args(aios_action *a, json_val *args); /* takes ownership */

/* Take args out of the descriptor (transferring ownership to the caller,
 * leaving a->args NULL). This is how exec_tool keeps tool_exec's
 * "args is consumed" contract without copying the JSON. */
json_val *aios_action_take_args(aios_action *a);

/* Declare one effect. Returns AIOS_OK, or AIOS_ERR_LIMIT past 16, or
 * AIOS_ERR_ARG if the effect is malformed (unknown verb, empty path, a ".."
 * component, or over-long). Malformed effects are rejected AT THE DOOR so
 * that a bad descriptor can never reach the policy table. */
aios_status aios_action_add_effect(aios_action *a, const char *effect);

/* Convenience: declare the effects a given tool+args pair implies, and mark
 * the set complete. This is the single place that knows "fs.write touches
 * path", used by both the explicit-command path and the router path. */
aios_status aios_action_declare_tool_effects(aios_action *a, const char *tool,
                                             const json_val *args);

/* Default budget for a kind (tools 10 s, agents 30 s). */
double aios_action_default_budget(aios_act_kind kind);
const char *aios_action_kind_name(aios_act_kind kind);
const char *aios_action_origin_name(aios_origin o);

/* 1 = valid, 0 = invalid with a human-readable reason in err.
 * Deliberately re-checks EVERYTHING: expiry against now, budget caps, effect
 * verbs, the origin stamp, and the AI-needs-an-intent rule. */
int aios_action_is_valid(const aios_action *a, char *err, size_t ecap);

/* Serialize (owned string, caller frees). Includes every trusted field so the
 * audit record is complete. */
char *aios_action_to_json(const aios_action *a);

/* Parse. origin is left UNSPECIFIED and expiry is clamped (never extended),
 * so the caller must stamp origin before is_valid() will pass. Accepts the
 * "origin" member for round-trip fidelity but never honours it. */
aios_action *aios_action_from_json(const char *json, char *err, size_t ecap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_ACTION_H */
