/* aios/agent.c — M4: the observe -> plan -> act -> reflect loop. */
#include "aios/agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBS_TRAIL_CAP 2048  /* local line cap (mem.c owns MAX_META) */

void agent_action_free(agent_action *a) {
    if (!a) return;
    free(a->thought);
    free(a->tool);
    free(a->final);
    free(a->facts_json);
    json_free(a->args);
    memset(a, 0, sizeof *a);
}

void cortex_free(cortex *cx) {
    if (!cx) return;
    if (cx->destroy) cx->destroy(cx);
    else free(cx);
}

/* ---------------- observation ---------------- */

/* Truncate long strings and embed the last tool result as JSON when
 * parseable, so both cortex backends see structured data. */
static json_val *build_observation(const agent_cfg *cfg, int step,
                                   const char *last_tool, bool last_ok,
                                   const char *last_err,
                                   const char *last_result_json,
                                   json_val *history) {
    json_val *o = json_new_obj();
    json_obj_set(o, "step", json_new_num((double)step));
    json_obj_set(o, "max_steps", json_new_num((double)cfg->max_steps));
    json_obj_set(o, "agent", json_new_str(cfg->name));
    json_obj_set(o, "goal", json_new_str(cfg->goal));
    if (cfg->tools) {
        char *help = tool_reg_help_json(cfg->tools);
        json_val *arr = help ? json_parse(help, strlen(help)) : NULL;
        free(help);
        json_obj_set(o, "tools", arr ? arr : json_new_arr());
    }
    if (cfg->mem) {
        mem_entry *hits[3];
        int n = mem_recall(cfg->mem, cfg->goal, 3, hits);
        json_val *arr = json_new_arr();
        for (int i = 0; i < n; i++)
            json_arr_push(arr, json_new_str(hits[i]->text));
        json_obj_set(o, "memories", arr);
    }
    /* what this agent already did THIS session — prevents loops */
    if (history && json_arr_len(history) > 0) {
        char *hj = json_dump(history);
        json_val *hcopy = hj ? json_parse(hj, strlen(hj)) : NULL;
        free(hj);
        json_obj_set(o, "history", hcopy ? hcopy : json_new_arr());
    }
    if (last_tool) {
        json_val *last = json_new_obj();
        json_obj_set(last, "tool", json_new_str(last_tool));
        json_obj_set(last, "ok", json_new_bool(last_ok));
        if (last_err && *last_err) json_obj_set(last, "error", json_new_str(last_err));
        if (last_result_json && *last_result_json) {
            json_val *r = json_parse(last_result_json, strlen(last_result_json));
            if (r) json_obj_set(last, "result", r);
            else {
                char trunc[512];
                snprintf(trunc, sizeof trunc, "%.*s", 500, last_result_json);
                json_obj_set(last, "result_text", json_new_str(trunc));
            }
        }
        json_obj_set(o, "last", last);
    }
    return o;
}

/* ---------------- the loop ---------------- */

