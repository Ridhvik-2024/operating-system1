/* shroute.c — M5/M7: aiosh intent router.
 *
 * The shell resolves builtins and registered tools syntactically. Only
 * when a line matches neither does it reach the router: the OS decides
 * whether the user meant ONE tool call, multi-step agent work, or a
 * plain conversational reply — and (M7) in WHICH scheduling class the
 * work should run: background/idle-priority requests spawn a background
 * job in the agents cgroup, urgent wording stays interactive.
 *
 *   scripted — deterministic heuristics; CI/testable, no model needed.
 *   micro    — scripted + the M7 micro-LM classifier deciding the class
 *              when no keyword is present (still fully deterministic).
 *   llm      — one z-ai bridge call with a strict-JSON decision contract
 *              and a corrective retry, same discipline as the M4 cortex.
 */
#include "shroute.h"
#include "llmbridge.h"
#include "aios/json.h"
#include "aios/microlm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sh_decision_free(sh_decision *d) {
    if (!d) return;
    free(d->tool);
    free(d->goal);
    free(d->text);
    free(d->thought);
    json_free(d->args);
    if (d->action) aios_action_free(d->action);   /* M1: never leak a proposal */
    memset(d, 0, sizeof *d);
}

aios_action *sh_decision_to_action(sh_decision *d) {
    if (!d) return NULL;
    aios_action *a = d->action;
    d->action = NULL;
    return a;
}

/* ---------------- shared helpers ---------------- */

