/* action.c — M1: the Action Descriptor.
 *
 * See include/aios/action.h for the design law. This file is deliberately
 * boring: no I/O, no globals, no printing. It is the type every later
 * milestone keys on, so it must be trivially unit-testable.
 *
 * The one subtle function here is aios_action_from_json(). It is written to
 * an attacker's specification: the JSON it parses is untrusted (it may come
 * from a model), so every field that carries authority is either ignored,
 * clamped, or re-derived. Trust is stamped afterwards by host code.
 */
#include "aios/action.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- helpers */

/* Effect verbs: the fixed list. A "verb:path" effect must start with one of
 * these, and the path half must be non-empty, relative-free of "..". */
static const char *const EFFECT_VERBS[] = {
    "read", "write", "delete", "spawn", "net", "ui", NULL
};

static int verb_known(const char *v, size_t n) {
    for (size_t i = 0; EFFECT_VERBS[i]; i++) {
        if (strlen(EFFECT_VERBS[i]) == n && strncmp(EFFECT_VERBS[i], v, n) == 0)
            return 1;
    }
    return 0;
}

/* A path half is acceptable if non-empty, NUL-free by construction, and does
 * not climb out with a ".." component. Absolute is NOT required: tools take
 * jail-relative paths ("notes.txt"), and the jail is applied by the sandbox. */
static int path_half_ok(const char *p, size_t n) {
    if (n == 0 || n > AIOS_ACTION_MAX_EFFECT - 16) return 0;
    /* reject ".." as a whole component */
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '.' && i + 1 < n && p[i + 1] == '.') {
            int left_ok  = (i == 0) || p[i - 1] == '/';
            int right_ok = (i + 2 == n) || p[i + 2] == '/';
            if (left_ok && right_ok) return 0;
        }
        if ((unsigned char)p[i] < 0x20) return 0;   /* control chars */
    }
    return 1;
}

static uint64_t fresh_u64(void) {
    /* action_id only needs to be unique within one store; time + address
     * entropy is enough and keeps this dependency-free. */
    static uint64_t ctr;
    uint64_t t = (uint64_t)(aios_now_ms() * 1000.0);
    uint64_t e = (uint64_t)(uintptr_t)&ctr;
    return t ^ (e << 17) ^ (++ctr * 0x9e3779b97f4a7c15ULL);
}

/* ------------------------------------------------------------------- life */

aios_action *aios_action_new(aios_act_kind kind, aios_origin origin,
                             const char *target) {
    if (kind < 0 || kind >= ACT__COUNT) return NULL;
    if (!target || !*target) return NULL;
    if (strlen(target) >= AIOS_ACTION_MAX_TARGET) return NULL;

    aios_action *a = (aios_action *)calloc(1, sizeof *a);
    if (!a) return NULL;

    a->kind     = kind;
    a->origin   = origin;
    a->target   = strdup(target);
    if (!a->target) { free(a); return NULL; }

    a->budget_ms         = aios_action_default_budget(kind);
    a->expires_ms        = aios_now_ms() + AIOS_ACTION_MAX_LIFETIME_MS;
    a->reversible        = 0;     /* pessimistic until someone says otherwise */
    a->effects_complete  = 0;
    a->intent_hash       = 0;
    snprintf(a->id, sizeof a->id, "act-%016llx",
             (unsigned long long)fresh_u64());
    return a;
}

void aios_action_free(aios_action *a) {
    if (!a) return;
    free(a->target);
    if (a->args) json_free(a->args);
    for (int i = 0; i < a->n_declared; i++) free(a->declared[i]);
    free(a);
}

/* ----------------------------------------------------------- host stamps */

void aios_action_stamp_origin(aios_action *a, aios_origin o) {
    if (a) a->origin = o;
}

void aios_action_set_intent(aios_action *a, const char *intent_text) {
    if (!a) return;
    a->intent_hash = intent_text
        ? aios_fnv1a(intent_text, strlen(intent_text))
        : 0;
}

void aios_action_set_reversible(aios_action *a, int reversible) {
    if (a) a->reversible = reversible ? 1 : 0;
}

void aios_action_set_budget(aios_action *a, double ms) {
    if (!a) return;
    if (ms <= 0 || ms > AIOS_ACTION_MAX_BUDGET_MS)
        ms = aios_action_default_budget(a->kind);   /* refuse, do not clamp up */
    a->budget_ms = ms;
}

void aios_action_set_args(aios_action *a, json_val *args) {
    if (!a) { if (args) json_free(args); return; }
    if (a->args) json_free(a->args);
    a->args = args;
}