aios_status agent_run(const agent_cfg *cfg, char *final_out, size_t cap) {
    if (!cfg || !cfg->name || !cfg->goal || !cfg->cx || !cfg->tools)
        return AIOS_ERR_ARG;
    if (final_out && cap) final_out[0] = 0;
    int max_steps = cfg->max_steps > 0 && cfg->max_steps <= 64 ? cfg->max_steps : 8;

    if (cfg->mem)
        mem_remember(cfg->mem, MEM_WORKING, cfg->goal, NULL);

    char last_tool[64] = {0};
    char last_err[512] = {0};
    char last_result_json[4096] = {0};
    bool last_ok = false;
    char err[512];
    aios_status st = AIOS_OK;
    json_val *history = json_new_arr();
    /* deterministic loop guard: the runtime, not the model, owns the
     * no-duplicate-calls discipline of the OS */
    char *h_tool[64] = {0};
    char *h_args[64] = {0};
    int hn = 0;

    for (int step = 0; step < max_steps; step++) {
        json_val *obs = build_observation(cfg, step,
                                          last_tool[0] ? last_tool : NULL,
                                          last_ok,
                                          last_err[0] ? last_err : NULL,
                                          last_result_json[0] ? last_result_json : NULL,
                                          history);
        char *obs_json = json_dump(obs);
        json_free(obs);
        if (!obs_json) return AIOS_ERR_NOMEM;

        agent_action act;
        memset(&act, 0, sizeof act);
        err[0] = 0;
        st = cfg->cx->plan(cfg->cx, obs_json, &act, err, sizeof err);
        free(obs_json);
        if (st != AIOS_OK) {
            /* err is caller-sized; last_err is 512. Clamp explicitly so a
             * long cortex message cannot be reported as a truncation bug. */
            snprintf(last_err, sizeof last_err, "cortex: %.*s",
                     (int)sizeof last_err - 9,
                     err[0] ? err : aios_status_str(st));
            last_tool[0] = 0;
            last_ok = false;
            if (final_out && cap) snprintf(final_out, cap, "%s", last_err);
            continue; /* burn a step on cortex failure, then retry */
        }

        if (act.done) {
            if (cfg->mem) {
                char line[OBS_TRAIL_CAP];
                snprintf(line, sizeof line,
                         "agent %s completed goal \"%s\": %.260s",
                         cfg->name, cfg->goal,
                         act.final ? act.final : "(no summary)");
                mem_remember(cfg->mem, MEM_EPISODIC, line, NULL);
                /* durable facts -> semantic tier (cross-session knowledge) */
                if (act.facts_json) {
                    json_val *arr = json_parse(act.facts_json,
                                               strlen(act.facts_json));
                    for (size_t i = 0; arr && i < json_arr_len(arr); i++) {
                        const char *f = json_str(json_arr_get(arr, i));
                        if (f && *f) {
                            char meta[256];
                            snprintf(meta, sizeof meta,
                                     "{\"agent\":\"%s\",\"goal\":\"%s\",\"fact\":true}",
                                     cfg->name, cfg->goal);
                            mem_remember(cfg->mem, MEM_SEMANTIC, f, meta);
                        }
                    }
                    json_free(arr);
                }
            }
            if (final_out && cap)
                snprintf(final_out, cap, "%s",
                         act.final ? act.final : "(done)");
            agent_action_free(&act);
            json_free(history);
            for (int i = 0; i < hn; i++) { free(h_tool[i]); free(h_args[i]); }
            return AIOS_OK;
        }

        /* act */
        char *args_json = act.args ? json_dump(act.args) : strdup("null");
        bool duplicate = false;
        int dup_step = -1;
        for (int i = 0; i < hn; i++) {
            if (strcmp(h_tool[i], act.tool) == 0 &&
                strcmp(h_args[i], args_json ? args_json : "") == 0) {
                duplicate = true;
                dup_step = i;
                break;
            }
        }
        json_val *result = NULL;
        err[0] = 0;
        if (duplicate) {
            /* OS rejects the wasted call: same tool, same args */
            snprintf(last_err, sizeof last_err,
                     "runtime rejected duplicate of step %d (%s %s): "
                     "its result is in history — use it or act differently",
                     dup_step, act.tool,
                     args_json ? args_json : "");
            snprintf(last_tool, sizeof last_tool, "%s", act.tool);
            last_ok = false;
            last_result_json[0] = 0;
            if (cfg->mem) {
                char line[OBS_TRAIL_CAP];
                snprintf(line, sizeof line, "runtime rejected repeat call %s%s"
                         " for goal \"%s\" at step %d",
                         act.tool, args_json ? args_json : "", cfg->goal, step);
                mem_remember(cfg->mem, MEM_EPISODIC, line, NULL);
            }
            json_val *h = json_new_obj();
            json_obj_set(h, "step", json_new_num((double)step));
            json_obj_set(h, "tool", json_new_str(act.tool));
            json_obj_set(h, "ok", json_new_bool(false));
            json_obj_set(h, "note", json_new_str("REJECTED: duplicate of an earlier step"));
            json_arr_push(history, h);
            agent_action_free(&act);
            free(args_json);
            continue;
        }
        st = tool_exec(cfg->tools, act.tool, act.args, &result,
                       err, sizeof err);
        act.args = NULL; /* consumed by tool_exec */
        snprintf(last_tool, sizeof last_tool, "%s", act.tool);
        last_ok = (st == AIOS_OK);

        /* session history for the cortex (with a result digest) */
        {
            char note[96] = {0};
            if (st == AIOS_OK && result) {
                char *rj = json_dump(result);
                if (rj) {
                    snprintf(note, sizeof note, "%.88s", rj);
                    free(rj);
                }
            } else {
                snprintf(note, sizeof note, "ERROR: %.78s", err);
            }
            json_val *h = json_new_obj();
            json_obj_set(h, "step", json_new_num((double)step));
            json_obj_set(h, "tool", json_new_str(act.tool));
            json_obj_set(h, "ok", json_new_bool(last_ok));
            json_obj_set(h, "note", json_new_str(note));
            json_arr_push(history, h);
            if (hn < 64) {
                h_tool[hn] = strdup(act.tool);
                h_args[hn] = args_json ? args_json : strdup("null");
                hn++;
                args_json = NULL;
            }
        }
        free(args_json);

        /* reflect: write the tool outcome into episodic memory */
        if (cfg->mem) {
            char line[OBS_TRAIL_CAP];
            if (st == AIOS_OK && result) {
                char *rj = json_dump(result);
                snprintf(last_result_json, sizeof last_result_json,
                         "%.*s", (int)sizeof last_result_json - 1,
                         rj ? rj : "");
                snprintf(line, sizeof line,
                         "tool %s used for goal \"%s\" -> %.240s",
                         act.tool, cfg->goal,
                         rj ? rj : "(empty result)");
                free(rj);
            } else {
                last_result_json[0] = 0;
                snprintf(line, sizeof line,
                         "tool %s failed for goal \"%s\": %.240s",
                         act.tool, cfg->goal, err);
            }
            mem_remember(cfg->mem, MEM_EPISODIC, line, NULL);
        } else if (st == AIOS_OK && result) {
            char *rj = json_dump(result);
            snprintf(last_result_json, sizeof last_result_json,
                     "%.*s", (int)sizeof last_result_json - 1, rj ? rj : "");
            free(rj);
        } else {
            last_result_json[0] = 0;
        }
        json_free(result);

        if (st != AIOS_OK) {
            snprintf(last_err, sizeof last_err, "%s", err);
            if (final_out && cap) snprintf(final_out, cap, "%s", err);
        } else {
            last_err[0] = 0;
        }

        agent_action_free(&act);
    }

    json_free(history);
    for (int i = 0; i < hn; i++) { free(h_tool[i]); free(h_args[i]); }
    if (final_out && cap && final_out[0] == 0)
        snprintf(final_out, cap, "step budget exhausted before completion");
    return AIOS_ERR_LIMIT;
}
