/* aios cortex backends — M4.
 * cortex_scripted: deterministic policy for the workspace-summary goal;
 *                  used by tests and offline demos.
 * cortex_llm:      real planning model via the `z-ai chat` CLI bridge
 *                  (writes strict-JSON actions; verified + retried). */
#include "aios/agent.h"
#include "llmbridge.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================= scripted cortex ================= */

typedef struct {
    char *goal;
    int   total_bytes;      /* accumulated fs.read byte counts */
    int   files_seen;
} sctx;

static void sctx_destroy(cortex *cx) {
    sctx *s = (sctx *)cx->ud;
    free(s->goal);
    free(s);
    free(cx);
}

/* Extract an integer member from the last tool result, if present. */
static int last_result_num(const char *obs_json, const char *key) {
    json_val *o = json_parse(obs_json, strlen(obs_json));
    if (!o) return -1;
    json_val *last = json_obj_get(o, "last");
    json_val *res  = last ? json_obj_get(last, "result") : NULL;
    json_val *v    = res ? json_obj_get(res, key) : NULL;
    int rc = v && json_type_of(v) == JSON_NUM ? (int)json_num(v) : -1;
    json_free(o);
    return rc;
}

static aios_status scripted_plan(cortex *cx, const char *obs_json,
                                 agent_action *out, char *err, size_t ecap) {
    sctx *s = (sctx *)cx->ud;
    json_val *o = json_parse(obs_json, strlen(obs_json));
    int step = o ? (int)json_num(json_obj_get(o, "step")) : 0;
    json_free(o);

    /* accumulate bytes from the previous fs.read result */
    int b = last_result_num(obs_json, "bytes");
    if (b > 0) {
        s->total_bytes += b;
        s->files_seen++;
    }

    memset(out, 0, sizeof *out);
    switch (step) {
    case 0:
        out->thought = strdup("list the workspace to see what is there");
        out->tool = strdup("fs.list");
        out->args = json_new_obj();
        json_obj_set(out->args, "path", json_new_str("."));
        break;
    case 1:
        out->thought = strdup("read notes.txt");
        out->tool = strdup("fs.read");
        out->args = json_new_obj();
        json_obj_set(out->args, "path", json_new_str("notes.txt"));
        break;
    case 2:
        out->thought = strdup("read metrics.csv");
        out->tool = strdup("fs.read");
        out->args = json_new_obj();
        json_obj_set(out->args, "path", json_new_str("metrics.csv"));
        break;
    case 3: {
        long tokens = s->total_bytes / 4;
        char expr[64];
        snprintf(expr, sizeof expr, "%d/4", s->total_bytes);
        out->thought = strdup("estimate tokens from observed byte counts");
        out->tool = strdup("calc");
        out->args = json_new_obj();
        json_obj_set(out->args, "expr", json_new_str(expr));
        (void)tokens;
        break;
    }
    case 4: {
        long tokens = s->total_bytes / 4;
        char body[512];
        snprintf(body, sizeof body,
                 "# Workspace summary\n\n- files read: %d\n- total bytes: %d\n"
                 "- estimated tokens (~chars/4): %ld\n",
                 s->files_seen, s->total_bytes, tokens);
        out->thought = strdup("write the summary file");
        out->tool = strdup("fs.write");
        out->args = json_new_obj();
        json_obj_set(out->args, "path", json_new_str("summary.md"));
        json_obj_set(out->args, "content", json_new_str(body));
        break;
    }
    default: {
        long tokens = s->total_bytes / 4;
        char final[256];
        snprintf(final, sizeof final,
                 "Wrote summary.md: %d files, %d bytes, ~%ld tokens.",
                 s->files_seen, s->total_bytes, tokens);
        out->done = true;
        out->final = strdup(final);
        const char *facts = "[\"workspace summary written to summary.md\","
                            "\"AI-OS scripted agent completed a full tool loop\"]";
        out->facts_json = strdup(facts);
        break;
    }
    }
    if ((!out->done && (!out->tool || !out->args)) ||
        (out->done && !out->final)) {
        snprintf(err, ecap, "scripted cortex failed to build action");
        agent_action_free(out);
        return AIOS_ERR_STATE;
    }
    return AIOS_OK;
}

