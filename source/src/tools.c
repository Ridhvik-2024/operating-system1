/* aios/tools.c — M4: tool registry, native tools, audit journal. */
#include "aios/tools.h"
#include "sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct tool_node {
    tool_def def;
    struct tool_node *next;
} tool_node;

struct tool_reg {
    tool_node *head;
    size_t n;
    char jail[512];
    char store[512];
    char agent[128];
    mem_ctx *mem;      /* borrowed */
    /* M3: when set, sandboxed writes are redirected here and the worker is
     * told the action is AI-originated. */
    char stage[512];
    int  staged;
};

/* Native tools need registry state (memory ctx, agent name). tools.c
 * supports one active registry at a time for native dispatch — set
 * around each native call, so this is re-entrant across registries in
 * single-threaded agent loops (the M4 contract). */
static tool_reg *g_active_reg = NULL;

/* ---------------- native tools ---------------- */

static aios_status native_memory_search(json_val *args, json_val **out,
                                        char *err, size_t ecap) {
    if (!g_active_reg || !g_active_reg->mem) {
        snprintf(err, ecap, "memory subsystem not attached");
        return AIOS_ERR_STATE;
    }
    const char *query = json_str(json_obj_get(args, "query"));
    if (!query || !*query) {
        snprintf(err, ecap, "missing \"query\"");
        return AIOS_ERR_ARG;
    }
    int k = (int)json_num(json_obj_get(args, "k"));
    if (k <= 0 || k > 10) k = 3;

    mem_entry *hits[10];
    int n = mem_recall(g_active_reg->mem, query, k, hits);
    json_val *res = json_new_obj();
    json_val *arr = json_new_arr();
    for (int i = 0; i < n; i++) {
        json_val *h = json_new_obj();
        json_obj_set(h, "text",  json_new_str(hits[i]->text));
        json_obj_set(h, "tier",  json_new_num((double)hits[i]->tier));
        json_obj_set(h, "score", json_new_num(hits[i]->score));
        json_arr_push(arr, h);
    }
    json_obj_set(res, "results", arr);
    json_obj_set(res, "count", json_new_num((double)n));
    *out = res;
    return AIOS_OK;
}

static aios_status native_proc_info(json_val *args, json_val **out,
                                    char *err, size_t ecap) {
    (void)args; (void)err; (void)ecap;
    json_val *res = json_new_obj();
    json_obj_set(res, "agent",   json_new_str(g_active_reg ? g_active_reg->agent : "?"));
    json_obj_set(res, "pid",     json_new_num((double)getpid()));
    json_obj_set(res, "now_ms",  json_new_num(aios_now_ms()));
    json_obj_set(res, "version", json_new_str(AIOS_VERSION));
    *out = res;
    return AIOS_OK;
}

/* ---------------- registry ---------------- */

tool_reg *tool_reg_create(const char *jail_dir, const char *store_dir,
                          const char *agent_name, mem_ctx *mem) {
    if (!jail_dir || !store_dir || !agent_name) return NULL;
    tool_reg *tr = (tool_reg *)calloc(1, sizeof(tool_reg));
    if (!tr) return NULL;
    snprintf(tr->jail,  sizeof tr->jail,  "%s", jail_dir);
    snprintf(tr->store, sizeof tr->store, "%s", store_dir);
    snprintf(tr->agent, sizeof tr->agent, "%s", agent_name);
    tr->mem = mem;
    return tr;
}

void tool_reg_free(tool_reg *tr) {
    if (!tr) return;
    if (g_active_reg == tr) g_active_reg = NULL;
    tool_node *t = tr->head;
    while (t) {
        tool_node *nx = t->next;
        free(t);
        t = nx;
    }
    free(tr);
}

aios_status tool_reg_add(tool_reg *tr, const tool_def *def) {
    if (!tr || !def || !def->name || !def->desc) return AIOS_ERR_ARG;
    if (tool_reg_find(tr, def->name)) return AIOS_ERR_STATE; /* duplicate */
    tool_node *n = (tool_node *)calloc(1, sizeof(tool_node));
    if (!n) return AIOS_ERR_NOMEM;
    n->def = *def;
    n->next = tr->head;
    tr->head = n;
    tr->n++;
    return AIOS_OK;
}