json_val *aios_action_take_args(aios_action *a) {
    if (!a) return NULL;
    json_val *v = a->args;
    a->args = NULL;
    return v;
}

/* --------------------------------------------------------------- effects */

aios_status aios_action_add_effect(aios_action *a, const char *effect) {
    if (!a || !effect) return AIOS_ERR_ARG;
    if (a->n_declared >= AIOS_ACTION_MAX_EFFECTS) return AIOS_ERR_LIMIT;

    const char *colon = strchr(effect, ':');
    if (!colon) return AIOS_ERR_ARG;                     /* need verb:path */
    size_t vlen = (size_t)(colon - effect);
    if (!verb_known(effect, vlen)) return AIOS_ERR_ARG;
    if (!path_half_ok(colon + 1, strlen(colon + 1))) return AIOS_ERR_ARG;
    if (strlen(effect) > AIOS_ACTION_MAX_EFFECT) return AIOS_ERR_ARG;

    /* duplicate declaration is a no-op, not an error: the router and the
     * tool effect table may legitimately agree. */
    for (int i = 0; i < a->n_declared; i++)
        if (strcmp(a->declared[i], effect) == 0) return AIOS_OK;

    char *copy = strdup(effect);
    if (!copy) return AIOS_ERR_NOMEM;
    a->declared[a->n_declared++] = copy;
    return AIOS_OK;
}

/* The single place that knows which resources a tool touches. Kept next to
 * the descriptor (not in the tools registry) so the claim is written down in
 * one readable table instead of being inferred from code behaviour.
 *
 * Note what these are NOT: they are not permissions. A tool that touches
 * nothing declares nothing and still runs; a tool that touches a path
 * declares it and is then held to it by the M4 diff. */
/* Declare one "verb:path" effect for a path taken from tool args.
 *
 * The path comes from the call, NOT from the wrapper: an over-long path must
 * be declared as accurately as the field allows or refused. It must never be
 * silently truncated, because then the record would name a DIFFERENT resource
 * from the one that gets touched - exactly the lie this type exists to catch.
 * An over-long path is therefore AIOS_ERR_LIMIT (refuse, do not round). */
static aios_status declare_path_effect(aios_action *a, const char *verb,
                                       const char *path) {
    if (!path || !*path) return AIOS_OK;         /* nothing to declare */
    size_t need = strlen(verb) + 1 + strlen(path);
    if (need > AIOS_ACTION_MAX_EFFECT) return AIOS_ERR_LIMIT;
    char eff[AIOS_ACTION_MAX_EFFECT + 1];
    snprintf(eff, sizeof eff, "%s:%s", verb, path);
    return aios_action_add_effect(a, eff);
}

aios_status aios_action_declare_tool_effects(aios_action *a, const char *tool,
                                             const json_val *args) {
    if (!a || !tool) return AIOS_ERR_ARG;
    /* Never trust a previous claim to stand: recompute from scratch so that
     * calling this twice cannot leave stale effects from the first call. */
    a->effects_complete = 0;

    aios_status st = AIOS_OK;
    const char *p = args ? json_str(json_obj_get(args, "path")) : NULL;

    if (strcmp(tool, "fs.read") == 0 || strcmp(tool, "fs.list") == 0) {
        st = declare_path_effect(a, "read", p);
    } else if (strcmp(tool, "fs.write") == 0) {
        st = declare_path_effect(a, "write", p);
    }
    /* calc, memory.search, proc.info, selftest.*: no filesystem effect.
     * memory.search reads the store, which the host already owns - it is not
     * an effect on the user's reality. */

    /* The set is complete only if we actually managed to record every effect
     * we found. On failure we leave effects_complete = 0, which makes the
     * descriptor INVALID - the gate refuses it. Failing closed matters more
     * than running the tool: a call whose effects could not be declared is a
     * call nothing can later check. */
    if (st == AIOS_OK) a->effects_complete = 1;
    return st;
}

/* -------------------------------------------------------------- metadata */

double aios_action_default_budget(aios_act_kind kind) {
    switch (kind) {
    case ACT_TOOL:      return AIOS_ACTION_DEFAULT_TOOL_MS;
    case ACT_AGENT:     return AIOS_ACTION_MAX_BUDGET_MS;   /* 30 s: the cap */
    case ACT_JOB_SPAWN: return AIOS_ACTION_DEFAULT_TOOL_MS;
    default:            return AIOS_ACTION_DEFAULT_TOOL_MS;
    }
}