cortex *cortex_scripted_create(const char *goal) {
    cortex *cx = (cortex *)calloc(1, sizeof(cortex));
    if (!cx) return NULL;
    sctx *s = (sctx *)calloc(1, sizeof(sctx));
    if (!s) { free(cx); return NULL; }
    s->goal = goal ? strdup(goal) : NULL;
    cx->name = "scripted";
    cx->plan = scripted_plan;
    cx->ud = s;
    cx->destroy = sctx_destroy;
    return cx;
}

/* ================= LLM cortex ================= */

#define SYS_PROMPT_CAP  4096
#define OBS_ARG_MAX     (96u * 1024)   /* stay well under MAX_ARG_STRLEN */

typedef struct {
    char system[SYS_PROMPT_CAP];
} lctx;

static void lctx_destroy(cortex *cx) {
    free(cx->ud);
    free(cx);
}

/* Run `z-ai chat -s SYSTEM -p OBS -o out.json`; returns AIOS_OK and
 * fills *content_out (malloc'd model message content) on success.
 * Retries with backoff on transient failures (429 rate limiting).
 * Shared with the M5 shell intent router (see llmbridge.h). */
aios_status aios_llm_call(const char *system, const char *obs,
                          char **content_out, char *err, size_t ecap) {
    *content_out = NULL;
    static int dbg_seq = 0;
    int dbg_id = ++dbg_seq;
    const char *dbg_dir = getenv("AIOS_LLM_DEBUG");
    if (dbg_dir) {
        char p[512];
        snprintf(p, sizeof p, "%s/obs-%02d.json", dbg_dir, dbg_id);
        FILE *df = fopen(p, "wb");
        if (df) { fputs(obs, df); fclose(df); }
    }
    char tmp_out[] = "/tmp/aios-llm-XXXXXX.json";
    char tmp_err[] = "/tmp/aios-llm-XXXXXX.err";
    int fd = mkstemps(tmp_out, 5);
    if (fd < 0) {
        snprintf(err, ecap, "cannot create temp file for llm output");
        return AIOS_ERR_IO;
    }
    close(fd);
    int efd = mkstemps(tmp_err, 4);
    if (efd >= 0) close(efd);

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, ecap, "fork failed");
        return AIOS_ERR_IO;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        int errfd = open(tmp_err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        /* the CLI logs banners on stdout; the completion arrives via -o */
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        if (errfd >= 0) { dup2(errfd, STDERR_FILENO); close(errfd); }
        execlp("z-ai", "z-ai", "chat",
               "-s", system,
               "-p", obs,
               "-o", tmp_out,
               (char *)NULL);
        _exit(127);
    }
    int wstatus = 0;
    if (waitpid(pid, &wstatus, 0) != pid || !WIFEXITED(wstatus) ||
        WEXITSTATUS(wstatus) != 0) {
        /* surface the CLI's own diagnostics (rate limits, auth, ...) */
        char *diag = NULL;
        size_t dlen = 0;
        char msg[256] = "bridge failed";
        if (aios_read_file(tmp_err, &diag, &dlen, 64 * 1024) == AIOS_OK && diag) {
            /* keep the last non-empty line, trimmed */
            char *tail = diag + dlen;
            while (tail > diag && (tail[-1] == '\n' || tail[-1] == '\r' || !tail[-1])) tail--;
            char *start = tail;
            while (start > diag && start[-1] != '\n') start--;
            size_t n = (size_t)(tail - start);
            snprintf(msg, sizeof msg, "bridge: %.*s",
                     (int)(n > 160 ? 160 : n), start);
            free(diag);
        }
        snprintf(err, ecap, "%s (status %d)", msg,
                 WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);
        remove(tmp_err);
        return AIOS_ERR_TOOL;
    }
    remove(tmp_err);

    char *raw = NULL;
    size_t rlen = 0;
    aios_status st = aios_read_file(tmp_out, &raw, &rlen, 4u * 1024 * 1024);
    remove(tmp_out);
    if (st != AIOS_OK) {
        snprintf(err, ecap, "cannot read llm output (%s)", aios_status_str(st));
        return AIOS_ERR_IO;
    }
    json_val *env = json_parse(raw, rlen);
    free(raw);
    if (!env) {
        snprintf(err, ecap, "llm bridge output unparsable");
        return AIOS_ERR_TOOL;
    }
    json_val *choices = json_obj_get(env, "choices");
    const char *content = NULL;
    if (json_arr_len(choices) > 0) {
        json_val *c0 = json_arr_get(choices, 0);
        json_val *msg = json_obj_get(c0, "message");
        content = json_str(json_obj_get(msg, "content"));
    }
    if (!content) {
        json_free(env);
        snprintf(err, ecap, "llm response missing content");
        return AIOS_ERR_TOOL;
    }
    if (dbg_dir) {
        char p[512];
        snprintf(p, sizeof p, "%s/resp-%02d.txt", dbg_dir, dbg_id);
        FILE *df = fopen(p, "wb");
        if (df) { fputs(content, df); fclose(df); }
    }
    *content_out = strdup(content);
    json_free(env);
    return *content_out ? AIOS_OK : AIOS_ERR_NOMEM;
}