size_t tool_reg_count(const tool_reg *tr) { return tr ? tr->n : 0; }

const tool_def *tool_reg_find(tool_reg *tr, const char *name) {
    if (!tr || !name) return NULL;
    for (tool_node *t = tr->head; t; t = t->next)
        if (strcmp(t->def.name, name) == 0) return &t->def;
    return NULL;
}

char *tool_reg_help_json(tool_reg *tr) {
    json_val *arr = json_new_arr();
    if (!arr) return NULL;
    for (tool_node *t = tr ? tr->head : NULL; t; t = t->next) {
        json_val *o = json_new_obj();
        json_obj_set(o, "name", json_new_str(t->def.name));
        json_obj_set(o, "desc", json_new_str(t->def.desc));
        json_obj_set(o, "args", json_new_str(t->def.args_help ? t->def.args_help : ""));
        json_obj_set(o, "sandboxed", json_new_bool(t->def.sandboxed));
        json_arr_push(arr, o);
    }
    char *s = json_dump(arr);
    json_free(arr);
    return s;
}

/* ---------------- audit ---------------- */

static void audit_log(tool_reg *tr, const char *tool, const char *args_json,
                      bool ok, double ms, const char *result_json) {
    if (!tr || !tr->store[0]) return;
    char path[600];
    snprintf(path, sizeof path, "%s/audit.log", tr->store);
    FILE *f = fopen(path, "ab");
    if (!f) return;
    uint64_t dig = aios_fnv1a(result_json ? result_json : "",
                              result_json ? strlen(result_json) : 0);
    /* embed args as JSON value if it parses, else as a JSON string */
    json_val *a = json_parse(args_json ? args_json : "null",
                             args_json ? strlen(args_json) : 4);
    char *args_embedded;
    if (a) {
        args_embedded = strdup(args_json ? args_json : "null");
        json_free(a);
    } else {
        json_val *s = json_new_str(args_json ? args_json : "");
        args_embedded = json_dump(s);
        json_free(s);
    }
    fprintf(f,
        "{\"ts\":%.0f,\"agent\":\"%s\",\"tool\":\"%s\",\"args\":%s,"
        "\"ok\":%s,\"ms\":%.1f,\"digest\":\"%016llx\"}\n",
        aios_now_ms(), tr->agent, tool,
        args_embedded ? args_embedded : "null",
        ok ? "true" : "false", ms,
        (unsigned long long)dig);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { /* best effort */ }
    fclose(f);
    free(args_embedded);
}

/* ---------------- execution ---------------- */

static double g_sandbox_timeout_s = 10.0;

void tool_reg_set_staging(tool_reg *tr, const char *stage_dir) {
    if (!tr) return;
    if (stage_dir && *stage_dir) {
        snprintf(tr->stage, sizeof tr->stage, "%s", stage_dir);
        tr->staged = 1;
    } else {
        tr->stage[0] = 0;
        tr->staged = 0;
    }
}

void tool_set_sandbox_timeout(double seconds) {
    if (seconds < 0.5) seconds = 0.5;
    g_sandbox_timeout_s = seconds;
}

