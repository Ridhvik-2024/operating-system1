/* aios/effects.c — M4: parse, compare and explain worker effect documents.
 *
 * This file is deliberately pure: no I/O, no clock, no allocation. The thing it
 * implements is a trust decision, and a trust decision that depends on the
 * environment cannot be reasoned about or tested exhaustively.
 */
#include "aios/effects.h"
#include "aios/types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

const char *aios_effects_verdict_str(aios_effects_verdict v) {
    switch (v) {
    case EFFECTS_OK:              return "effects match the declaration";
    case EFFECTS_UNDECLARED:      return "undeclared effect";
    case EFFECTS_INCOMPLETE:      return "the worker cannot account for its own effects";
    case EFFECTS_MISSING:         return "no effects document";
    case EFFECTS_MALFORMED:       return "malformed effect";
    case EFFECTS_UNDER_DELIVERED: return "declared work did not happen";
    }
    return "?";
}

/* A verb we are willing to compare on. Anything else is malformed: an unknown
 * verb in an effect document is not a thing to ignore, it is a worker telling
 * us it is confused about what it did. */
static bool verb_ok(const char *v) {
    return v && (strcmp(v, "write") == 0 || strcmp(v, "read") == 0 ||
                 strcmp(v, "delete") == 0 || strcmp(v, "list") == 0);
}

/* The descriptor spells effects "write:a.txt" and "read:a.txt"; the worker
 * spells them {"op":"write","path":"a.txt"}. Compare path-only, and treat a
 * declaration as covering the verbs it names. `fs.list` is a read. */
static bool declared_covers(const char *decl, const char *op, const char *path) {
    if (!decl || !op || !path) return false;
    const char *colon = strchr(decl, ':');
    if (!colon) return false;
    size_t vlen = (size_t)(colon - decl);
    const char *dpath = colon + 1;

    bool verb_match = false;
    if (vlen == 5 && strncmp(decl, "write", 5) == 0)
        verb_match = (strcmp(op, "write") == 0);
    else if (vlen == 4 && strncmp(decl, "read", 4) == 0)
        verb_match = (strcmp(op, "read") == 0 || strcmp(op, "list") == 0);
    else if (vlen == 6 && strncmp(decl, "delete", 6) == 0)
        verb_match = (strcmp(op, "delete") == 0);
    else
        return false;

    return verb_match && strcmp(dpath, path) == 0;
}

/* The mirror of aios_effects_compare: given the paths that were supposed to be
 * written into staging, which of them actually appeared there and which are
 * still sitting in the jail?
 *
 * This is the check that does not care what the worker says. A worker can forge
 * its effect document; it cannot forge the tree it left behind. When a path is
 * found in the jail instead of the staging dir, the worker ignored the staging
 * directory -- which means every claim it made about this run is worthless.
 *
 * Pure: the caller does the stat() calls and passes the answers in. */
bool aios_effects_mirror(const char *const *declared, int n_declared,
                         const char *const *in_jail, const char *const *in_upper,
                         int n_paths, char *first, size_t first_cap) {
    if (first && first_cap) first[0] = 0;
    if (n_declared <= 0 || !declared) return true;

    for (int d = 0; d < n_declared; d++) {
        const char *decl = declared[d];
        if (!decl || strncmp(decl, "write:", 6) != 0) continue;
        const char *path = decl + 6;
        if (!*path) continue;

        bool landed = false, staged = false;
        for (int i = 0; i < n_paths; i++) {
            if (in_jail[i] && strcmp(in_jail[i], path) == 0) landed = true;
            if (in_upper[i] && strcmp(in_upper[i], path) == 0) staged = true;
        }
        if (landed) {
            if (first && first_cap) snprintf(first, first_cap, "%s", path);
            return false;
        }
        (void)staged;   /* a staged file is the expected outcome, not a finding */
    }
    return true;
}