/* Strip markdown fences and leading prose; find the first '{' .. last '}'.
 * Shared with the M5 shell intent router (see llmbridge.h). */
char *aios_llm_extract_json(const char *s) {
    const char *first = strchr(s, '{');
    if (!first) return NULL;
    const char *last = strrchr(s, '}');
    if (!last || last < first) return NULL;
    size_t n = (size_t)(last - first + 1);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, first, n);
    out[n] = 0;
    return out;
}

static aios_status llm_parse_action(const char *content, agent_action *out,
                                    char *err, size_t ecap) {
    char *js = aios_llm_extract_json(content);
    if (!js) {
        snprintf(err, ecap, "model output contains no JSON object");
        return AIOS_ERR_TOOL;
    }
    json_val *o = json_parse(js, strlen(js));
    free(js);
    if (!o || json_type_of(o) != JSON_OBJ) {
        json_free(o);
        snprintf(err, ecap, "model action is not a JSON object");
        return AIOS_ERR_TOOL;
    }
    memset(out, 0, sizeof *out);
    const char *th = json_str(json_obj_get(o, "thought"));
    out->thought = th ? strdup(th) : NULL;
    json_val *done = json_obj_get(o, "done");
    if (done && json_bool(done)) {
        out->done = true;
        const char *fin = json_str(json_obj_get(o, "final"));
        out->final = fin ? strdup(fin) : strdup("(model finished without a summary)");
        json_val *facts = json_obj_take(o, "facts");
        if (facts && json_type_of(facts) == JSON_ARR) {
            out->facts_json = json_dump(facts);
        }
        json_free(facts);
        json_free(o);
        return AIOS_OK;
    }
    const char *tool = json_str(json_obj_get(o, "tool"));
    json_val *args = json_obj_take(o, "args");
    /* copy tool out before the tree is freed */
    char *tool_copy = tool ? strdup(tool) : NULL;
    json_free(o);
    if (!tool_copy || !*tool_copy) {
        agent_action_free(out);
        json_free(args);
        free(tool_copy);
        snprintf(err, ecap, "model action missing \"tool\"");
        return AIOS_ERR_TOOL;
    }
    if (args && json_type_of(args) != JSON_OBJ) {
        json_free(args);
        args = json_new_obj();
    }
    out->tool = tool_copy;
    out->args = args ? args : json_new_obj();
    if (!out->tool || !out->args) {
        agent_action_free(out);
        return AIOS_ERR_NOMEM;
    }
    return AIOS_OK;
}