static char *trim_dup(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\n' || s[n-1] == '\r')) n--;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* word-boundary prefix test: "write " matches "write me" not "writer" */
static bool starts_word(const char *s, const char *word) {
    size_t n = strlen(word);
    return strncmp(s, word, n) == 0 && !isalnum((unsigned char)s[n]) &&
           s[n] != '_';
}

/* word-boundary contains: "later" matches "later, tidy" and "run it
 * later" but never "calculate" — M7 scheduling-class cues. */
static bool contains_word(const char *s, const char *word) {
    size_t n = strlen(word);
    for (const char *p = s; (p = strstr(p, word)) != NULL; p++) {
        bool a_ok = p == s || !isalnum((unsigned char)p[-1]);
        bool b_ok = !isalnum((unsigned char)p[n]) && p[n] != '_';
        if (a_ok && b_ok) return true;
    }
    return false;
}

/* ---------------- M7: scheduling class from language ---------------- */

/* deterministic keyword cues (checked in order: urgent wins ties so a
 * panicky line is never demoted to a background job) */
static const char *const bg_cues[] = {
    "in background", "background", "idle priority", "at idle",
    "no rush", "whenever", "overnight", "later", "when quiet",
    "low priority", "tonight", "sometime", "no hurry", "slowly",
};
static const char *const urgent_cues[] = {
    "asap", "immediately", "urgent", "right now", "frozen", "stuck",
    "emergency", "hurry", "on fire", "at once", "cannot wait",
};

/* Returns 1 = background, 2 = urgent, 0 = normal. `use_lm` enables the
 * micro-LM classifier as the fallback when no keyword hits. */
static int sched_class_of(const char *line, int use_lm, double *lm_conf) {
    for (size_t i = 0; i < sizeof urgent_cues / sizeof urgent_cues[0]; i++)
        if (contains_word(line, urgent_cues[i])) return 2;
    for (size_t i = 0; i < sizeof bg_cues / sizeof bg_cues[0]; i++)
        if (contains_word(line, bg_cues[i])) return 1;
    if (use_lm) {
        double conf = 0.0;
        aios_lm_class c = aios_microlm_classify(line, &conf);
        if (lm_conf) *lm_conf = conf;
        if (c == AIOS_LM_BG) return 1;
        if (c == AIOS_LM_URGENT) return 2;
    }
    return 0;
}

/* ---------------- scripted (deterministic) router ---------------- */

/* "12*(7+5)", "2^10", "-3.5/2" -> true; words -> false. */
static bool is_arith(const char *s) {
    bool digit = false, op = false;
    for (const char *p = s; *p; p++) {
        if (isdigit((unsigned char)*p) || *p == '.') { digit = true; continue; }
        if (strchr("+-*/^%", *p)) { op = true; continue; }
        if (*p == '(' || *p == ')' || *p == ' ' || *p == '\t') continue;
        return false;
    }
    return digit && op;
}

static aios_status scripted_route(const char *line, sh_decision *out,
                                  char *err, size_t ecap, int use_lm) {
    memset(out, 0, sizeof *out);
    char *t = trim_dup(line);
    if (!t) return AIOS_ERR_NOMEM;

    /* 1. pure arithmetic -> calc */
    if (is_arith(t)) {
        out->kind = SH_ROUTE_TOOL;
        out->tool = strdup("calc");
        out->args = json_new_obj();
        json_obj_set(out->args, "expr", json_new_str(t));
        out->thought = strdup("arithmetic expression: route to calc");
        free(t);
        return AIOS_OK;
    }

    /* 2. "read <path>" / "cat <path>" -> fs.read */
    if (starts_word(t, "read") || starts_word(t, "cat")) {
        const char *p = t;
        while (*p && !isspace((unsigned char)*p)) p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p) {
            char *path = trim_dup(p);
            bool one_word = path;
            for (const char *q = path; one_word && q && *q; q++)
                if (isspace((unsigned char)*q)) one_word = false;
            if (one_word) {
                out->kind = SH_ROUTE_TOOL;
                out->tool = strdup("fs.read");
                out->args = json_new_obj();
                json_obj_set(out->args, "path", json_new_str(path));
                out->thought = strdup("single path after read/cat: fs.read");
                free(path);
                free(t);
                return AIOS_OK;
            }
            free(path);
        }
    }

    /* 3. "ls" / "list files" / "list in <dir>" -> fs.list */
    if (strcmp(t, "ls") == 0 || strcmp(t, "list") == 0 ||
        strcmp(t, "list files") == 0 || starts_with(t, "list in ")) {
        out->kind = SH_ROUTE_TOOL;
        out->tool = strdup("fs.list");
        out->args = json_new_obj();
        const char *dir = ".";
        if (starts_with(t, "list in ")) dir = trim_dup(t + 8);
        json_obj_set(out->args, "path", json_new_str(dir));
        out->thought = strdup("listing request: fs.list");
        free(t);
        return AIOS_OK;
    }

    /* 4. "write <text> to <path>" -> fs.write (path is the last word) */
    if (starts_word(t, "write")) {
        const char *last_sp = NULL;
        for (const char *p = t + 5; *p; p++)
            if (*p == ' ' || *p == '\t') last_sp = p;
        if (last_sp) {
            char *path = trim_dup(last_sp + 1);
            bool one_word = true;
            for (const char *q = path; q && *q; q++)
                if (isspace((unsigned char)*q)) one_word = false;
            /* The last space must leave ROOM for content: "write one" has its
             * only space at index 5, which is before t+6, so `last_sp - (t+6)`
             * underflows to SIZE_MAX and the memcpy that follows writes a 10-byte
             * string into a 0-byte allocation. That is not a routing mistake, it
             * is a heap overflow reachable by typing two words.
             *
             * Guarding here means the phrase simply does not match this route and
             * falls through to the text reply, which is the honest answer: there
             * is no content/path split to be found in "write one". */
            if (one_word && path[0] && last_sp > t + 6) {
                size_t clen = (size_t)(last_sp - (t + 6));
                char *content = (char *)malloc(clen + 1);
                if (content) {
                    memcpy(content, t + 6, clen);
                    content[clen] = 0;
                    out->kind = SH_ROUTE_TOOL;
                    out->tool = strdup("fs.write");
                    out->args = json_new_obj();
                    json_obj_set(out->args, "path", json_new_str(path));
                    json_obj_set(out->args, "content", json_new_str(content));
                    out->thought = strdup("write text to a file: fs.write");
                    free(content);
                    free(path);
                    free(t);
                    return AIOS_OK;
                }
            }
            free(path);
        }
    }

    /* 4.5 M7: scheduling class of the work — background wording (or the
     * micro-LM's bg verdict) spawns a background job in the agents
     * class; urgent wording stays on the interactive path. */
    {
        double lm_conf = 0.0;
        int cls = sched_class_of(t, use_lm, &lm_conf);
        if (cls == 1) {
            out->kind = SH_ROUTE_AGENT;
            out->goal = t; /* transfer ownership */
            out->bg = 1;
            out->thought = strdup(use_lm
                ? "scheduling class bg (keyword/micro-lm): spawn a "
                  "background job in the agents class"
                : "scheduling class bg (keyword): spawn a background job "
                  "in the agents class");
            return AIOS_OK;
        }
        if (cls == 2) {
            out->kind = SH_ROUTE_AGENT;
            out->goal = t;
            out->urgent = 1;
            out->thought = strdup("urgent wording: keep interactive "
                                  "(foreground agent, interactive class)");
            return AIOS_OK;
        }
    }

    /* 5. action verbs -> the agent runtime owns multi-step work */
    static const char *const verbs[] = {
        "summarize", "summarise", "analyze", "analyse", "organize",
        "organise", "plan", "build", "make", "create", "find",
        "research", "draft", "generate", "clean", "audit"
    };
    for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (starts_word(t, verbs[i])) {
            out->kind = SH_ROUTE_AGENT;
            out->goal = strdup(t);
            out->thought = strdup("action verb: hand to agent runtime");
            free(t);
            return AIOS_OK;
        }
    }

    /* 6. questions -> direct reply (scripted mode has no answer model) */
    static const char *const qwords[] = {
        "what", "why", "how", "who", "when", "where", "which",
        "explain", "describe", "tell"
    };
    for (size_t i = 0; i < sizeof qwords / sizeof qwords[0]; i++) {
        if (starts_word(t, qwords[i])) {
            out->kind = SH_ROUTE_REPLY;
            out->text = strdup(
                "(scripted cortex) conversational answers need the llm "
                "cortex: run 'cortex llm' and ask again, or rephrase as "
                "'agent <goal>' for tool-backed work.");
            out->thought = strdup("question: reply directly");
            free(t);
            return AIOS_OK;
        }
    }

    /* 7. default: everything else is agent work */
    out->kind = SH_ROUTE_AGENT;
    out->goal = strdup(t);
    out->thought = strdup("no syntactic match: treat as agent goal");
    free(t);
    (void)err; (void)ecap;
    return AIOS_OK;
}
/* ---------------- llm router ---------------- */