const char *aios_action_kind_name(aios_act_kind kind) {
    switch (kind) {
    case ACT_TOOL:      return "TOOL";
    case ACT_AGENT:     return "AGENT";
    case ACT_FS_WRITE:  return "FS_WRITE";
    case ACT_FS_DELETE: return "FS_DELETE";
    case ACT_JOB_SPAWN: return "JOB_SPAWN";
    case ACT_SHELL:     return "SHELL";
    default:            return "UNKNOWN";
    }
}

const char *aios_action_origin_name(aios_origin o) {
    switch (o) {
    case ORIGIN_HUMAN:       return "human";
    case ORIGIN_AI:          return "ai";
    case ORIGIN_UNSPECIFIED: return "unspecified";
    default:                 return "invalid";
    }
}

/* -------------------------------------------------------------- validate */

int aios_action_is_valid_at(const aios_action *a, double now_ms,
                            char *err, size_t ecap) {
    if (err && ecap) err[0] = 0;
    /* Every failure path goes through FAIL so the reason is never empty. */
#define FAIL(...) do { if (err && ecap) snprintf(err, ecap, __VA_ARGS__); \
                       return 0; } while (0)

    if (!a) FAIL("no descriptor");

    /* 1. origin must be stamped by the host */
    if (a->origin == ORIGIN_UNSPECIFIED)
        FAIL("origin not stamped (host must set human/ai; a model cannot)");
    if (a->origin != ORIGIN_HUMAN && a->origin != ORIGIN_AI)
        FAIL("origin is not human/ai");

    /* 2. kind must be one we understand: unknown kinds fail CLOSED */
    if (a->kind < 0 || a->kind >= ACT__COUNT)
        FAIL("unknown action kind %d", (int)a->kind);

    /* 3. target */
    if (!a->target || !*a->target) FAIL("empty target");
    if (strlen(a->target) >= AIOS_ACTION_MAX_TARGET)
        FAIL("target too long (%zu >= %d)", strlen(a->target),
             AIOS_ACTION_MAX_TARGET);

    /* 4. budget: present, positive, capped. Not clamped - refused. */
    if (!(a->budget_ms > 0.0)) FAIL("budget_ms must be > 0");
    if (a->budget_ms > AIOS_ACTION_MAX_BUDGET_MS)
        FAIL("budget_ms %.0f exceeds the %d ms cap", a->budget_ms,
             (int)AIOS_ACTION_MAX_BUDGET_MS);

    /* 5. expiry: a descriptor is a short-lived token, not a standing grant.
     *    now_ms <= 0 means the caller has no clock (pure evaluation); the
     *    relative ceiling is still enforced, only the absolute comparison is
     *    skipped. */
    if (now_ms > 0 && !(a->expires_ms > now_ms))
        FAIL("descriptor expired");
    if (now_ms > 0 && a->expires_ms > now_ms + AIOS_ACTION_MAX_LIFETIME_MS)
        FAIL("expiry too far in the future (max %d ms ahead)",
             (int)AIOS_ACTION_MAX_LIFETIME_MS);

    /* 6. effects: bounded, well-formed, and declared complete */
    if (a->n_declared < 0 || a->n_declared > AIOS_ACTION_MAX_EFFECTS)
        FAIL("too many declared effects (%d > %d)", a->n_declared,
             AIOS_ACTION_MAX_EFFECTS);
    for (int i = 0; i < a->n_declared; i++) {
        const char *e = a->declared[i];
        if (!e) FAIL("effect %d is NULL", i);
        const char *colon = strchr(e, ':');
        if (!colon) FAIL("effect '%s' is not verb:path", e);
        if (!verb_known(e, (size_t)(colon - e)))
            FAIL("effect '%s' has an unknown verb", e);
        if (!path_half_ok(colon + 1, strlen(colon + 1)))
            FAIL("effect '%s' has an unusable path", e);
    }
    if (a->n_declared > 0 && !a->effects_complete)
        FAIL("effects declared but the set is not marked complete");
    /* NOTE: effects_complete == 0 with zero effects is NOT an error here.
     * An ACT_AGENT proposal genuinely cannot enumerate its effects until it
     * plans (that is M3/M4's job), and a descriptor parsed from JSON has not
     * been through declare_tool_effects() yet. Treating "nobody looked" as
     * "touches nothing" would be worse than treating it as unknown - so the
     * flag stays meaningful as UNKNOWN, and the CALLER is responsible for
     * honouring a failed declaration (see the check in shell.c). */

    /* 7. an AI action must carry the hash of the human intent it serves.
     *    A human action need not: the human IS the intent. */
    if (a->origin == ORIGIN_AI && a->intent_hash == 0)
        FAIL("AI action without an intent hash (what human request is this?)");

    /* 8. args must be an object or null when present */
    if (a->args && json_type_of(a->args) != JSON_OBJ)
        FAIL("args must be a JSON object");

    return 1;