/* Retry wrapper: transient API failures (429 rate limits, network blips)
 * get up to 3 attempts with exponential backoff. */
static aios_status llm_call(const char *system, const char *obs,
                            char **content_out, char *err, size_t ecap) {
    aios_status st = AIOS_ERR;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            /* 429 needs a real cooldown; small backoffs just burn quota */
            unsigned ms = strstr(err, "429") || strstr(err, "Too many")
                          ? (attempt == 1 ? 10000u : 20000u)
                          : (1500u << (attempt - 1));
            struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        st = aios_llm_call(system, obs, content_out, err, ecap);
        if (st == AIOS_OK) return st;
        /* only retry on plausible-transient failures */
        if (!strstr(err, "429") && !strstr(err, "Too many") &&
            !strstr(err, "timeout") && !strstr(err, "network") &&
            !strstr(err, "ECONN") && !strstr(err, "529") &&
            !strstr(err, "502") && !strstr(err, "503") &&
            !strstr(err, "status -1"))
            break;
    }
    return st;
}

static aios_status llm_plan(cortex *cx, const char *obs_json,
                            agent_action *out, char *err, size_t ecap) {
    lctx *l = (lctx *)cx->ud;
    if (strlen(obs_json) >= OBS_ARG_MAX) {
        snprintf(err, ecap, "observation too large for llm bridge");
        return AIOS_ERR_LIMIT;
    }
    char *content = NULL;
    aios_status st = llm_call(l->system, obs_json, &content, err, ecap);
    if (st != AIOS_OK) return st;
    st = llm_parse_action(content, out, err, ecap);
    if (st != AIOS_OK) {
        /* one corrective retry: tell the model its output was invalid */
        char obs2[OBS_ARG_MAX + 512];
        snprintf(obs2, sizeof obs2,
                 "%s\n\nIMPORTANT: your previous reply was not a valid action "
                 "object (%s). Reply with ONLY the JSON action object.",
                 obs_json, err);
        free(content);
        content = NULL;
        st = llm_call(l->system, obs2, &content, err, ecap);
        if (st == AIOS_OK) st = llm_parse_action(content, out, err, ecap);
    }
    free(content);
    return st;
}

cortex *cortex_llm_create(const char *system_extra) {
    cortex *cx = (cortex *)calloc(1, sizeof(cortex));
    if (!cx) return NULL;
    lctx *l = (lctx *)calloc(1, sizeof(lctx));
    if (!l) { free(cx); return NULL; }
    int n = snprintf(l->system, sizeof l->system,
        "You are the planning cortex (the 'brain') of an AI-native operating "
        "system. An agent process is pursuing a goal using OS tools. Each "
        "turn you receive an observation JSON with the goal, remaining "
        "steps, the tool catalogue, relevant OS memories, this session's "
        "action history, and the last tool result. Decide exactly ONE next "
        "action. Never repeat an action already in history — use its "
        "results and move forward.\n"
        "Respond with ONLY one JSON object, no markdown, no commentary:\n"
        "  {\"thought\":\"short reason\",\"tool\":\"<name>\",\"args\":{...}}\n"
        "or, when the goal is fully achieved:\n"
        "  {\"thought\":\"...\",\"done\":true,\"final\":\"result for the user\","
        "\"facts\":[\"one-line durable facts worth remembering\"]}\n"
        "Rules: use only listed tools; NEVER call a tool with arguments "
        "identical to a history entry — its note already carries the result; "
        "after reading the inputs, compute with calc, write the requested "
        "output with fs.write, then finish; keep 'thought' under 200 chars.%s%s",
        system_extra ? "\n" : "",
        system_extra ? system_extra : "");
    if (n < 0 || n >= (int)sizeof l->system) { free(l); free(cx); return NULL; }
    cx->name = "llm";
    cx->plan = llm_plan;
    cx->ud = l;
    cx->destroy = lctx_destroy;
    return cx;
}