#define ROUTE_SYS_CAP 2048

static void build_route_system(char *buf, size_t cap, tool_reg *tools) {
    /* compact catalogue: tool names only — enough to pick one by name */
    char cat[1024] = {0};
    size_t off = 0;
    char *help = tool_reg_help_json(tools);
    if (help) {
        json_val *arr = json_parse(help, strlen(help));
        if (arr) {
            for (size_t i = 0; i < json_arr_len(arr); i++) {
                json_val *o = json_arr_get(arr, i);
                const char *nm = json_str(json_obj_get(o, "name"));
                if (!nm) continue;
                int w = snprintf(cat + off, sizeof cat - off, "%s%s",
                                 off ? ", " : "", nm);
                if (w < 0 || (size_t)w >= sizeof cat - off) break;
                off += (size_t)w;
            }
        }
        json_free(arr);
        free(help);
    }
    snprintf(buf, cap,
        "You are the intent router of aiosh, the shell of an AI-native "
        "operating system. The user typed a line the shell could not "
        "resolve as a builtin or a tool invocation. Decide ONE action and "
        "respond with ONLY one JSON object, no markdown:\n"
        "  {\"kind\":\"tool\", \"tool\":\"<name>\", \"args\":{...}} — run "
        "exactly one tool now\n"
        "  {\"kind\":\"agent\",\"goal\":\"<imperative goal>\"} — multi-step "
        "work for the autonomous agent runtime\n"
        "  {\"kind\":\"reply\",\"text\":\"<direct answer>\"} — plain "
        "conversational answer, no tools\n"
        "Available tools: %s.\n"
        "Rules: pure arithmetic -> calc; one file read/list/write that is "
        "fully specified -> that tool; anything needing exploration, "
        "several tools, or judgment -> agent; greetings, meta-questions "
        "about the shell, or opinions -> reply. Rewrite vague requests as "
        "a clear imperative goal for the agent.",
        cat[0] ? cat : "(none)");
}

static bool valid_decision(json_val *o, sh_decision *out, tool_reg *tools,
                           char *err, size_t ecap) {
    memset(out, 0, sizeof *out);
    const char *kind = json_str(json_obj_get(o, "kind"));
    if (!kind) { snprintf(err, ecap, "decision missing \"kind\""); return false; }
    if (strcmp(kind, "tool") == 0) {
        const char *tool = json_str(json_obj_get(o, "tool"));
        if (!tool || !*tool) {
            snprintf(err, ecap, "tool decision missing \"tool\"");
            return false;
        }
        if (!tool_reg_find(tools, tool)) {
            snprintf(err, ecap, "router chose unknown tool \"%s\"", tool);
            return false;
        }
        out->kind = SH_ROUTE_TOOL;
        out->tool = strdup(tool);
        json_val *args = json_obj_take(o, "args");
        if (args && json_type_of(args) != JSON_OBJ) { json_free(args); args = NULL; }
        out->args = args ? args : json_new_obj();
        return true;
    }
    if (strcmp(kind, "agent") == 0) {
        const char *goal = json_str(json_obj_get(o, "goal"));
        if (!goal || !*goal) {
            snprintf(err, ecap, "agent decision missing \"goal\"");
            return false;
        }
        out->kind = SH_ROUTE_AGENT;
        out->goal = strdup(goal);
        return true;
    }
    if (strcmp(kind, "reply") == 0) {
        const char *text = json_str(json_obj_get(o, "text"));
        out->kind = SH_ROUTE_REPLY;
        out->text = strdup(text ? text : "(empty reply)");
        return true;
    }
    snprintf(err, ecap, "unknown decision kind \"%s\"", kind);
    return false;
}