bool aios_effects_to_rel(const char *actual_path, const char *stage_dir,
                         char *out, size_t ocap) {
    if (!actual_path || !out || ocap == 0) return false;
    if (!stage_dir || !*stage_dir) {
        if (strlen(actual_path) >= ocap) return false;
        memcpy(out, actual_path, strlen(actual_path) + 1);
        return true;
    }
    /* <stage>/upper/<rel> -> <rel> */
    char prefix[AIOS_EFFECT_PATH_MAX + 16];
    int pn = snprintf(prefix, sizeof prefix, "%s/upper/", stage_dir);
    if (pn < 0 || (size_t)pn >= sizeof prefix) return false;
    size_t plen = (size_t)pn;
    if (strncmp(actual_path, prefix, plen) != 0) return false;
    const char *rel = actual_path + plen;
    if (!*rel || strlen(rel) >= ocap) return false;
    memcpy(out, rel, strlen(rel) + 1);
    return true;
}

static bool parse_array(const json_val *eff, int complete, aios_effects *out,
                        char *err, size_t ecap);

bool aios_effects_from_array(const json_val *effects_arr, int complete,
                             aios_effects *out, char *err, size_t ecap) {
    if (!out) return set_err(err, ecap, "no output"), false;
    memset(out, 0, sizeof *out);
    return parse_array(effects_arr, complete, out, err, ecap);
}

bool aios_effects_from_json(const json_val *resp, aios_effects *out,
                            aios_usage *usage, char *err, size_t ecap) {
    if (!out) return set_err(err, ecap, "no output"), false;
    memset(out, 0, sizeof *out);
    if (usage) memset(usage, 0, sizeof *usage);
    if (!resp) return set_err(err, ecap, "no response"), false;

    if (usage) {
        json_val *u = json_obj_get(resp, "usage");
        if (u && json_type_of(u) == JSON_OBJ) {
            usage->ms            = (long)json_num(json_obj_get(u, "ms"));
            usage->bytes_read    = (long)json_num(json_obj_get(u, "bytes_read"));
            usage->bytes_written = (long)json_num(json_obj_get(u, "bytes_written"));
        }
    }

    const json_val *eff = json_obj_get(resp, "effects");
    const json_val *comp = json_obj_get(resp, "complete");
    /* A document that omits `complete` is NOT assumed complete. The whole point
     * of the flag is that a worker which dropped effects says so; defaulting it
     * to true would make an older or hostile worker's silence look like a
     * clean bill of health. */
    int complete = comp ? json_bool(comp) : 0;
    if (!eff) return true;                       /* absent: legal, means nothing */
    return parse_array(eff, complete, out, err, ecap);
}

/* The invariant that makes the comparison meaningful: an effect list is only
 * usable if every entry is well-formed AND the worker says it did not drop any.
 * A partial list is worse than none, because it looks complete. */
static bool parse_array(const json_val *eff, int complete, aios_effects *out,
                        char *err, size_t ecap) {
    if (!eff) { out->present = false; return true; }
    if (json_type_of(eff) != JSON_ARR)
        return set_err(err, ecap, "effects is not an array"), false;

    out->present = true;
    out->complete = complete ? 1 : 0;
    size_t n = json_arr_len(eff);
    if (n > AIOS_EFFECT_MAX)
        return set_err(err, ecap, "too many effects in one response (%zu)", n), false;

    for (size_t i = 0; i < n; i++) {
        json_val *e = json_arr_get(eff, i);
        if (!e || json_type_of(e) != JSON_OBJ)
            return set_err(err, ecap, "effect %zu is not an object", i), false;
        const char *p = json_str(json_obj_get(e, "path"));
        const char *op = json_str(json_obj_get(e, "op"));
        if (!p || !*p)
            return set_err(err, ecap, "effect %zu has no path", i), false;
        if (!verb_ok(op))
            return set_err(err, ecap, "effect %zu has an unknown op %.16s", i,
                           op ? op : "(none)"), false;
        if (p[0] == '/' || strstr(p, ".."))
            return set_err(err, ecap, "effect %zu names a path outside the jail", i),
                   false;
        if (strlen(p) >= AIOS_EFFECT_PATH_MAX)
            return set_err(err, ecap, "effect %zu path too long", i), false;

        aios_effect *item = &out->items[out->n++];
        memcpy(item->path, p, strlen(p) + 1);
        snprintf(item->op, sizeof item->op, "%s", op);
        item->bytes = (long)json_num(json_obj_get(e, "bytes"));
    }
    return true;
}