#undef FAIL
}

int aios_action_is_valid(const aios_action *a, char *err, size_t ecap) {
    return aios_action_is_valid_at(a, aios_now_ms(), err, ecap);
}

/* ------------------------------------------------------------------- JSON */

char *aios_action_to_json(const aios_action *a) {
    if (!a) return NULL;
    json_val *o = json_new_obj();
    if (!o) return NULL;

    json_obj_set(o, "descriptor_version", json_new_str(AIOS_ACTION_VERSION));
    json_obj_set(o, "action_id", json_new_str(a->id));
    json_obj_set(o, "created_at", json_new_num(aios_now_ms()));
    json_obj_set(o, "created_by",
                 json_new_str(a->origin == ORIGIN_HUMAN ? "human" : "ai"));
    json_obj_set(o, "origin", json_new_str(aios_action_origin_name(a->origin)));
    json_obj_set(o, "action_type", json_new_str(aios_action_kind_name(a->kind)));
    json_obj_set(o, "kind", json_new_num((double)a->kind));
    json_obj_set(o, "target", json_new_str(a->target));

    /* args: embed a copy, never steal the live one */
    if (a->args) {
        char *aj = json_dump(a->args);
        if (aj) {
            json_val *parsed = json_parse(aj, strlen(aj));
            json_free(parsed);                    /* only to prove it parses */
            /* embed as-is through a re-parse of the dump (json has no
             * "adopt string" constructor, and args is small) */
            json_val *obj = json_parse(aj, strlen(aj));
            if (obj) json_obj_set(o, "args", obj);
            else     json_obj_set(o, "args", json_new_null());
            free(aj);
        }
    } else {
        json_obj_set(o, "args", json_new_null());
    }

    json_val *eff = json_new_obj();
    json_obj_set(eff, "complete", json_new_bool(a->effects_complete));
    json_val *arr = json_new_arr();
    for (int i = 0; i < a->n_declared; i++)
        json_arr_push(arr, json_new_str(a->declared[i]));
    json_obj_set(eff, "declared", arr);
    json_obj_set(o, "effects", eff);

    json_val *res = json_new_obj();
    json_obj_set(res, "max_duration_ms", json_new_num(a->budget_ms));
    json_obj_set(o, "resources", res);

    json_val *rb = json_new_obj();
    json_obj_set(rb, "supported", json_new_bool(a->reversible));
    json_obj_set(o, "rollback", rb);

    /* A 64-bit FNV hash does NOT survive a JSON number: this parser (like
     * every JSON implementation) holds numbers as double, so only 53 bits
     * are exact. Serializing 12499884494978536736 as a number reads back as
     * ...537472 - a different hash, which would silently break audit
     * correlation in M6. String, hex, exact. */
    {
        char hx[24];
        snprintf(hx, sizeof hx, "%016llx", (unsigned long long)a->intent_hash);
        json_obj_set(o, "intent_hash", json_new_str(hx));
    }
    json_obj_set(o, "expires_at", json_new_num(a->expires_ms));
    json_obj_set(o, "reversible", json_new_bool(a->reversible));

    json_val *audit = json_new_obj();
    json_obj_set(audit, "log", json_new_bool(1));
    json_obj_set(audit, "log_level", json_new_str("SUMMARY"));
    json_obj_set(o, "audit", audit);

    char *out = json_dump(o);
    json_free(o);
    return out;
}