static aios_status llm_route(const char *line, tool_reg *tools,
                             sh_decision *out, char *err, size_t ecap) {
    char system[ROUTE_SYS_CAP];
    build_route_system(system, sizeof system, tools);

    char *content = NULL;
    aios_status st = aios_llm_call(system, line, &content, err, ecap);
    if (st != AIOS_OK) return st;

    for (int attempt = 0; attempt < 2; attempt++) {
        char *js = aios_llm_extract_json(content);
        free(content);
        content = NULL;
        json_val *o = js ? json_parse(js, strlen(js)) : NULL;
        free(js);
        if (o && json_type_of(o) == JSON_OBJ) {
            char verr[256] = {0};
            bool okv = valid_decision(o, out, tools, verr, sizeof verr);
            json_free(o);
            if (okv) return AIOS_OK;
            snprintf(err, ecap, "%s", verr);
        } else {
            json_free(o);
            snprintf(err, ecap, "router reply is not a JSON object");
        }
        if (attempt == 0) {
            /* one corrective retry, like the M4 cortex */
            char prompt[4096];
            snprintf(prompt, sizeof prompt,
                     "%s\n\nIMPORTANT: your previous reply was not a valid "
                     "routing decision (%s). Respond with ONLY the JSON "
                     "decision object.", line, err);
            st = aios_llm_call(system, prompt, &content, err, ecap);
            if (st != AIOS_OK) return st;
        }
    }
    return AIOS_ERR_TOOL;
}

/* ---------------- entry ---------------- */

/* Turn a finished decision into the descriptor it implies.
 *
 * Done once, here, rather than in each of the eleven places that fill a
 * sh_decision: the routers know WHICH tool/goal, but the descriptor also
 * needs declared effects per tool, a budget per kind, and the human intent.
 * Spreading that across the routers would guarantee one of them drifts.
 *
 * origin is left UNSPECIFIED on purpose: the caller decides whether this
 * route was human or AI. A routed natural-language line is AI (the router
 * chose), which is why shell.c stamps ORIGIN_AI.
 *
 * Returns NULL only on allocation failure or for SH_ROUTE_REPLY, which
 * proposes no action at all (the shell answers from its own knowledge and
 * touches nothing). */
static aios_action *decision_to_descriptor(sh_decision *d, const char *line) {
    if (!d || d->kind == SH_ROUTE_REPLY) return NULL;

    aios_act_kind kind = ACT_TOOL;
    const char *target = NULL;

    switch (d->kind) {
    case SH_ROUTE_TOOL:
        target = d->tool;
        kind = ACT_TOOL;
        break;
    case SH_ROUTE_AGENT:
        target = d->goal;
        kind = d->bg ? ACT_JOB_SPAWN : ACT_AGENT;
        break;
    default:
        return NULL;
    }
    if (!target || !*target) return NULL;

    aios_action *a = aios_action_new(kind, ORIGIN_UNSPECIFIED, target);
    if (!a) return NULL;

    /* The human intent this serves is the line the human actually typed. */
    aios_action_set_intent(a, line);

    if (d->kind == SH_ROUTE_TOOL) {
        /* args transfer to the descriptor: the router's copy becomes NULL so
         * sh_decision_free() stays a no-op for it. */
        if (d->args) aios_action_set_args(a, d->args);
        d->args = NULL;
        aios_action_declare_tool_effects(a, target, a->args);
    }
    return a;
}

aios_status sh_route(const char *mode, const char *line, tool_reg *tools,
                     sh_decision *out, char *err, size_t ecap) {
    if (err && ecap) err[0] = 0;
    if (!mode || !line || !out) return AIOS_ERR_ARG;

    aios_status st = (strcmp(mode, "llm") == 0 && tools)
        ? llm_route(line, tools, out, err, ecap)
        : scripted_route(line, out, err, ecap, strcmp(mode, "micro") == 0);
    if (st != AIOS_OK) return st;

    out->action = decision_to_descriptor(out, line);
    return AIOS_OK;
}