aios_effects_verdict aios_effects_compare(aios_effects *actual,
                                          const char *const *declared,
                                          int n_declared,
                                          char *first, size_t first_cap) {
    if (first && first_cap) first[0] = 0;
    if (!actual) return EFFECTS_MISSING;

    /* No document at all. The caller decides whether that is fatal; this
     * function reports it rather than guessing, because "the tool touched
     * nothing" and "the tool did not tell us" are different claims and only one
     * of them is good news. */
    if (!actual->present) return EFFECTS_MISSING;

    if (!actual->complete) return EFFECTS_INCOMPLETE;

    if (n_declared > 0 && !declared)
        return EFFECTS_MALFORMED;

    /* Forward direction: every actual effect must be covered by a declaration. */
    for (size_t i = 0; i < actual->n; i++) {
        aios_effect *e = &actual->items[i];
        e->declared = false;
        for (int d = 0; d < n_declared; d++) {
            if (declared_covers(declared[d], e->op, e->path)) {
                e->declared = true;
                break;
            }
        }
        if (!e->declared) {
            /* A read is not a violation of the write contract, but it is still
             * unaccounted for. Report the first one and stop: the audit entry
             * only needs one name. */
            if (first && first_cap) {
                snprintf(first, first_cap, "%s:%s", e->op, e->path);
            }
            return EFFECTS_UNDECLARED;
        }
    }

    /* Reverse direction: a declared write that did not happen is not a safety
     * failure, but it IS a lie, and the caller should be able to say so. */
    for (int d = 0; d < n_declared; d++) {
        const char *decl = declared[d];
        if (!decl) continue;
        /* Only writes are "work that must have happened" — a read declaration
         * that did not happen just means the file was already known. */
        if (strncmp(decl, "write:", 6) != 0) continue;
        bool found = false;
        for (size_t i = 0; i < actual->n; i++) {
            if (declared_covers(decl, actual->items[i].op, actual->items[i].path)) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (first && first_cap) snprintf(first, first_cap, "%s", decl + 6);
            return EFFECTS_UNDER_DELIVERED;
        }
    }

    return EFFECTS_OK;
}

char *aios_effects_text(const aios_effects *actual, char *first_undeclared) {
    char buf[512];
    if (!actual) {
        snprintf(buf, sizeof buf, "no effects document");
        return strdup(buf);
    }
    size_t writes = 0, reads = 0, dels = 0, other = 0;
    for (size_t i = 0; i < actual->n; i++) {
        const char *op = actual->items[i].op;
        if (strcmp(op, "write") == 0) writes++;
        else if (strcmp(op, "read") == 0 || strcmp(op, "list") == 0) reads++;
        else if (strcmp(op, "delete") == 0) dels++;
        else other++;
    }
    int n = snprintf(buf, sizeof buf,
                     "%zu effect%s (%zu write, %zu read, %zu delete",
                     actual->n, actual->n == 1 ? "" : "s", writes, reads, dels);
    if (other && n > 0 && (size_t)n < sizeof buf)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ", %zu other", other);
    if (n > 0 && (size_t)n < sizeof buf)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ")");
    if (first_undeclared && first_undeclared[0] && n > 0 && (size_t)n < sizeof buf)
        snprintf(buf + n, sizeof buf - (size_t)n, " — undeclared: %.120s",
                 first_undeclared);
    return strdup(buf);
}