aios_action *aios_action_from_json(const char *json, char *err, size_t ecap) {
    if (err && ecap) err[0] = 0;
    if (!json) { if (err && ecap) snprintf(err, ecap, "no json"); return NULL; }

    json_val *root = json_parse(json, strlen(json));
    if (!root) { if (err && ecap) snprintf(err, ecap, "invalid json"); return NULL; }
    if (json_type_of(root) != JSON_OBJ) {
        if (err && ecap) snprintf(err, ecap, "descriptor must be an object");
        json_free(root);
        return NULL;
    }

    const char *ver = json_str(json_obj_get(root, "descriptor_version"));
    if (!ver || strcmp(ver, AIOS_ACTION_VERSION) != 0) {
        if (err && ecap)
            snprintf(err, ecap, "unsupported descriptor_version '%s'",
                     ver ? ver : "(missing)");
        json_free(root);
        return NULL;
    }

    const char *target = json_str(json_obj_get(root, "target"));
    if (!target || !*target) {
        if (err && ecap) snprintf(err, ecap, "missing target");
        json_free(root);
        return NULL;
    }

    int kind = (int)json_num(json_obj_get(root, "kind"));
    if (kind < 0 || kind >= ACT__COUNT) {
        if (err && ecap) snprintf(err, ecap, "unknown kind %d", kind);
        json_free(root);
        return NULL;
    }

    aios_action *a = aios_action_new((aios_act_kind)kind,
                                     ORIGIN_UNSPECIFIED, target);
    if (!a) {
        if (err && ecap) snprintf(err, ecap, "cannot construct descriptor");
        json_free(root);
        return NULL;
    }

    /* action_id: keep it if it looks like ours, otherwise mint a new one. */
    const char *id = json_str(json_obj_get(root, "action_id"));
    if (id && *id && strlen(id) < sizeof a->id)
        snprintf(a->id, sizeof a->id, "%s", id);

    /* args: deep copy by re-parsing its dump (json_val has no clone). */
    json_val *args = json_obj_get(root, "args");
    if (args && json_type_of(args) == JSON_OBJ) {
        char *aj = json_dump(args);
        if (aj) {
            a->args = json_parse(aj, strlen(aj));
            free(aj);
        }
    }

    /* effects.declared[] — each one is validated on the way in. */
    json_val *eff = json_obj_get(root, "effects");
    if (eff
        && json_type_of(eff) == JSON_OBJ) {
        json_val *decl = json_obj_get(eff, "declared");
        if (decl && json_type_of(decl) == JSON_ARR) {
            size_t n = json_arr_len(decl);
            if (n > AIOS_ACTION_MAX_EFFECTS) {
                if (err && ecap)
                    snprintf(err, ecap,
                             "declares %zu effects (max %d)", n,
                             AIOS_ACTION_MAX_EFFECTS);
                json_free(root);
                aios_action_free(a);
                return NULL;
            }
            for (size_t i = 0; i < n; i++) {
                const char *e = json_str(json_arr_get(decl, i));
                if (!e) continue;              /* skip junk; is_valid() re-checks */
                if (aios_action_add_effect(a, e) != AIOS_OK) {
                    if (err && ecap)
                        snprintf(err, ecap, "effect '%s' rejected", e);
                    json_free(root);
                    aios_action_free(a);
                    return NULL;
                }
            }
        }
        a->effects_complete = json_bool(json_obj_get(eff, "complete"));
    }

    /* resources.max_duration_ms — a claim, so it is bounded below only; the
     * validator rejects anything over the cap. */
    json_val *res = json_obj_get(root, "resources");
    if (res && json_type_of(res) == JSON_OBJ) {
        json_val *d = json_obj_get(res, "max_duration_ms");
        if (d && json_type_of(d) == JSON_NUM && json_num(d) > 0)
            a->budget_ms = json_num(d);
    }

    /* rollback.supported — a CLAIM by the author, so accepted, but note that
     * M3 will only ever trust its own staging, never this flag. */
    json_val *rb = json_obj_get(root, "rollback");
    if (rb && json_type_of(rb) == JSON_OBJ)
        a->reversible = json_bool(json_obj_get(rb, "supported"));

    /* intent_hash: accepted (a hash cannot grant anything on its own).
     * Exact form is a 16-digit hex string; a bare number is tolerated for
     * older records but is documented as lossy above 2^53. */
    json_val *ih = json_obj_get(root, "intent_hash");
    if (ih) {
        if (json_type_of(ih) == JSON_STR) {
            const char *hx = json_str(ih);
            char *end = NULL;
            unsigned long long v = strtoull(hx ? hx : "", &end, 16);
            if (hx && end && *end == 0 && *hx) a->intent_hash = (uint64_t)v;
        } else if (json_type_of(ih) == JSON_NUM) {
            a->intent_hash = (uint64_t)json_num(ih);
        }
    }

    /* ---- TRUST FIELDS, deliberately NOT honoured ----
     * "origin"/"created_by" are read and discarded: a model that could set
     * origin would make the audit log lie. Expiry is never EXTENDED by the
     * JSON: we take whatever aios_action_new stamped and only ever shorten
     * it. Raising a ceiling on the basis of untrusted input is how systems
     * get owned. */
    json_val *exp = json_obj_get(root, "expires_at");
    if (exp && json_type_of(exp) == JSON_NUM) {
        double want = json_num(exp);
        double ceiling = aios_now_ms() + AIOS_ACTION_MAX_LIFETIME_MS;
        if (want < a->expires_ms && want > 0) a->expires_ms = want;  /* shorten */
        if (a->expires_ms > ceiling) a->expires_ms = ceiling;        /* clamp  */
    }

    json_free(root);
    return a;
}