/* args is always consumed (freed) on every return path. */
aios_status tool_exec(tool_reg *tr, const char *name, json_val *args,
                      json_val **result_out, char *err, size_t ecap) {
    if (err && ecap) err[0] = 0;
    if (!tr || !name || !result_out) {
        json_free(args);
        if (err && ecap) snprintf(err, ecap, "invalid tool_exec call");
        return AIOS_ERR_ARG;
    }
    const tool_def *def = tool_reg_find(tr, name);
    if (!def) {
        json_free(args);
        if (err && ecap) snprintf(err, ecap, "unknown tool \"%s\"", name);
        return AIOS_ERR_NOTFOUND;
    }

    char *args_json = json_dump(args ? args : json_new_null());
    double t0 = aios_mono_ms();
    aios_status st;
    json_val *res = NULL;

    if (def->sandboxed) {
        json_val *req = json_new_obj();
        json_obj_set(req, "tool", json_new_str(name));
        json_obj_set(req, "args", args ? args : json_new_null());
        /* M3: origin and staging travel with the request. The worker needs the
         * origin to enforce "an AI write must not land", and it must receive
         * that from its parent rather than assume it. */
        json_obj_set(req, "origin", json_new_str(tr->staged ? "ai" : "human"));
        if (tr->staged) json_obj_set(req, "stage", json_new_str(tr->stage));
        char *req_json = json_dump(req);
        json_free(req);              /* frees args too: ownership moved */
        args = NULL;
        st = sandbox_run(tr->jail,
                         req_json ? req_json : "{}",
                         req_json ? strlen(req_json) : 2,
                         g_sandbox_timeout_s, &res, err, ecap);
        free(req_json);
        if (st == AIOS_OK && res) {
            /* unwrap worker envelope {"ok":true,"result":X} -> X (or null) */
            json_val *r = json_obj_take(res, "result");
            json_free(res);
            res = r ? r : json_new_null();
        }
    } else {
        g_active_reg = tr;
        st = def->native ? def->native(args, &res, err, ecap)
                         : AIOS_ERR_STATE;
        g_active_reg = NULL;
        json_free(args);
        args = NULL;
    }

    double ms = aios_mono_ms() - t0;
    char *res_json = (st == AIOS_OK && res) ? json_dump(res) : NULL;
    audit_log(tr, name, args_json ? args_json : "null",
              st == AIOS_OK, ms, res_json);
    free(res_json);
    free(args_json);

    if (st != AIOS_OK) {
        json_free(res);
        if (err && ecap && err[0] == 0)
            snprintf(err, ecap, "%s", aios_status_str(st));
        return st;
    }
    *result_out = res;
    return AIOS_OK;
}

/* ---------------- default M4 toolset ---------------- */

void tool_reg_install_default(tool_reg *tr) {
    if (!tr) return;
    tool_reg_add(tr, &(tool_def){
        .name = "calc",
        .desc = "Evaluate an arithmetic expression (+ - * / ^ parentheses).",
        .args_help = "{\"expr\":\"12*(7+5)\"}",
        .sandboxed = true,
        .native = NULL
    });
    tool_reg_add(tr, &(tool_def){
        .name = "fs.read",
        .desc = "Read a text file inside the agent workspace jail.",
        .args_help = "{\"path\":\"notes.txt\"}",
        .sandboxed = true,
        .native = NULL
    });
    tool_reg_add(tr, &(tool_def){
        .name = "fs.write",
        .desc = "Write a text file inside the agent workspace jail.",
        .args_help = "{\"path\":\"out.md\",\"content\":\"text\"}",
        .sandboxed = true,
        .native = NULL
    });
    tool_reg_add(tr, &(tool_def){
        .name = "fs.list",
        .desc = "List files in a directory of the agent workspace jail.",
        .args_help = "{\"path\":\".\"}",
        .sandboxed = true,
        .native = NULL
    });
    tool_reg_add(tr, &(tool_def){
        .name = "selftest.sleep",
        .desc = "internal: sleep ms (sandbox timeout tests)",
        .args_help = "{\"ms\":2000}",
        .sandboxed = true,
        .native = NULL
    });
    tool_reg_add(tr, &(tool_def){
        .name = "selftest.spin",
        .desc = "internal: busy loop forever (sandbox kill tests)",
        .args_help = "{}",
        .sandboxed = true,
        .native = NULL
    });
    tool_reg_add(tr, &(tool_def){
        .name = "memory.search",
        .desc = "Search OS-owned agent memory by meaning; returns top hits.",
        .args_help = "{\"query\":\"token estimate\",\"k\":3}",
        .sandboxed = false,
        .native = native_memory_search
    });
    tool_reg_add(tr, &(tool_def){
        .name = "proc.info",
        .desc = "Report AI-OS process info (agent, pid, version).",
        .args_help = "{}",
        .sandboxed = false,
        .native = native_proc_info
    });
}
