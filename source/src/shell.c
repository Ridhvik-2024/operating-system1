/* shell.c — M5: the aiosh engine.
 *
 * Line grammar (one sh_exec call):
 *   line     := pipeline (';' pipeline)*        — stops at first failure
 *   pipeline := command ('|' command)*           — text+JSON flows right
 *   command  := word ( arg )*                    — "quoted", 'quoted',
 *                                                  key=value, {json}
 *   $name ${name} $out $? $json expand; $$ is a literal '$'; no backslash
 *   escapes; '#' starts a comment line; single quotes disable expansion.
 *
 * Command resolution: builtin -> registered tool -> NL intent router.
 * Background agent jobs run in forked children with their own memory,
 * tool registry and cortex; results land in <store>/jobs/<id>.json.
 * M8 durable: every job spawn/exit is appended (fsync'd) to
 * <store>/jobs.jsonl, and a fresh shell replays that journal — the job
 * table survives reboots when the store lives on the ext2 side disk. */
#include "aios/shell.h"
#include "aios/ksched.h"
#include "aios/ebpf.h"
#include "aios/agent.h"
#include "aios/microlm.h"
#include "aios/autopilot.h"
#include "aios/durable.h"
#include "aios/policy.h"
#include "aios/stage.h"
#include "shroute.h"

#include <dirent.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SH_ERR_CAP     1024
#define SH_MAX_VARS    64
#define SH_HIST_MAX    64
#define SH_MAX_JOBS    64
#define SH_JOB_WAIT_MS 120000.0

/* ---------------- tiny dynamic string ---------------- */

typedef struct { char *s; size_t n, cap; } sb;

static void sb_putn(sb *b, const char *s, size_t len) {
    if (!s || !len) return;
    if (b->n + len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        while (nc < b->n + len + 1) nc *= 2;
        char *ns = (char *)realloc(b->s, nc);
        if (!ns) return; /* drop on oom; shell keeps running */
        b->s = ns; b->cap = nc;
    }
    memcpy(b->s + b->n, s, len);
    b->n += len;
    b->s[b->n] = 0;
}
static void sb_puts(sb *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(sb *b, char c)        { sb_putn(b, &c, 1); }

static char *xstrdup(const char *s) {
    return s ? strdup(s) : strdup("");
}

static char *trim_dup(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\n' || s[n-1]=='\r'))
        n--;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* join argv[from..] with single spaces; malloc'd; "" when empty */
static char *join_args(char **v, size_t n, size_t from) {
    sb b = {0};
    for (size_t i = from; i < n; i++) {
        if (b.n) sb_putc(&b, ' ');
        sb_puts(&b, v[i]);
    }
    if (!b.s) return strdup("");
    return b.s;
}

/* ---------------- shell state ---------------- */

/* M8 durable job journal (defined below, used by sh_poll_jobs) */
static void jobs_journal(const char *store, const char *op, long id,
                         const char *goal, const char *state);

enum { JOB_RUNNING = 0, JOB_DONE = 1, JOB_FAILED = 2, JOB_KILLED = 3 };

typedef struct {
    long   id;
    pid_t  pid;
    char  *goal;
    double started_ms;
    int    state;
    char  *final;      /* from result file once finished */
} sh_job;

struct sh_shell {
    sembus   *bus;
    mem_ctx  *mem;
    tool_reg *tools;
    char      jail[512];
    char      store[512];
    char      cortex_mode[16];
    int       last_needs_sandbox;    /* M2: host requirements from the verdict */
    int       last_needs_rollback;

    /* M3: a machine-originated write lands here instead of the jail. NULL means
     * the tool layer writes directly (a human typed the command, or the caller
     * opted out). Set up per command, torn down after it. */
    char      stage_dir[512];
    int       stage_live;

    char *var_name[SH_MAX_VARS];
    char *var_val[SH_MAX_VARS];
    int   nvars;

    char *hist[SH_HIST_MAX];
    int   hist_n;

    sh_job jobs[SH_MAX_JOBS];
    int    njobs;
    long   next_job_id;

    int   last_status;
    char *last_out;     /* text of the last executed line ($out)   */
    char *last_json;    /* json dump of the same line ($json)      */
    int   exit_requested;
};

static const char *tier_name(mem_tier t) {
    switch (t) {
    case MEM_WORKING:  return "working";
    case MEM_EPISODIC: return "episodic";
    default:           return "semantic";
    }
}

static const char *job_state_name(int s) {
    switch (s) {
    case JOB_RUNNING: return "running";
    case JOB_DONE:    return "done";
    case JOB_FAILED:  return "failed";
    default:          return "killed";
    }
}

/* ---------------- parser ---------------- */

typedef struct { char **v; size_t n, cap; } sargv;

static void sargv_push(sargv *a, char *tok) {   /* takes ownership */
    if (a->n + 1 > a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 8;
        char **nv = (char **)realloc(a->v, nc * sizeof *nv);
        if (!nv) { free(tok); return; }
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = tok;
}
static void sargv_free(sargv *a) {
    for (size_t i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    a->v = NULL; a->n = a->cap = 0;
}

typedef struct { char *raw; sargv argv; } scmd;
typedef struct { scmd *cmd; size_t n, cap; } spipe;
typedef struct { spipe *pl; size_t n, cap; } sline;

static void spipe_push(spipe *pl, scmd c) {
    if (pl->n + 1 > pl->cap) {
        size_t nc = pl->cap ? pl->cap * 2 : 4;
        scmd *nv = (scmd *)realloc(pl->cmd, nc * sizeof *nv);
        if (!nv) { free(c.raw); sargv_free(&c.argv); return; }
        pl->cmd = nv; pl->cap = nc;
    }
    pl->cmd[pl->n++] = c;
}
static void sline_push(sline *ln, spipe p) {
    if (ln->n + 1 > ln->cap) {
        size_t nc = ln->cap ? ln->cap * 2 : 4;
        spipe *nv = (spipe *)realloc(ln->pl, nc * sizeof *nv);
        if (!nv) {
            for (size_t i = 0; i < p.n; i++) { free(p.cmd[i].raw); sargv_free(&p.cmd[i].argv); }
            free(p.cmd);
            return;
        }
        ln->pl = nv; ln->cap = nc;
    }
    ln->pl[ln->n++] = p;
}
static void sline_free(sline *ln) {
    for (size_t i = 0; i < ln->n; i++) {
        spipe *p = &ln->pl[i];
        for (size_t k = 0; k < p->n; k++) { free(p->cmd[k].raw); sargv_free(&p->cmd[k].argv); }
        free(p->cmd);
    }
    free(ln->pl);
    ln->pl = NULL; ln->n = ln->cap = 0;
}

/* $name ${name} $? $out $json $$ — *p points at '$'. */
static void expand_dollar(sh_shell *sh, const char **pp, sb *tok, sb *raw) {
    const char *p = *pp + 1;
    if (*p == '$') { sb_putc(tok, '$'); sb_putc(raw, '$'); *pp = p + 1; return; }
    if (*p == '?') {
        char b[16];
        snprintf(b, sizeof b, "%d", sh->last_status ? 1 : 0);
        sb_puts(tok, b); sb_puts(raw, b);
        *pp = p + 1; return;
    }
    const char *name = NULL;
    size_t nlen = 0;
    char brace = 0;
    if (*p == '{') { brace = 1; name = p + 1; }
    else if (isalpha((unsigned char)*p) || *p == '_') name = p;
    if (name) {
        const char *q = name;
        while (isalnum((unsigned char)*q) || *q == '_') q++;
        nlen = (size_t)(q - name);
        if (brace && *q != '}') { nlen = 0; }        /* malformed ${...} */
    }
    if (nlen == 0) {                                  /* literal '$' */
        sb_putc(tok, '$'); sb_putc(raw, '$');
        *pp = *pp + 1;
        return;
    }
    char key[64];
    snprintf(key, sizeof key, "%.*s", (int)(nlen > 63 ? 63 : nlen), name);
    const char *val = NULL;
    if (strcmp(key, "out") == 0)       val = sh->last_out ? sh->last_out : "";
    else if (strcmp(key, "json") == 0) val = sh->last_json ? sh->last_json : "";
    else {
        for (int i = 0; i < sh->nvars; i++)
            if (strcmp(sh->var_name[i], key) == 0) { val = sh->var_val[i]; break; }
    }
    sb_puts(tok, val ? val : "");
    sb_puts(raw, val ? val : "");
    *pp = name + nlen + (brace ? 1 : 0);
}

/* Tokenize one line into pipelines of commands. Returns 0 or -1 on
 * unterminated quote. Raw segment text (post-expansion) is kept per
 * command for the natural-language router. */
static int parse_line(sh_shell *sh, const char *line, sline *out,
                      char *err, size_t ecap) {
    memset(out, 0, sizeof *out);
    scmd  cur_cmd  = {0};
    spipe cur_pipe = {0};
    sb tok = {0}, raw = {0};
    bool tok_any = false;
    char q = 0;
    const char *p = line;

#define FLUSH_TOKEN() do {                                          \
        if (tok.n > 0 || tok_any) {                                 \
            sargv_push(&cur_cmd.argv, tok.s ? tok.s : strdup(""));   \
        } else free(tok.s);                                         \
        tok.s = NULL; tok.n = tok.cap = 0; tok_any = false;         \
    } while (0)
#define FLUSH_CMD() do {                                            \
        if (cur_cmd.argv.n > 0 || raw.n > 0) {                      \
            cur_cmd.raw = raw.s ? raw.s : strdup("");               \
            raw.s = NULL; raw.n = raw.cap = 0;                      \
            spipe_push(&cur_pipe, cur_cmd);                         \
        } else free(raw.s);                                         \
        memset(&cur_cmd, 0, sizeof cur_cmd);                        \
        raw.s = NULL; raw.n = raw.cap = 0;                          \
    } while (0)
#define FLUSH_PIPE() do {                                           \
        if (cur_pipe.n > 0) sline_push(out, cur_pipe);              \
        else {                                                      \
            for (size_t i_ = 0; i_ < cur_pipe.n; i_++)              \
                { free(cur_pipe.cmd[i_].raw); sargv_free(&cur_pipe.cmd[i_].argv); } \
            free(cur_pipe.cmd);                                     \
        }                                                           \
        memset(&cur_pipe, 0, sizeof cur_pipe);                      \
    } while (0)

    while (*p) {
        char c = *p;
        if (q) {
            if (c == q) { q = 0; sb_putc(&raw, c); }
            else if (c == '$' && q != '\'') expand_dollar(sh, &p, &tok, &raw), p--;
            else sb_putc(&tok, c), sb_putc(&raw, c);
            p++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            FLUSH_TOKEN();
            sb_putc(&raw, ' ');
            p++;
            continue;
        }
        if (c == '|') {
            FLUSH_TOKEN();
            FLUSH_CMD();
            p++;
            continue;
        }
        if (c == ';') {
            FLUSH_TOKEN();
            FLUSH_CMD();
            FLUSH_PIPE();
            p++;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            tok_any = true;
            sb_putc(&raw, c);
            p++;
            continue;
        }
        if (c == '$') { expand_dollar(sh, &p, &tok, &raw); continue; }
        sb_putc(&tok, c);
        sb_putc(&raw, c);
        p++;
    }
    if (q) {
        snprintf(err, ecap, "unterminated %s quote", q == '\'' ? "single" : "double");
        free(tok.s); free(raw.s);
        free(cur_cmd.raw); sargv_free(&cur_cmd.argv);
        for (size_t i = 0; i < cur_pipe.n; i++)
            { free(cur_pipe.cmd[i].raw); sargv_free(&cur_pipe.cmd[i].argv); }
        free(cur_pipe.cmd);
        sline_free(out);
        return -1;
    }
    FLUSH_TOKEN();
    FLUSH_CMD();
    FLUSH_PIPE();
#undef FLUSH_TOKEN
#undef FLUSH_CMD
#undef FLUSH_PIPE
    (void)err; (void)ecap;
    return 0;
}

/* ---------------- tool execution ---------------- */

/* Pipe/display text of a tool result. Tools whose JSON result wraps a
 * payload render the payload (fs.read -> file content, calc -> the
 * number, fs.list -> one name per line, memory.search -> hit texts);
 * everything else renders as JSON. The structured form stays available
 * in $json and in result.json. */
static void stage_text_of(const char *tool, json_val *v, char **text_out) {
    if (!v) { *text_out = strdup(""); return; }
    if (json_type_of(v) == JSON_STR) { *text_out = xstrdup(json_str(v)); return; }
    json_val *f;
    if (tool && strcmp(tool, "fs.read") == 0 &&
        (f = json_obj_get(v, "content")) && json_str(f)) {
        *text_out = xstrdup(json_str(f));
        return;
    }
    if (tool && strcmp(tool, "calc") == 0 &&
        (f = json_obj_get(v, "value")) && json_type_of(f) == JSON_NUM) {
        char b[64];
        snprintf(b, sizeof b, "%g", json_num(f));
        *text_out = strdup(b);
        return;
    }
    if (tool && strcmp(tool, "fs.write") == 0 &&
        (f = json_obj_get(v, "path")) && json_str(f)) {
        char b[640];
        snprintf(b, sizeof b, "wrote %s", json_str(f));
        *text_out = strdup(b);
        return;
    }
    if (tool && strcmp(tool, "fs.list") == 0 &&
        (f = json_obj_get(v, "entries")) && json_type_of(f) == JSON_ARR) {
        sb b = {0};
        for (size_t i = 0; i < json_arr_len(f); i++) {
            const char *nm = json_str(json_obj_get(json_arr_get(f, i), "name"));
            if (nm) {
                if (b.n) sb_putc(&b, '\n');
                sb_puts(&b, nm);
            }
        }
        *text_out = b.s ? b.s : strdup("");
        return;
    }
    if (tool && strcmp(tool, "memory.search") == 0 &&
        (f = json_obj_get(v, "results")) && json_type_of(f) == JSON_ARR) {
        sb b = {0};
        for (size_t i = 0; i < json_arr_len(f); i++) {
            const char *t = json_str(json_obj_get(json_arr_get(f, i), "text"));
            if (t) {
                if (b.n) sb_putc(&b, '\n');
                sb_puts(&b, t);
            }
        }
        *text_out = b.s ? b.s : strdup("");
        return;
    }
    char *j = json_dump(v);
    *text_out = j ? j : strdup("");
}

/* ---- M1 gate ----------------------------------------------------------
 * The single place where a proposal stops being a proposal.
 *
 * M2 will put policy_eval() here; M3/M4 will put the stage-and-diff around
 * the call; M6 will hash the record into the audit chain. Today it exists to
 * make ONE thing true, which is the whole point of M1: no tool call can be
 * constructed anywhere in this file without first having been a validated,
 * origin-stamped, effect-declared aios_action.
 *
 * Note what is NOT here: no execution, no permission. The descriptor does not
 * get to decide anything; the host code below decides, having read it.
 * --------------------------------------------------------------------- */
/* Capabilities this shell holds. Compiled in, not configurable, for the same
 * reason the law table is: an environment variable that grants capabilities
 * is a privilege escalation with extra steps.
 *
 * HUMAN commands need no capability (the human is the authority). Everything
 * the MACHINE originates does, which is why the AI path can only use tools
 * listed here. fs.write is deliberately included and act:FS_DELETE is
 * deliberately NOT: the interesting part of M2 is not the deny, it is the
 * small, boring, auditable list of what is actually granted. */
static const char *SH_CAPS[] = {
    "tool:calc", "tool:fs.read", "tool:fs.list", "tool:fs.write",
    "tool:memory.search", "tool:proc.info", "tool:selftest.sleep",
    "agent:run",
};

#define SH_N_CAPS ((int)(sizeof SH_CAPS / sizeof SH_CAPS[0]))

static int action_gate(sh_shell *sh, const aios_action *act,
                       char *err, size_t ecap) {
    char why[256];

    /* ---- M1: is this even a proposal? ---- */
    if (!aios_action_is_valid(act, why, sizeof why)) {
        snprintf(err, ecap, "action refused: %s", why);
        sembus_pub_kv(sh->bus, "sh.action.refused",
                      (const char *const[]){
                          "kind", aios_action_kind_name(act ? act->kind : ACT__COUNT),
                          "target", (act && act->target) ? act->target : "",
                          "why", why, NULL});
        return 1;
    }

    /* ---- M2: is it permitted? ----
     * The verdict comes from a pure function that cannot consult the model,
     * cannot read the clock, and has no override path. Note what this code
     * cannot do: it cannot reason about the action, soften the verdict, or ask
     * the AI for justification. It reads a decision and obeys it. */
    aios_policy_ctx pctx = aios_policy_ctx_default();
    /* The sandbox is where the tool will actually run (the worker process),
     * so for a tool action it is performed rather than pending. A shell or
     * delete is not simulated yet - that is M3/M4, and until then those stay
     * denied instead of being waved through. */
    pctx.sandbox_executed = (act->kind == ACT_TOOL || act->kind == ACT_AGENT);
    /* No snapshot store exists yet (M5). Saying so is what makes the
     * rollback rule bite instead of pretending. */
    pctx.rollback_available = 0;

    aios_decision d = aios_policy_eval(act, sh->jail, SH_CAPS, SH_N_CAPS,
                                       &pctx, aios_now_ms());

    char *act_json = aios_action_to_json(act);
    if (d.v != VERDICT_ALLOW) {
        snprintf(err, ecap, "action refused by policy [%s]: %s",
                 aios_policy_reason_str(d.r), d.why);
        sembus_pub_kv(sh->bus, "sh.action.refused",
                      (const char *const[]){
                          "kind", aios_action_kind_name(act->kind),
                          "target", act->target ? act->target : "",
                          "verdict", aios_policy_verdict_str(d.v),
                          "reason", aios_policy_reason_str(d.r),
                          "why", d.why, NULL});
        /* The refusal is logged as a record, not just printed: a denied
         * action is the most interesting thing in an audit trail. */
        sembus_pub_kv(sh->bus, "sh.action.log",
                      (const char *const[]){
                          "verdict", aios_policy_verdict_str(d.v),
                          "reason", aios_policy_reason_str(d.r),
                          "action", act_json ? act_json : "", NULL});
        free(act_json);
        return 1;
    }

    sembus_pub_kv(sh->bus, "sh.action.log",
                  (const char *const[]){
                      "verdict", "ALLOW",
                      "reason", aios_policy_reason_str(d.r),
                      "action", act_json ? act_json : "", NULL});
    free(act_json);

    /* Requirements the host must honour before this is real. Both are
     * reported so the M3/M5 work has a consumer rather than a comment. */
    if (d.needs_sandbox) sh->last_needs_sandbox = 1;
    if (d.needs_rollback) sh->last_needs_rollback = 1;
    return 0;
}

/* Build (and publish) the record of an action we accepted. The audit chain
 * proper is M6; this is the descriptor being made observable NOW so that the
 * later milestones have something to chain. */
static void action_accept(sh_shell *sh, const aios_action *act) {
    char *j = aios_action_to_json(act);
    if (!j) return;
    sembus_pub(sh->bus, "sh.action.accept", j);
    free(j);
}

/* args is consumed on every path (tool_exec contract). */
static int exec_tool(sh_shell *sh, const char *name, json_val *args,
                     char **text_out, json_val **json_out,
                     char *err, size_t ecap) {
    json_val *res = NULL;
    aios_status st = tool_exec(sh->tools, name, args, &res, err, ecap);
    if (st != AIOS_OK) {
        stage_text_of(NULL, NULL, text_out);
        *json_out = NULL;
        return 1;
    }
    stage_text_of(name, res, text_out);
    *json_out = res;
    return 0;
}

/* ---- M3: staging for machine-originated writes ----
 *
 * The rule: if an AI chose the action, its writes do not land. They go to a
 * staging directory, are shown as a diff, and land only on `promote`. A human's
 * own `fs.write` still writes directly -- gating a person's explicit command
 * behind a second confirmation is theatre, and people learn to click through
 * theatre.
 *
 * The staged directory is passed to the worker as an ARGUMENT, not an env var:
 * an environment variable is inherited by everything the worker spawns, so it
 * would leak the staging location into any process the tool starts.
 */
static bool sh_stage_begin(sh_shell *sh, aios_action *act, char *err, size_t ecap) {
    if (!sh || !act) return false;
    stage_session s;
    if (!stage_begin(sh->jail, sh->store, act->intent_hash, 0, &s, err, ecap)) {
        sh->stage_live = 0;
        sh->stage_dir[0] = 0;
        return false;
    }
    snprintf(sh->stage_dir, sizeof sh->stage_dir, "%s", s.dir);
    sh->stage_live = 1;

    /* Pre-note every declared write so the diff has rows even if the worker
     * never gets the chance to note its own. The note is idempotent. */
    for (int i = 0; i < act->n_declared; i++) {
        const char *eff = act->declared[i];       /* "write:a/b.txt" */
        if (!eff || strncmp(eff, "write:", 6) != 0) continue;
        const char *p = eff + 6;
        if (*p) stage_note(&s, p, STAGE_OP_ADD, err, ecap);
    }
    stage_end(&s);
    return true;
}

static void sh_stage_drop(sh_shell *sh) {
    if (!sh) return;
    sh->stage_live = 0;
    sh->stage_dir[0] = 0;
}

/* After a staged tool run: turn the session into a diff for the human. */
static int sh_stage_report(sh_shell *sh, char **text_out, char *err, size_t ecap) {
    stage_session s;
    if (!stage_open(sh->stage_dir, &s, err, ecap)) {
        sh_stage_drop(sh);
        *text_out = strdup("(staged, but the session could not be reopened)\n");
        return 0;
    }
    stage_diff d;
    int rc = 0;
    if (stage_diff_compute(&s, &d, err, ecap)) {
        char *body = stage_diff_text(&d);
        char *msg = NULL;
        if (asprintf(&msg,
                     "%s"
                     "  nothing was written to the jail.\n"
                     "  promote            apply it\n"
                     "  discard            throw it away\n",
                     body ? body : "") < 0)
            msg = NULL;
        free(body);
        *text_out = msg ? msg : strdup("(staged)\n");
        stage_diff_free(&d);
    } else {
        *text_out = strdup("(staged, but the diff could not be computed)\n");
        rc = 0;
    }
    stage_end(&s);
    return rc;
}

/* ---- the guardian: how the agent loop reaches policy and staging ----
 *
 * The agent loop used to call tool_exec() directly. That made it the one place
 * an action ran with no descriptor, no verdict and no staging -- and the agent
 * is precisely the component most likely to act unsupervised, so this was the
 * largest hole in the constitution, not a detail.
 *
 * Every step now builds a real descriptor and asks the same two questions the
 * shell asks: is this permitted, and does it land here or in staging?
 */
typedef struct {
    sh_shell *sh;
    const char *goal;      /* the agent's goal, for the intent hash */
    size_t    n_staged;
    int       n_refused;
} sh_guard_ctx;

static int sh_agent_guard(void *ud, const char *tool, json_val *args,
                          char *err, size_t ecap) {
    sh_guard_ctx *g = (sh_guard_ctx *)ud;
    if (!g || !g->sh) return 1;
    sh_shell *sh = g->sh;

    const char *path = json_str(json_obj_get(args, "path"));
    aios_act_kind kind = ACT_TOOL;
    if (strcmp(tool, "fs.write") == 0) kind = ACT_FS_WRITE;
    else if (strcmp(tool, "fs.read") == 0 || strcmp(tool, "fs.list") == 0 ||
             strcmp(tool, "calc") == 0 || strcmp(tool, "memory.search") == 0 ||
             strcmp(tool, "proc.info") == 0)
        kind = ACT_TOOL;

    /* An AI action. There is no path from the agent loop to a human-origin
     * descriptor: the origin is not a parameter the planner can choose. */
    /* The target must be the TOOL for a tool action, not the path: the gate
     * looks up `target` among the shell's capabilities, so `fs.write` has to
     * appear there for the capability rule to be decided. The path travels in
     * args, and policy step 2 also inspects it directly. */
    aios_action *act = aios_action_new(kind, ORIGIN_AI, tool);
    if (!act) {
        snprintf(err, ecap, "cannot build a descriptor for %s", tool);
        return 1;
    }

    /* An AI action must carry an intent hash, and M1 refuses one that does
     * not. A fresh descriptor has none, so without this the guardian would
     * refuse every single step -- fail-closed, but useless. The intent is the
     * agent's goal plus the tool, which is what a reviewer would want to see
     * in the audit record. */
    char intent[256];
    snprintf(intent, sizeof intent, "%.180s -> %s", g->goal ? g->goal : "", tool);
    aios_action_set_intent(act, intent);
    (void)path;

    int rc = 0;
    if (kind == ACT_FS_WRITE && path) {
        /* Declare the effects on the action itself, so policy sees exactly the
         * descriptor a typed command would produce. This also gives step 2d a
         * `path` to inspect even when the effect could not be declared. */
        /* The action carries the path so M1 can validate it and policy step 2
         * can inspect it; the content stays with the planner's args, which are
         * what actually reach the tool. */
        json_val *probe = json_new_obj();
        json_obj_set(probe, "path", json_new_str(path));
        json_obj_set(probe, "content", json_new_str(""));
        aios_action_set_args(act, probe);
        aios_action_declare_tool_effects(act, tool, probe);
    }

    if (action_gate(sh, act, err, ecap) != 0) {
        g->n_refused++;
        aios_action_free(act);
        return 1;
    }
    action_accept(sh, act);

    /* An AI write is staged, not applied. This is the same rule the typed
     * path follows; the difference is that here the human never saw the
     * command, so the diff afterwards is the only place they can catch it. */
    if (kind == ACT_FS_WRITE) {
        if (!sh->stage_live) {
            if (!sh_stage_begin(sh, act, err, ecap)) {
                aios_action_free(act);
                return 1;
            }
        }
        tool_reg_set_staging(sh->tools, sh->stage_dir);
        g->n_staged++;
    }

    aios_action_free(act);
    return rc;
}

/* Execute a tool THROUGH its descriptor.
 *
 * `act` is borrowed: the caller keeps ownership and frees it after the call,
 * so the refusal text and any later milestone can still see it. The args are
 * transferred out of the descriptor here, because tool_exec's contract is
 * that args is consumed on every path.
 *
 * This is the ONLY way a tool runs. Command execution, argv parsing and the
 * intent router all end up here (run_command -> run_tool_stage /
 * SH_ROUTE_TOOL -> exec_tool_action). */
static int exec_tool_action(sh_shell *sh, aios_action *act,
                            char **text_out, json_val **json_out,
                            char *err, size_t ecap) {
    if (action_gate(sh, act, err, ecap) != 0) {
        stage_text_of(NULL, NULL, text_out);
        *json_out = NULL;
        return 1;
    }
    action_accept(sh, act);

    /* M3: an AI write is intercepted before it can reach the jail. */
    bool staged = false;
    if (act->origin == ORIGIN_AI && strcmp(act->target, "fs.write") == 0) {
        if (!sh_stage_begin(sh, act, err, ecap)) {
            stage_text_of(NULL, NULL, text_out);
            *json_out = NULL;
            return 1;
        }
        staged = true;
    }

    /* Tell the tool layer where an AI write must go. Cleared on every path so
     * one staged command cannot redirect the next one. */
    tool_reg_set_staging(sh->tools, staged ? sh->stage_dir : NULL);
    int rc = exec_tool(sh, act->target, aios_action_take_args(act),
                       text_out, json_out, err, ecap);
    tool_reg_set_staging(sh->tools, NULL);
    if (staged) {
        /* The worker wrote into the staging dir (it was told where). Replace
         * the raw tool text with the diff the human needs to decide. */
        free(*text_out);
        char *msg = NULL;
        rc = sh_stage_report(sh, &msg, err, ecap);
        *text_out = msg;
    }
    return rc;
}

/* Build tool args from argv (key=value / {json} / positional shorthand),
 * then inject piped input where the tool wants it.
 *
 * ORIGIN_HUMAN: the human typed the tool name and the arguments. Nothing was
 * inferred, so a human is accountable for what runs. (A natural-language line
 * that the ROUTER turns into a tool is ORIGIN_AI - see run_command's router
 * branch - because there the machine chose the tool.) */
static int run_tool_stage(sh_shell *sh, const char *name, sargv *a,
                          const char *raw_line, const char *pipe_text,
                          char **text_out, json_val **json_out,
                          char *err, size_t ecap) {
    json_val *args = json_new_obj();
    if (!args) { snprintf(err, ecap, "oom"); return 1; }

    for (size_t i = 1; i < a->n; i++) {
        const char *tok = a->v[i];
        if (tok[0] == '{') {                       /* whole-args JSON */
            json_val *j = json_parse(tok, strlen(tok));
            if (!j || json_type_of(j) != JSON_OBJ || a->n > 2) {
                json_free(j);
                json_free(args);
                snprintf(err, ecap, "a {json} argument must be the only argument");
                return 1;
            }
            json_free(args);
            args = j;
            continue;
        }
        const char *eq = strchr(tok, '=');
        if (eq && eq != tok) {                     /* key=value */
            char key[64];
            size_t klen = (size_t)(eq - tok);
            snprintf(key, sizeof key, "%.*s", (int)(klen > 63 ? 63 : klen), tok);
            json_val *jv = json_parse(eq + 1, strlen(eq + 1));
            json_obj_set(args, key, jv ? jv : json_new_str(eq + 1));
            continue;
        }
        /* positional shorthand per tool */
        if (strcmp(name, "calc") == 0)
            json_obj_set(args, "expr", json_new_str(tok));
        else if (strcmp(name, "fs.read") == 0 || strcmp(name, "fs.list") == 0)
            json_obj_set(args, "path", json_new_str(tok));
        else if (strcmp(name, "fs.write") == 0) {
            if (!json_obj_get(args, "path"))
                json_obj_set(args, "path", json_new_str(tok));
            else
                json_obj_set(args, "content", json_new_str(tok));
        }
        else if (strcmp(name, "memory.search") == 0)
            json_obj_set(args, "query", json_new_str(tok));
        else if (strcmp(name, "selftest.sleep") == 0)
            json_obj_set(args, "ms", json_new_num(atof(tok)));
        else {
            json_free(args);
            snprintf(err, ecap, "tool %s takes key=value arguments "
                     "(unexpected \"%s\")", name, tok);
            return 1;
        }
    }

    /* pipe injection: documented tools accept the previous stage */
    if (pipe_text) {
        if (strcmp(name, "calc") == 0 && !json_obj_get(args, "expr")) {
            char *t = trim_dup(pipe_text);
            json_obj_set(args, "expr", json_new_str(t ? t : ""));
            free(t);
        } else if (strcmp(name, "fs.write") == 0 && !json_obj_get(args, "content")) {
            json_obj_set(args, "content", json_new_str(pipe_text));
        } else if (strcmp(name, "memory.search") == 0 && !json_obj_get(args, "query")) {
            json_obj_set(args, "query", json_new_str(pipe_text));
        }
    }
    /* M1: wrap the explicitly-typed command in a descriptor and validate it
     * before anything executes. The descriptor carries the effects the tool
     * implies, so the claim is on the record even when a human asked. */
    aios_action *act = aios_action_new(ACT_TOOL, ORIGIN_HUMAN, name);
    if (!act) {
        json_free(args);
        snprintf(err, ecap, "oom (descriptor)");
        return 1;
    }
    aios_action_set_intent(act, raw_line ? raw_line : name);
    aios_action_set_args(act, args);
    /* If the effects CANNOT be declared (e.g. a path too long to record), we
     * must not run the call: nothing downstream could then verify what it
     * touched. Refusing is the fail-closed choice, and the reason is stated
     * rather than a tool silently executing undeclared. */
    if (aios_action_declare_tool_effects(act, name, act->args) != AIOS_OK) {
        stage_text_of(NULL, NULL, text_out);
        *json_out = NULL;
        snprintf(err, ecap,
                 "action refused: effects of %s cannot be declared "
                 "(path too long to record)", name);
        aios_action_free(act);
        return 1;
    }
    int rc = exec_tool_action(sh, act, text_out, json_out, err, ecap);
    aios_action_free(act);
    return rc;
}

/* ---------------- agent execution ---------------- */

/* `goal` is the descriptor's target; the descriptor itself carries the
 * intent hash and origin that M6 will log and M2 will evaluate. An agent
 * cannot be started without one. */
static int run_agent_fg(sh_shell *sh, const aios_action *act,
                        char **text_out, char *err, size_t ecap) {
    if (action_gate(sh, act, err, ecap) != 0) {
        *text_out = strdup("");
        return 1;
    }
    action_accept(sh, act);
    const char *goal = act->target;
    cortex *cx = strcmp(sh->cortex_mode, "llm") == 0
                 ? cortex_llm_create(NULL)
                 : cortex_scripted_create(goal);
    if (!cx) {
        snprintf(err, ecap, "cannot create %s cortex%s", sh->cortex_mode,
                 strcmp(sh->cortex_mode, "llm") == 0 ? " (is z-ai on PATH?)" : "");
        return 1;
    }
    double t0 = aios_mono_ms();
    sembus_pub_kv(sh->bus, "sh.agent.start",
                  (const char *const[]){"agent", "sh", "goal", goal,
                                        "cortex", sh->cortex_mode, NULL});
    sh_guard_ctx gctx = { .sh = sh, .goal = goal, .n_staged = 0, .n_refused = 0 };
    agent_cfg cfg = {
        .name = "sh", .goal = goal, .cx = cx,
        .tools = sh->tools, .mem = sh->mem, .max_steps = 10,
        .guard = { .check = sh_agent_guard, .ud = &gctx }
    };
    char final[2048] = {0};
    aios_status st = agent_run(&cfg, final, sizeof final);
    char msbuf[32];
    snprintf(msbuf, sizeof msbuf, "%.0f", aios_mono_ms() - t0);
    sembus_pub_kv(sh->bus, "sh.agent.done",
                  (const char *const[]){"agent", "sh",
                                        "status", st == AIOS_OK ? "achieved"
                                                    : aios_status_str(st),
                                        "ms", msbuf, NULL});
    cortex_free(cx);

    /* If the agent tried to write, what the human needs is the diff, not the
     * agent's summary of it. The summary is optimistic by construction. */
    if (sh->stage_live) {
        char *diff = NULL;
        sh_stage_report(sh, &diff, err, ecap);
        if (diff) {
            char *msg = NULL;
            if (asprintf(&msg, "%s\n%s", final, diff) < 0) msg = NULL;
            free(diff);
            *text_out = msg ? msg : xstrdup(final);
        } else {
            *text_out = xstrdup(final);
        }
        sh->stage_live = 0;
        sh->stage_dir[0] = 0;
    } else {
        *text_out = xstrdup(final);
    }
    if (st != AIOS_OK) {
        /* Clamp the message into the caller's buffer on purpose: a failed
         * agent run may carry a 2 KB final state, and err/ecap is whatever
         * the caller asked for. Truncation is fine here - this is a
         * human-readable error, and snprintf always terminates. */
        const char *why = final[0] ? final : aios_status_str(st);
        size_t room = ecap > 8 ? ecap - 8 : 0;   /* room for "agent: " */
        snprintf(err, ecap, "agent: %.*s", (int)room, why);
        return 1;
    }
    return 0;
}

/* child body of `spawn`: builds fresh OS objects, runs the agent,
 * writes the result file, returns the exit code (caller _exits). */
static int job_child_main(const char *store, const char *jail, long id,
                          const char *goal, const char *mode) {
    double t0 = aios_mono_ms();
    /* M6 kernel bridge: background agent jobs are IDLE work — they only
     * run when nothing interactive wants the CPU, capped at 25%. */
    aios_ksched_apply(AIOS_KS_IDLE);
    aios_ksched_register(AIOS_KS_IDLE);
    aios_cgroup_move_pid((long)getpid(), AIOS_KS_IDLE);
    char name[32];
    snprintf(name, sizeof name, "job-%ld", id);
    mem_ctx  *m  = mem_open(store);
    tool_reg *tr = m ? tool_reg_create(jail, store, name, m) : NULL;
    if (tr) tool_reg_install_default(tr);
    cortex   *cx = (strcmp(mode, "llm") == 0)
                  ? cortex_llm_create(NULL)
                  : cortex_scripted_create(goal);
    char final[2048] = {0};
    aios_status st = AIOS_ERR_STATE;
    if (m && tr && cx) {
        agent_cfg cfg = {.name = name, .goal = goal, .cx = cx,
                         .tools = tr, .mem = m, .max_steps = 10};
        st = agent_run(&cfg, final, sizeof final);
    }
    json_val *o = json_new_obj();
    json_obj_set(o, "id",     json_new_num((double)id));
    json_obj_set(o, "goal",   json_new_str(goal));
    json_obj_set(o, "status", json_new_str(st == AIOS_OK ? "done"
                                          : st == AIOS_ERR_LIMIT ? "budget"
                                          : "failed"));
    json_obj_set(o, "final",  json_new_str(final));
    json_obj_set(o, "ms",     json_new_num(aios_mono_ms() - t0));
    char *dump = json_dump(o);
    json_free(o);
    if (dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/jobs/%ld.json", store, id);
        aios_write_file(path, dump, strlen(dump));
        aios_dur_fsync_path(path);   /* M8: result survives power loss */
        free(dump);
    }
    cortex_free(cx);
    tool_reg_free(tr);
    if (m) mem_close(m);
    return st == AIOS_OK ? 0 : 1;
}

/* ---------------- line iteration for text builtins ---------------- */

typedef struct { const char *s; size_t pos; } lineiter;

static bool next_line(lineiter *it, const char **start, size_t *len) {
    if (!it->s[it->pos]) return false;
    *start = it->s + it->pos;
    const char *nl = strchr(it->s + it->pos, '\n');
    if (nl) { *len = (size_t)(nl - *start); it->pos += *len + 1; }
    else    { *len = strlen(*start);        it->pos += *len; }
    return true;
}

/* ---------------- builtins ---------------- */

static const char HELP_TEXT[] =
    "aiosh — the AI-native shell\n"
    "grammar:  pipeline (';' pipeline)*   stop at first failure\n"
    "          command ('|' command)*     text+JSON flow to the next stage\n"
    "          args: word  \"double\"  'single' (no expansion)  key=value  {json}\n"
    "          $var ${var}  $out (last output)  $? (last status)  $json\n"
    "builtins:\n"
    "  help version history exit quit\n"
    "  echo TEXT...                print text (pipe source)\n"
    "  count [-w|-c]               count lines/words/chars of piped input\n"
    "  grep PAT                    keep lines of piped input containing PAT\n"
    "  head [N] | tail [N]         first/last N lines of piped input\n"
    "  remember [TEXT]             store text (or piped input) as episodic memory\n"
    "  recall QUERY                search OS memory by meaning\n"
    "  emit TOPIC {json}           publish on the shell's semantic bus\n"
    "  set NAME VALUE...           set a shell variable\n"
    "  unset NAME | vars           variable table\n"
    "  kernel                      show kernel bridge status (M6/M7)\n"
    "  autopilot                   autopilot governor status (M7)\n"
    "  tune [k=v ...]              show/rewrite the autopilot conf (M7)\n"
    "  lm [classify|gen|selftest]  the OS micro language model (M7)\n"
    "  dur [sync]                  durable-store status (ext2 side disk, M8)\n"
    "  cortex [scripted|micro|llm] show/switch the routing cortex\n"
    "  jail [DIR] | store [DIR]    show/rebind workspace jail / OS store\n"
    "  agent GOAL...               run an agent to completion (foreground)\n"
    "  spawn GOAL...               run an agent in the background\n"
    "  jobs | wait [ID|all] | kill ID     supervise background jobs\n"
    "tools:    anything in the registry: calc fs.read fs.write fs.list\n"
    "          memory.search proc.info  (args: key=value or {json})\n"
    "natural   anything else is routed: arithmetic -> calc,\n"
    "language  'read FILE' -> fs.read, 'list files' -> fs.list,\n"
    "          background/idle wording -> background job (agents class),\n"
    "          urgent wording -> foreground (interactive class),\n"
    "          action verbs -> the agent runtime, questions -> reply\n"
    "examples: fs.read notes.txt | count\n"
    "          echo 12*(7+5) | calc\n"
    "          summarize the workspace\n"
    "          spawn summarize the workspace ; jobs ; wait all\n";

/* `policy` — show the law. Also `policy check <kind> <target>` to ask the
 * evaluator what it would say, which is how a human (or a debugging engineer)
 * finds out why something they expected to work was refused.
 *
 * There is deliberately no `policy allow` / `policy override`: a writable rule
 * set is a privilege escalation with a nicer name. Changing the law means
 * changing the source and shipping an image. */
static int builtin_policy(sh_shell *sh, sargv *a,
                          char **text_out, json_val **json_out,
                          char *err, size_t ecap) {
    sb b = {0};

    if (a->n >= 2 && strcmp(a->v[1], "check") == 0) {
        /* policy check <kind> <target> */
        aios_act_kind kind = ACT_TOOL;
        const char *target = a->n >= 4 ? a->v[3] : (a->n >= 3 ? a->v[2] : NULL);
        if (a->n >= 3 && !target) { snprintf(err, ecap, "policy check needs a target"); return 1; }
        const char *k = a->n >= 4 ? a->v[2] : "tool";
        if (strcmp(k, "agent") == 0) kind = ACT_AGENT;
        else if (strcmp(k, "shell") == 0) kind = ACT_SHELL;
        else if (strcmp(k, "delete") == 0) kind = ACT_FS_DELETE;
        else if (strcmp(k, "spawn") == 0) kind = ACT_JOB_SPAWN;
        else if (strcmp(k, "write") == 0) kind = ACT_FS_WRITE;

        aios_action *act = aios_action_new(kind, ORIGIN_AI, target);
        if (!act) { snprintf(err, ecap, "cannot build a descriptor"); return 1; }
        aios_action_set_intent(act, "policy check");
        aios_policy_ctx pctx = aios_policy_ctx_default();
        pctx.sandbox_executed = (kind == ACT_TOOL || kind == ACT_AGENT);
        aios_decision d = aios_policy_eval(act, sh->jail, SH_CAPS, SH_N_CAPS,
                                           &pctx, aios_now_ms());
        char line[512];
        snprintf(line, sizeof line, "verdict: %s [%s]\n  %s\n",
                 aios_policy_verdict_str(d.v), aios_policy_reason_str(d.r), d.why);
        sb_puts(&b, line);
        /* now the same thing as a human, which is the interesting comparison */
        aios_action *h = aios_action_new(kind, ORIGIN_HUMAN, target);
        if (h) {
            aios_decision hd = aios_policy_eval(h, sh->jail, NULL, 0, &pctx,
                                                aios_now_ms());
            snprintf(line, sizeof line, "as human: %s [%s]\n",
                     aios_policy_verdict_str(hd.v),
                     aios_policy_reason_str(hd.r));
            sb_puts(&b, line);
            aios_action_free(h);
        }
        aios_action_free(act);
        *text_out = b.s ? b.s : strdup("");
        return 0;
    }

    sb_puts(&b, "AI-OS law table (compiled in; no file, no override)\n");
    sb_puts(&b, "tier 0 = constitutional veto (binds the human too)\n");
    sb_puts(&b, "tier 1 = critical (machine actions stopped or sent to a human)\n\n");
    size_t n = aios_policy_rule_count();
    for (size_t i = 0; i < n; i++) {
        const char *law = NULL;
        aios_reason r = R_OK;
        int tier = 0;
        if (!aios_policy_rule_at(i, &law, &tier, &r)) continue;
        char line[512];
        snprintf(line, sizeof line, "  [%d] %-62.62s (%s)\n",
                 tier, law ? law : "?", aios_policy_reason_str(r));
        sb_puts(&b, line);
    }
    sb_puts(&b, "\ncapabilities granted to this shell's AI:\n  ");
    for (int i = 0; i < SH_N_CAPS; i++) {
        sb_puts(&b, SH_CAPS[i]);
        sb_puts(&b, i + 1 < SH_N_CAPS ? ", " : "\n");
    }
    *text_out = b.s ? b.s : strdup("");
    (void)json_out;
    return 0;
}

/* ---- M3: the human half of staging ---- */

/* Find the most recent staging session in the store. `promote` with no argument
 * means "the thing I was just shown", which is what a human will type. */
static bool sh_latest_stage(sh_shell *sh, char *out, size_t ocap) {
    char staging[600];
    snprintf(staging, sizeof staging, "%s/staging", sh->store);
    DIR *d = opendir(staging);
    if (!d) return false;

    char best[512] = {0};
    long long best_mtime = -1;
    /* `staging` may be up to 600 bytes and `best` is 512: we copy only what
     * fits and refuse a session dir we could not name completely. */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char dir[700];
        int dn = snprintf(dir, sizeof dir, "%s/%s", staging, de->d_name);
        if (dn < 0 || (size_t)dn >= sizeof dir) continue;
        if (stage_manifest_state(dir) == STAGE_MANIFEST_NONE) continue;
        if (stage_manifest_state(dir) == STAGE_MANIFEST_DONE) continue;
        struct stat sst;
        if (stat(dir, &sst) != 0 || !S_ISDIR(sst.st_mode)) continue;
        if ((long long)sst.st_mtime >= best_mtime) {
            best_mtime = (long long)sst.st_mtime;
            if (strlen(dir) < sizeof best) memcpy(best, dir, strlen(dir) + 1);
        }
    }
    closedir(d);
    if (!best[0] || strlen(best) >= ocap) return false;
    memcpy(out, best, strlen(best) + 1);
    return true;
}

static int builtin_staged(sh_shell *sh, char **text_out, json_val **json_out,
                          char *err, size_t ecap) {
    char dir[600];
    if (sh->stage_live && sh->stage_dir[0]) {
        if (strlen(sh->stage_dir) >= sizeof dir) { *text_out = strdup(""); return 1; }
        memcpy(dir, sh->stage_dir, strlen(sh->stage_dir) + 1);
    } else if (!sh_latest_stage(sh, dir, sizeof dir)) {
        *text_out = strdup("nothing is staged\n");
        (void)json_out;
        return 0;
    }
    stage_session s;
    if (!stage_open(dir, &s, err, ecap)) return 1;
    stage_diff d;
    if (!stage_diff_compute(&s, &d, err, ecap)) { stage_end(&s); return 1; }
    char *body = stage_diff_text(&d);
    *text_out = body;
    stage_diff_free(&d);
    stage_end(&s);
    (void)json_out;
    return 0;
}

static int builtin_promote(sh_shell *sh, sargv *a, char **text_out,
                           json_val **json_out, char *err, size_t ecap) {
    char dir[600];
    if (a->n >= 2) {
        snprintf(dir, sizeof dir, "%s", a->v[1]);
    } else if (!sh_latest_stage(sh, dir, sizeof dir)) {
        snprintf(err, ecap, "nothing is staged to promote");
        return 1;
    }
    stage_session s;
    if (!stage_open(dir, &s, err, ecap)) return 1;
    size_t n = 0;
    /* A human typed `promote`: that makes the change human-sponsored, which is
     * exactly what the policy layer wanted before it would let it through. The
     * staging record is the accountability link between the AI that proposed
     * the change and the person who accepted it. */
    bool ok = stage_promote_locked(&s, &n, err, ecap);
    if (ok) {
        char line[256];
        snprintf(line, sizeof line, "promoted %zu file%s into the jail\n",
                 n, n == 1 ? "" : "s");
        *text_out = strdup(line);
        if (sh->bus) {
            char ev[512];
            snprintf(ev, sizeof ev, "{\"files\":%zu,\"dir\":\"%.300s\"}", n, s.dir);
            sembus_pub(sh->bus, "sh.stage.promote", ev);
        }
    }
    stage_end(&s);
    (void)json_out;
    return ok ? 0 : 1;
}

static int builtin_discard(sh_shell *sh, sargv *a, char **text_out,
                           json_val **json_out, char *err, size_t ecap) {
    char dir[600];
    if (a->n >= 2) {
        snprintf(dir, sizeof dir, "%s", a->v[1]);
    } else if (!sh_latest_stage(sh, dir, sizeof dir)) {
        snprintf(err, ecap, "nothing is staged to discard");
        return 1;
    }
    stage_session s;
    if (!stage_open(dir, &s, err, ecap)) return 1;
    bool ok = stage_discard(&s, err, ecap);
    if (ok) *text_out = strdup("discarded; the jail was never touched\n");
    stage_end(&s);
    (void)json_out;
    return ok ? 0 : 1;
}

static int builtin_gc(sh_shell *sh, char **text_out, json_val **json_out,
                      char *err, size_t ecap) {
    stage_gc_result r;
    if (!stage_gc(sh->store, (long long)aios_now_ms(), 3600LL * 1000, &r,
                  err, ecap))
        return 1;
    char line[192];
    snprintf(line, sizeof line, "gc: removed %zu stale session%s, kept %zu\n",
             r.removed, r.removed == 1 ? "" : "s", r.kept);
    *text_out = strdup(line);
    (void)json_out;
    return 0;
}

static int builtin_version(sh_shell *sh, char **text_out, json_val **json_out) {
    *text_out = strdup("AI-OS " AIOS_VERSION " — aiosh (v1.0)");
    json_val *o = json_new_obj();
    json_obj_set(o, "version",  json_new_str(AIOS_VERSION));
    json_obj_set(o, "milestone", json_new_str("v1.0"));
    json_obj_set(o, "component", json_new_str("aiosh"));
    json_obj_set(o, "cortex",   json_new_str(sh->cortex_mode));
    *json_out = o;
    return 0;
}

static int builtin_count(sargv *a, const char *pipe_text,
                         char **text_out, json_val **json_out,
                         char *err, size_t ecap) {
    if (!pipe_text) {
        snprintf(err, ecap, "count needs piped input (try: fs.read notes.txt | count)");
        return 1;
    }
    int mode = 0; /* 0 lines, 1 words, 2 chars */
    for (size_t i = 1; i < a->n; i++) {
        if (strcmp(a->v[i], "-w") == 0) mode = 1;
        else if (strcmp(a->v[i], "-c") == 0) mode = 2;
        else { snprintf(err, ecap, "count: unknown flag %s (use -w or -c)", a->v[i]); return 1; }
    }
    long n = 0;
    if (mode == 2) n = (long)strlen(pipe_text);
    else if (mode == 1) {
        bool inword = false;
        for (const char *p = pipe_text; *p; p++) {
            if (isspace((unsigned char)*p)) inword = false;
            else if (!inword) { inword = true; n++; }
        }
    } else {
        lineiter it = { pipe_text, 0 };
        const char *s; size_t l;
        while (next_line(&it, &s, &l)) n++;
    }
    char b[32];
    snprintf(b, sizeof b, "%ld", n);
    *text_out = strdup(b);
    *json_out = json_new_num((double)n);
    return 0;
}

static int builtin_grep(sargv *a, const char *pipe_text,
                        char **text_out, char *err, size_t ecap) {
    if (!pipe_text) { snprintf(err, ecap, "grep needs piped input"); return 1; }
    if (a->n < 2)   { snprintf(err, ecap, "grep needs a pattern"); return 1; }
    if (a->n > 2)   { snprintf(err, ecap, "grep takes one pattern"); return 1; }
    sb b = {0};
    lineiter it = { pipe_text, 0 };
    const char *s; size_t l;
    while (next_line(&it, &s, &l)) {
        char buf[2048];
        snprintf(buf, sizeof buf, "%.*s", (int)(l > 2047 ? 2047 : l), s);
        if (strstr(buf, a->v[1])) {
            if (b.n) sb_putc(&b, '\n');
            sb_putn(&b, s, l);
        }
    }
    *text_out = b.s ? b.s : strdup("");
    return 0;
}

static int builtin_headtail(sargv *a, const char *pipe_text, bool head,
                            char **text_out, char *err, size_t ecap) {
    if (!pipe_text) { snprintf(err, ecap, "%s needs piped input", head ? "head" : "tail"); return 1; }
    long n = 10;
    if (a->n > 2) { snprintf(err, ecap, "%s takes at most one count", head ? "head" : "tail"); return 1; }
    if (a->n == 2) {
        char *end = NULL;
        n = strtol(a->v[1], &end, 10);
        if (!end || *end || n < 0) { snprintf(err, ecap, "%s: bad count", head ? "head" : "tail"); return 1; }
    }
    /* collect all lines */
    const char *starts[256]; size_t lens[256];
    size_t total = 0;
    lineiter it = { pipe_text, 0 };
    const char *s; size_t l;
    while (next_line(&it, &s, &l) && total < 256) { starts[total] = s; lens[total] = l; total++; }
    size_t un = n < 0 ? 0 : (size_t)n;
    size_t from = head ? 0 : (total > un ? total - un : 0);
    size_t to   = head ? (un < total ? un : total) : total;
    sb b = {0};
    for (size_t i = from; i < to; i++) {
        if (b.n) sb_putc(&b, '\n');
        sb_putn(&b, starts[i], lens[i]);
    }
    *text_out = b.s ? b.s : strdup("");
    return 0;
}

static int builtin_remember(sh_shell *sh, sargv *a, const char *pipe_text,
                            char **text_out, char *err, size_t ecap) {
    char *text = a->n > 1 ? join_args(a->v, a->n, 1) : xstrdup(pipe_text);
    if (!text || !*text) { free(text); snprintf(err, ecap, "remember needs text or piped input"); return 1; }
    aios_status st = mem_remember(sh->mem, MEM_EPISODIC, text, NULL);
    if (st != AIOS_OK) { snprintf(err, ecap, "remember failed: %s", aios_status_str(st)); free(text); return 1; }
    char b[256];
    snprintf(b, sizeof b, "remembered: %.230s", text);
    *text_out = strdup(b);
    free(text);
    return 0;
}

static int builtin_recall(sh_shell *sh, sargv *a, const char *pipe_text,
                          char **text_out, json_val **json_out,
                          char *err, size_t ecap) {
    char *query = a->n > 1 ? join_args(a->v, a->n, 1) : xstrdup(pipe_text);
    if (!query || !*query) { free(query); snprintf(err, ecap, "recall needs a query or piped input"); return 1; }
    mem_entry *hits[5];
    int n = mem_recall(sh->mem, query, 5, hits);
    sb b = {0};
    json_val *arr = json_new_arr();
    for (int i = 0; i < n; i++) {
        char line[512];
        snprintf(line, sizeof line, "[%.2f] %-8s %.420s",
                 hits[i]->score, tier_name(hits[i]->tier), hits[i]->text);
        if (b.n) sb_putc(&b, '\n');
        sb_puts(&b, line);
        json_val *h = json_new_obj();
        json_obj_set(h, "text",  json_new_str(hits[i]->text));
        json_obj_set(h, "tier",  json_new_str(tier_name(hits[i]->tier)));
        json_obj_set(h, "score", json_new_num(hits[i]->score));
        json_arr_push(arr, h);
    }
    *text_out = b.s ? b.s : strdup("(no matching memories)");
    *json_out = arr;
    free(query);
    return 0;
}

static int builtin_emit(sh_shell *sh, sargv *a,
                        char **text_out, char *err, size_t ecap) {
    if (a->n < 2) { snprintf(err, ecap, "emit needs a topic"); return 1; }
    char *payload = a->n > 2 ? join_args(a->v, a->n, 2) : strdup("null");
    if (payload[0]) {
        json_val *j = json_parse(payload, strlen(payload));
        if (!j) { snprintf(err, ecap, "emit payload must be valid JSON"); free(payload); return 1; }
        json_free(j);
    }
    int n = sembus_pub(sh->bus, a->v[1], payload);
    char b[256];
    snprintf(b, sizeof b, "published to %s (%d subscriber%s)", a->v[1], n, n == 1 ? "" : "s");
    *text_out = strdup(b);
    free(payload);
    return 0;
}

static int builtin_set(sh_shell *sh, sargv *a, char **text_out,
                       char *err, size_t ecap) {
    if (a->n < 2) { snprintf(err, ecap, "set needs NAME [VALUE...]"); return 1; }
    const char *name = a->v[1];
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') {
        snprintf(err, ecap, "set: invalid name \"%s\"", name); return 1;
    }
    for (const char *q = name + 1; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '_') {
            snprintf(err, ecap, "set: invalid name \"%s\"", name); return 1;
        }
    char *val = a->n > 2 ? join_args(a->v, a->n, 2) : strdup("");
    for (int i = 0; i < sh->nvars; i++)
        if (strcmp(sh->var_name[i], name) == 0) {
            free(sh->var_val[i]);
            sh->var_val[i] = val;
            char b[1024];
            snprintf(b, sizeof b, "%s=%s", name, val);
            *text_out = strdup(b);
            return 0;
        }
    if (sh->nvars >= SH_MAX_VARS) { free(val); snprintf(err, ecap, "variable table full"); return 1; }
    sh->var_name[sh->nvars] = strdup(name);
    sh->var_val[sh->nvars] = val;
    sh->nvars++;
    char b[1024];
    snprintf(b, sizeof b, "%s=%s", name, val);
    *text_out = strdup(b);
    return 0;
}

static int builtin_unset(sh_shell *sh, sargv *a, char **text_out,
                         char *err, size_t ecap) {
    if (a->n != 2) { snprintf(err, ecap, "unset needs exactly NAME"); return 1; }
    for (int i = 0; i < sh->nvars; i++)
        if (strcmp(sh->var_name[i], a->v[1]) == 0) {
            free(sh->var_name[i]); free(sh->var_val[i]);
            for (int k = i; k < sh->nvars - 1; k++) {
                sh->var_name[k] = sh->var_name[k+1];
                sh->var_val[k]  = sh->var_val[k+1];
            }
            sh->nvars--;
            *text_out = strdup("unset");
            return 0;
        }
    snprintf(err, ecap, "unset: no variable \"%s\"", a->v[1]);
    return 1;
}

static int builtin_vars(sh_shell *sh, char **text_out) {
    if (sh->nvars == 0) { *text_out = strdup("(no variables)"); return 0; }
    sb b = {0};
    for (int i = 0; i < sh->nvars; i++) {
        if (b.n) sb_putc(&b, '\n');
        sb_puts(&b, sh->var_name[i]);
        sb_putc(&b, '=');
        sb_puts(&b, sh->var_val[i]);
    }
    *text_out = b.s ? b.s : strdup("");
    return 0;
}

static int builtin_history(sh_shell *sh, char **text_out) {
    if (sh->hist_n == 0) { *text_out = strdup("(no history)"); return 0; }
    sb b = {0};
    for (int i = 0; i < sh->hist_n; i++) {
        if (b.n) sb_putc(&b, '\n');
        char num[16];
        snprintf(num, sizeof num, "%3d  ", i + 1);
        sb_puts(&b, num);
        sb_puts(&b, sh->hist[i]);
    }
    *text_out = b.s ? b.s : strdup("");
    return 0;
}

static int builtin_cortex(sh_shell *sh, sargv *a, char **text_out,
                          char *err, size_t ecap) {
    if (a->n == 1) {
        char b[128];
        snprintf(b, sizeof b, "cortex: %s", sh->cortex_mode);
        *text_out = strdup(b);
        return 0;
    }
    if (a->n != 2 ||
        (strcmp(a->v[1], "scripted") != 0 && strcmp(a->v[1], "micro") != 0 &&
         strcmp(a->v[1], "llm") != 0)) {
        snprintf(err, ecap, "cortex: use 'scripted', 'micro' or 'llm'");
        return 1;
    }
    snprintf(sh->cortex_mode, sizeof sh->cortex_mode, "%s", a->v[1]);
    char b[128];
    snprintf(b, sizeof b, "cortex set to %s", sh->cortex_mode);
    *text_out = strdup(b);
    return 0;
}

/* ---------------- M7: micro-lm / autopilot / tune ---------------- */

static int builtin_lm(sh_shell *sh, sargv *a,
                      char **text_out, char *err, size_t ecap) {
    (void)sh; (void)err; (void)ecap;
    char b[1024];
    if (a->n >= 2 && strcmp(a->v[1], "classify") == 0) {
        /* join the rest into one line (quoted words kept apart) */
        char line[768] = {0};
        size_t off = 0;
        for (size_t i = 2; i < a->n && off < sizeof line - 2; i++) {
            if (off) line[off++] = ' ';
            size_t l = strlen(a->v[i]);
            if (l > sizeof line - off - 1) l = sizeof line - off - 1;
            memcpy(line + off, a->v[i], l);
            off += l;
        }
        line[off] = 0;
        double conf = 0.0;
        aios_lm_class c = aios_microlm_classify(line, &conf);
        snprintf(b, sizeof b, "lm: class=%s conf=%.2f (micro-lm)%s%s",
                 aios_microlm_class_name(c), conf,
                 off ? " text=\"" : "", off ? line : "");
        *text_out = strdup(b);
        return 0;
    }
    if (a->n >= 2 && strcmp(a->v[1], "gen") == 0) {
        char prompt[512] = {0};
        size_t off = 0;
        for (size_t i = 2; i < a->n && off < sizeof prompt - 2; i++) {
            if (off) prompt[off++] = ' ';
            size_t l = strlen(a->v[i]);
            if (l > sizeof prompt - off - 1) l = sizeof prompt - off - 1;
            memcpy(prompt + off, a->v[i], l);
            off += l;
        }
        prompt[off] = 0;
        if (aios_microlm_generate(prompt, b, sizeof b) <= 0) {
            snprintf(err, ecap, "lm: generation failed");
            return 1;
        }
        *text_out = strdup(b);
        return 0;
    }
    if (a->n >= 2 && strcmp(a->v[1], "selftest") == 0) {
        int fails = aios_microlm_selftest();
        snprintf(b, sizeof b, "lm selftest: %s (%d phrases, %d failures)",
                 fails ? "FAIL" : "PASS",
                 fails ? fails : 18, fails);
        *text_out = strdup(b);
        return fails ? 1 : 0;
    }
    aios_microlm_stats(b, sizeof b);
    *text_out = strdup(b);
    return 0;
}

static int builtin_autopilot(sh_shell *sh, sargv *a,
                             char **text_out, char *err, size_t ecap) {
    (void)sh; (void)a; (void)err; (void)ecap;
    sb b = {0};
    sb_putn(&b, "", 0);
    json_val *st = json_parse_file(AIOS_AP_PATH_STATE);
    if (!st) {
        char line[256];
        snprintf(line, sizeof line,
                 "autopilot: governor not running "
                 "(state file %s absent — started by init unless "
                 "aios.autopilot=off)", AIOS_AP_PATH_STATE);
        *text_out = strdup(line);
        return 0;
    }
    const char *mode = json_str(json_obj_get(st, "mode"));
    json_val *tick  = json_obj_get(st, "tick");
    json_val *reg   = json_obj_get(st, "registry");
    json_val *reason= json_obj_get(st, "reason");
    json_val *adjt  = json_obj_get(st, "adjustments_total");
    char line[512];
    snprintf(line, sizeof line, "autopilot: mode=%s governor-ticks=%.0f "
             "adjustments=%.0f registry=%.0f",
             mode ? mode : "?",
             tick ? json_num(tick) : 0,
             adjt ? json_num(adjt) : 0,
             reg ? json_num(reg) : -1);
    sb_putn(&b, line, strlen(line));
    sb_putc(&b, '\n');
    int lt = 0, la = 0;
    aios_cgroup_budget_get(AIOS_KS_BATCH, &lt);
    aios_cgroup_budget_get(AIOS_KS_IDLE, &la);
    snprintf(line, sizeof line,
             "  live budgets: tools=%d%% agents=%d%% (cgroup cpu.max)\n",
             lt, la);
    sb_putn(&b, line, strlen(line));
    /* per-class wakeup/util lines from the flattened state keys */
    static const char *const cn_names[3] =
        { "interactive", "batch", "idle" };
    static const char *const wake_keys[3] =
        { "wakeups.interactive", "wakeups.batch", "wakeups.idle" };
    static const char *const util_keys[3] =
        { "util.interactive", "util.batch", "util.idle" };
    for (int c = 0; c < 3; c++) {
        json_val *w = json_obj_get(st, wake_keys[c]);
        json_val *u = json_obj_get(st, util_keys[c]);
        snprintf(line, sizeof line, "  %-11s wakeups=%-.0f util=%-.1f%%\n",
                 cn_names[c], w ? json_num(w) : 0, u ? json_num(u) : 0);
        sb_putn(&b, line, strlen(line));
    }
    snprintf(line, sizeof line, "  last decision: %s\n",
             reason && json_str(reason) ? json_str(reason) : "(none)");
    sb_putn(&b, line, strlen(line));
    json_free(st);
    *text_out = b.s ? b.s : strdup("autopilot:");
    return 0;
}

static int builtin_tune(sh_shell *sh, sargv *a,
                        char **text_out, char *err, size_t ecap) {
    (void)sh;
    aios_ap_conf cf;
    aios_ap_conf_defaults(&cf);
    aios_ap_conf_load(AIOS_AP_PATH_CONF, &cf); /* overlay existing */
    if (a->n == 1) {
        char b[512];
        snprintf(b, sizeof b,
                 "autopilot conf (%s): mode=%s tools=%d agents=%d "
                 "tick_ms=%d — governor re-reads it every tick",
                 AIOS_AP_PATH_CONF, aios_ap_mode_name(cf.mode),
                 cf.tools_base, cf.agents_base, cf.tick_ms);
        *text_out = strdup(b);
        return 0;
    }
    for (size_t i = 1; i < a->n; i++) {
        char *eq = strchr(a->v[i], '=');
        if (!eq) {
            snprintf(err, ecap, "tune: expected key=value, got '%s'", a->v[i]);
            return 1;
        }
        *eq = 0;
        const char *k = a->v[i], *v = eq + 1;
        if (strcmp(k, "mode") == 0) {
            int m = aios_ap_mode_parse(v);
            if (m < 0) {
                snprintf(err, ecap, "tune: mode must be off|static|"
                         "balanced|aggressive");
                return 1;
            }
            cf.mode = (aios_ap_mode)m;
        } else if (strcmp(k, "tools") == 0 || strcmp(k, "agents") == 0) {
            int n = atoi(v);
            if (n < 1 || n > 100) {
                snprintf(err, ecap, "tune: %s must be 1..100", k);
                return 1;
            }
            if (*k == 't') cf.tools_base = n; else cf.agents_base = n;
        } else if (strcmp(k, "tick_ms") == 0) {
            int n = atoi(v);
            if (n < 100 || n > 60000) {
                snprintf(err, ecap, "tune: tick_ms must be 100..60000");
                return 1;
            }
            cf.tick_ms = n;
        } else {
            snprintf(err, ecap, "tune: unknown key '%s' (mode, tools, "
                     "agents, tick_ms)", k);
            return 1;
        }
        *eq = '='; /* sargv strings are shared; restore */
    }
    if (aios_ap_conf_save(AIOS_AP_PATH_CONF, &cf) != 0) {
        snprintf(err, ecap, "tune: cannot write %s", AIOS_AP_PATH_CONF);
        return 1;
    }
    /* M8 durable: the tuned conf survives reboots (governor mirrors it
     * back through init's restore step next boot). */
    aios_dur_mirror(AIOS_AP_PATH_CONF, "var/autopilot.conf");
    char b[512];
    snprintf(b, sizeof b,
             "autopilot conf saved: mode=%s tools=%d agents=%d tick_ms=%d "
             "— the governor applies it within one tick",
             aios_ap_mode_name(cf.mode), cf.tools_base, cf.agents_base,
             cf.tick_ms);
    *text_out = strdup(b);
    return 0;
}

static int builtin_jail(sh_shell *sh, sargv *a, char **text_out,
                        char *err, size_t ecap) {
    if (a->n == 1) { *text_out = strdup(sh->jail); return 0; }
    if (a->n != 2) { snprintf(err, ecap, "jail takes one directory"); return 1; }
    tool_reg *nt = tool_reg_create(a->v[1], sh->store, "sh", sh->mem);
    if (!nt) { snprintf(err, ecap, "cannot rebind jail to %s", a->v[1]); return 1; }
    tool_reg_install_default(nt);
    tool_reg_free(sh->tools);
    sh->tools = nt;
    snprintf(sh->jail, sizeof sh->jail, "%s", a->v[1]);
    char b[640];
    snprintf(b, sizeof b, "jail set to %s", sh->jail);
    *text_out = strdup(b);
    return 0;
}

static int builtin_store(sh_shell *sh, sargv *a, char **text_out,
                         char *err, size_t ecap) {
    if (a->n == 1) { *text_out = strdup(sh->store); return 0; }
    if (a->n != 2) { snprintf(err, ecap, "store takes one directory"); return 1; }
    mem_ctx *nm = mem_open(a->v[1]);
    if (!nm) { snprintf(err, ecap, "cannot open store %s", a->v[1]); return 1; }
    tool_reg *nt = tool_reg_create(sh->jail, a->v[1], "sh", nm);
    if (!nt) { mem_close(nm); snprintf(err, ecap, "cannot rebind store to %s", a->v[1]); return 1; }
    tool_reg_install_default(nt);
    tool_reg_free(sh->tools);
    mem_close(sh->mem);
    sh->tools = nt;
    sh->mem = nm;
    snprintf(sh->store, sizeof sh->store, "%s", a->v[1]);
    char b[640];
    snprintf(b, sizeof b, "store set to %s", sh->store);
    *text_out = strdup(b);
    return 0;
}

/* ---------------- jobs builtins ---------------- */

void sh_poll_jobs(sh_shell *sh) {
    if (!sh) return;
    for (int i = 0; i < sh->njobs; i++) {
        sh_job *j = &sh->jobs[i];
        if (j->state != JOB_RUNNING) continue;
        int wst = 0;
        pid_t r = waitpid(j->pid, &wst, WNOHANG);
        if (r == 0) continue;
        if (r < 0) { j->state = JOB_FAILED; continue; }
        j->state = (WIFEXITED(wst) && WEXITSTATUS(wst) == 0)
                   ? JOB_DONE : JOB_FAILED;
        jobs_journal(sh->store, "exit", j->id, NULL,
                     job_state_name(j->state));        /* M8 durable */
        char path[1024];
        snprintf(path, sizeof path, "%s/jobs/%ld.json", sh->store, j->id);
        json_val *o = json_parse_file(path);
        if (o) {
            const char *f = json_str(json_obj_get(o, "final"));
            if (f) j->final = strdup(f);
            json_free(o);
        }
    }
}

size_t sh_jobs_alive(sh_shell *sh) {
    if (!sh) return 0;
    size_t n = 0;
    for (int i = 0; i < sh->njobs; i++)
        if (sh->jobs[i].state == JOB_RUNNING) n++;
    return n;
}

/* ---------------- M8 durable job journal ---------------- */

/* append one fsync'd JSONL record to <store>/jobs.jsonl (works on the
 * ext2 side disk or plain tmpfs alike; fsync is what makes replays safe) */
static void jobs_journal(const char *store, const char *op, long id,
                         const char *goal, const char *state) {
    char path[1024];
    snprintf(path, sizeof path, "%s/jobs.jsonl", store);
    json_val *o = json_new_obj();
    if (!o) return;
    json_obj_set(o, "op", json_new_str(op));
    json_obj_set(o, "id", json_new_num((double)id));
    if (goal)  json_obj_set(o, "goal", json_new_str(goal));
    if (state) json_obj_set(o, "state", json_new_str(state));
    json_obj_set(o, "ts_ms", json_new_num(aios_now_ms()));
    char *line = json_dump(o);
    json_free(o);
    if (!line) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!write(fd, line, strlen(line));
        (void)!write(fd, "\n", 1);
        fsync(fd);
        close(fd);
    }
    free(line);
}

static int job_state_from_name(const char *s) {
    if (!s) return JOB_FAILED;
    if (strcmp(s, "done") == 0)   return JOB_DONE;
    if (strcmp(s, "killed") == 0) return JOB_KILLED;
    return JOB_FAILED;
}

/* replay <store>/jobs.jsonl into a fresh shell's job table. Only lines
 * terminated by '\n' are trusted — a torn tail means the previous session
 * died mid-append and that record is dropped. Final texts are pulled
 * from the per-job result documents (<store>/jobs/<id>.json). */
static void jobs_restore(sh_shell *sh) {
    char path[1024];
    snprintf(path, sizeof path, "%s/jobs.jsonl", sh->store);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char buf[1 << 18];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    long max_id = 0;
    char *pos = buf;
    while (pos && *pos) {
        char *nl = strchr(pos, '\n');
        if (!nl) break;                       /* torn tail — drop */
        *nl = 0;
        json_val *o = json_parse(pos, strlen(pos));
        pos = nl + 1;
        if (!o) continue;
        const char *op = json_str(json_obj_get(o, "op"));
        long id = (long)json_num(json_obj_get(o, "id"));
        if (!op || id <= 0) { json_free(o); continue; }
        if (id > max_id) max_id = id;
        if (strcmp(op, "spawn") == 0) {
            if (sh->njobs >= SH_MAX_JOBS) {   /* keep the most recent */
                free(sh->jobs[0].goal);
                free(sh->jobs[0].final);
                memmove(&sh->jobs[0], &sh->jobs[1],
                        sizeof(sh_job) * (size_t)(sh->njobs - 1));
                sh->njobs--;
            }
            sh_job *j = &sh->jobs[sh->njobs++];
            memset(j, 0, sizeof *j);
            j->id = id;
            const char *g = json_str(json_obj_get(o, "goal"));
            j->goal = strdup(g ? g : "");
            j->state = JOB_FAILED;    /* running when the power went */
        } else if (strcmp(op, "exit") == 0) {
            for (int i = sh->njobs - 1; i >= 0; i--) {
                if (sh->jobs[i].id == id) {
                    sh->jobs[i].state =
                        job_state_from_name(json_str(json_obj_get(o, "state")));
                    break;
                }
            }
        }
        json_free(o);
    }

    /* final texts from the per-job result documents */
    for (int i = 0; i < sh->njobs; i++) {
        sh_job *j = &sh->jobs[i];
        snprintf(path, sizeof path, "%s/jobs/%ld.json", sh->store, j->id);
        json_val *o = json_parse_file(path);
        if (o) {
            const char *fin = json_str(json_obj_get(o, "final"));
            if (fin) j->final = strdup(fin);
            json_free(o);
        }
    }
    sh->next_job_id = max_id + 1;
}

/* M8 durable: `dur` — durable-store status on the ext2 side disk. */
static int builtin_dur(sh_shell *sh, sargv *a, char **text_out,
                       char *err, size_t ecap) {
    (void)err; (void)ecap;
    if (a->n == 2 && strcmp(a->v[1], "sync") == 0) {
        sync();
        *text_out = strdup("durable: sync done (all buffers flushed)");
        return 0;
    }
    sb b = {0};
    if (!aios_dur_available()) {
        sb_puts(&b, "durable: no durable disk — RAM-only session "
                    "(state dies at power-off)");
        *text_out = b.s ? b.s : strdup("");
        return 0;
    }
    size_t mem_n = 0, audit_n = 0, boots = 0, apn = 0;
    char lastap[256] = {0}, line[512];
    aios_dur_lines("store/memory.journal", &mem_n, NULL, 0);
    aios_dur_lines("store/audit.log", &audit_n, NULL, 0);
    aios_dur_lines("boots.jsonl", &boots, NULL, 0);
    aios_dur_lines("var/autopilot.jsonl", &apn, lastap, sizeof lastap);

    /* spawn-record count from the durable job journal */
    static char jbuf[1 << 18];
    size_t jlen = 0, spawns = 0, jrecs = 0;
    if (aios_dur_read("store/jobs.jsonl", jbuf, sizeof jbuf - 1, &jlen) == AIOS_OK) {
        jbuf[jlen] = 0;
        const char *p = jbuf;
        while ((p = strstr(p, "\"op\":\"spawn\"")) != NULL) {
            spawns++;
            p += 5;
        }
        for (size_t i = 0; i < jlen; i++)
            if (jbuf[i] == '\n') jrecs++;
    }

    snprintf(line, sizeof line, "durable: %s (ext2 side disk, %zu boots)%s",
             aios_dur_root(), boots,
             strncmp(sh->store, aios_dur_root(), strlen(aios_dur_root())) == 0
                 ? " — this shell's store is on it" : "");
    sb_puts(&b, line);
    snprintf(line, sizeof line, "\n  memory:     %zu entries", mem_n);
    sb_puts(&b, line);
    snprintf(line, sizeof line, "\n  audit:      %zu tool calls", audit_n);
    sb_puts(&b, line);
    snprintf(line, sizeof line, "\n  jobs:       %zu spawned / %zu journal records",
             spawns, jrecs);
    sb_puts(&b, line);
    if (apn > 0) {
        snprintf(line, sizeof line, "\n  autopilot:  %zu adjustments (last: %.200s)",
                 apn, lastap);
    } else {
        snprintf(line, sizeof line, "\n  autopilot:  no journal yet");
    }
    sb_puts(&b, line);
    *text_out = b.s ? b.s : strdup("");
    return 0;
}

static int spawn_job(sh_shell *sh, const char *goal,
                     char **text_out, char *err, size_t ecap) {
    if (!*goal) { snprintf(err, ecap, "spawn needs a goal"); return 1; }
    if (sh->njobs >= SH_MAX_JOBS) { snprintf(err, ecap, "job table full"); return 1; }
    char jd[600];
    snprintf(jd, sizeof jd, "%s/jobs", sh->store);
    if (aios_mkdirs(jd) != AIOS_OK) { snprintf(err, ecap, "cannot create %s", jd); return 1; }
    char *goal_copy = strdup(goal);
    if (!goal_copy) { snprintf(err, ecap, "oom"); return 1; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { free(goal_copy); snprintf(err, ecap, "fork: %s", strerror(errno)); return 1; }
    if (pid == 0) {
        long id = sh->next_job_id;
        char mode[16];
        snprintf(mode, sizeof mode, "%s", sh->cortex_mode);
        char store[512], jail[512];
        snprintf(store, sizeof store, "%s", sh->store);
        snprintf(jail, sizeof jail, "%s", sh->jail);
        int rc = job_child_main(store, jail, id, goal, mode);
        _exit(rc);
    }
    sh_job *j = &sh->jobs[sh->njobs++];
    j->id = sh->next_job_id++;
    j->pid = pid;
    j->goal = goal_copy;
    j->started_ms = aios_now_ms();
    j->state = JOB_RUNNING;
    j->final = NULL;
    jobs_journal(sh->store, "spawn", j->id, goal, NULL);   /* M8 durable */
    char idbuf[16];
    snprintf(idbuf, sizeof idbuf, "%ld", j->id);
    sembus_pub_kv(sh->bus, "sh.spawn",
                  (const char *const[]){"job", idbuf, "goal", goal, NULL});
    char b[640];
    snprintf(b, sizeof b, "[job %ld] spawned: %s", j->id, goal);
    *text_out = strdup(b);
    return 0;
}

static int builtin_spawn(sh_shell *sh, sargv *a,
                         char **text_out, char *err, size_t ecap) {
    char *goal = join_args(a->v, a->n, 1);
    int rc = spawn_job(sh, goal ? goal : "", text_out, err, ecap);
    free(goal);
    return rc;
}

static int builtin_jobs(sh_shell *sh, char **text_out) {
    sh_poll_jobs(sh);
    if (sh->njobs == 0) { *text_out = strdup("(no jobs)"); return 0; }
    sb b = {0};
    for (int i = 0; i < sh->njobs; i++) {
        sh_job *j = &sh->jobs[i];
        if (b.n) sb_putc(&b, '\n');
        char line[1024];
        if (j->final && (j->state == JOB_DONE))
            snprintf(line, sizeof line, "%3ld  %-8s %.380s -> %.400s",
                     j->id, job_state_name(j->state), j->goal, j->final);
        else if (j->state == JOB_FAILED && j->final)
            snprintf(line, sizeof line, "%3ld  %-8s %.380s -> %.400s",
                     j->id, job_state_name(j->state), j->goal, j->final);
        else
            snprintf(line, sizeof line, "%3ld  %-8s %.600s",
                     j->id, job_state_name(j->state), j->goal);
        sb_puts(&b, line);
    }
    *text_out = b.s ? b.s : strdup("");
    return 0;
}

static int builtin_wait(sh_shell *sh, sargv *a,
                        char **text_out, char *err, size_t ecap) {
    bool all = a->n == 1 || (a->n == 2 && strcmp(a->v[1], "all") == 0);
    long want = -1;
    if (!all) {
        if (a->n != 2) { snprintf(err, ecap, "wait [ID|all]"); return 1; }
        char *end = NULL;
        want = strtol(a->v[1], &end, 10);
        if (!end || *end) { snprintf(err, ecap, "wait: bad job id \"%s\"", a->v[1]); return 1; }
    }
    /* block until matching jobs finish (poll loop with deadline) */
    double deadline = aios_mono_ms() + SH_JOB_WAIT_MS;
    for (;;) {
        sh_poll_jobs(sh);
        bool pending = false;
        for (int i = 0; i < sh->njobs; i++)
            if (sh->jobs[i].state == JOB_RUNNING &&
                (all || sh->jobs[i].id == want)) pending = true;
        if (!pending) break;
        if (aios_mono_ms() > deadline) {
            snprintf(err, ecap, "wait: timed out after %.0fs", SH_JOB_WAIT_MS / 1000.0);
            return 1;
        }
        usleep(20000);
    }
    if (want >= 0) {
        for (int i = 0; i < sh->njobs; i++)
            if (sh->jobs[i].id == want) {
                sh_job *j = &sh->jobs[i];
                char b[1024];
                snprintf(b, sizeof b, "[job %ld] %s%s%s", j->id,
                         job_state_name(j->state),
                         j->final ? ": " : "", j->final ? j->final : "");
                *text_out = strdup(b);
                return j->state == JOB_DONE ? 0 : 1;
            }
        snprintf(err, ecap, "wait: no job %ld", want);
        return 1;
    }
    return builtin_jobs(sh, text_out);
}

static int builtin_kill(sh_shell *sh, sargv *a,
                        char **text_out, char *err, size_t ecap) {
    if (a->n != 2) { snprintf(err, ecap, "kill needs a job id"); return 1; }
    char *end = NULL;
    long want = strtol(a->v[1], &end, 10);
    if (!end || *end) { snprintf(err, ecap, "kill: bad job id \"%s\"", a->v[1]); return 1; }
    for (int i = 0; i < sh->njobs; i++) {
        sh_job *j = &sh->jobs[i];
        if (j->id != want) continue;
        if (j->state != JOB_RUNNING) {
            snprintf(err, ecap, "kill: job %ld already %s", j->id, job_state_name(j->state));
            return 1;
        }
        kill(j->pid, SIGKILL);
        for (int k = 0; k < 200; k++) {
            int wst = 0;
            if (waitpid(j->pid, &wst, WNOHANG) == j->pid) break;
            usleep(10000);
        }
        j->state = JOB_KILLED;
        char b[128];
        snprintf(b, sizeof b, "[job %ld] killed", j->id);
        *text_out = strdup(b);
        return 0;
    }
    snprintf(err, ecap, "kill: no job %ld", want);
    return 1;
}

/* ---------------- dispatch ---------------- */

static int run_command(sh_shell *sh, scmd *c,
                       const char *pipe_text, json_val *pipe_json,
                       char **text_out, json_val **json_out,
                       char *err, size_t ecap) {
    (void)pipe_json; /* stages exchange text; JSON rides the pipeline */
    sargv *a = &c->argv;
    *text_out = NULL;
    *json_out = NULL;
    if (a->n == 0) { snprintf(err, ecap, "empty command in pipeline"); return 1; }
    const char *c0 = a->v[0];

    /* ---- builtins ---- */
    if (strcmp(c0, "help") == 0)    { *text_out = strdup(HELP_TEXT); return 0; }
    if (strcmp(c0, "kernel") == 0) {
        char *st = (char *)malloc(8192);
        if (!st) { snprintf(err, ecap, "oom"); return 1; }
        aios_kernel_status(st, 8192);
        *text_out = st;
        return 0;
    }
    if (strcmp(c0, "version") == 0) return builtin_version(sh, text_out, json_out);
    if (strcmp(c0, "policy") == 0)
        return builtin_policy(sh, a, text_out, json_out, err, ecap);
    if (strcmp(c0, "echo") == 0)    { *text_out = join_args(a->v, a->n, 1); return 0; }
    if (strcmp(c0, "count") == 0)   return builtin_count(a, pipe_text, text_out, json_out, err, ecap);
    if (strcmp(c0, "grep") == 0)    return builtin_grep(a, pipe_text, text_out, err, ecap);
    if (strcmp(c0, "head") == 0)    return builtin_headtail(a, pipe_text, true,  text_out, err, ecap);
    if (strcmp(c0, "tail") == 0)    return builtin_headtail(a, pipe_text, false, text_out, err, ecap);
    if (strcmp(c0, "remember") == 0) return builtin_remember(sh, a, pipe_text, text_out, err, ecap);
    if (strcmp(c0, "recall") == 0)  return builtin_recall(sh, a, pipe_text, text_out, json_out, err, ecap);
    if (strcmp(c0, "emit") == 0)    return builtin_emit(sh, a, text_out, err, ecap);
    if (strcmp(c0, "set") == 0)     return builtin_set(sh, a, text_out, err, ecap);
    if (strcmp(c0, "unset") == 0)   return builtin_unset(sh, a, text_out, err, ecap);
    if (strcmp(c0, "vars") == 0)    return builtin_vars(sh, text_out);
    if (strcmp(c0, "history") == 0) return builtin_history(sh, text_out);
    if (strcmp(c0, "cortex") == 0)  return builtin_cortex(sh, a, text_out, err, ecap);
    if (strcmp(c0, "jail") == 0)    return builtin_jail(sh, a, text_out, err, ecap);
    if (strcmp(c0, "staged") == 0)  return builtin_staged(sh, text_out, json_out, err, ecap);
    if (strcmp(c0, "promote") == 0) return builtin_promote(sh, a, text_out, json_out, err, ecap);
    if (strcmp(c0, "discard") == 0) return builtin_discard(sh, a, text_out, json_out, err, ecap);
    if (strcmp(c0, "gc") == 0)      return builtin_gc(sh, text_out, json_out, err, ecap);
    if (strcmp(c0, "store") == 0)   return builtin_store(sh, a, text_out, err, ecap);
    if (strcmp(c0, "jobs") == 0)    return builtin_jobs(sh, text_out);
    if (strcmp(c0, "wait") == 0)    return builtin_wait(sh, a, text_out, err, ecap);
    if (strcmp(c0, "kill") == 0)    return builtin_kill(sh, a, text_out, err, ecap);
    if (strcmp(c0, "spawn") == 0)   return builtin_spawn(sh, a, text_out, err, ecap);
    if (strcmp(c0, "lm") == 0)      return builtin_lm(sh, a, text_out, err, ecap);
    if (strcmp(c0, "autopilot") == 0) return builtin_autopilot(sh, a, text_out, err, ecap);
    if (strcmp(c0, "tune") == 0)    return builtin_tune(sh, a, text_out, err, ecap);
    if (strcmp(c0, "dur") == 0)     return builtin_dur(sh, a, text_out, err, ecap);
    if (strcmp(c0, "agent") == 0) {
        char *goal = join_args(a->v, a->n, 1);
        if (!*goal) { free(goal); snprintf(err, ecap, "agent needs a goal"); return 1; }
        /* The human typed `agent <goal>`: they chose the goal, so they are
         * accountable for it. Still a descriptor - an agent always has one. */
        aios_action *act = aios_action_new(ACT_AGENT, ORIGIN_HUMAN, goal);
        int rc;
        if (!act) {
            snprintf(err, ecap, "oom (descriptor)");
            rc = 1;
        } else {
            aios_action_set_intent(act, c->raw ? c->raw : goal);
            rc = run_agent_fg(sh, act, text_out, err, ecap);
            aios_action_free(act);
        }
        free(goal);
        return rc;
    }
    if (strcmp(c0, "exit") == 0 || strcmp(c0, "quit") == 0) {
        sh->exit_requested = 1;
        *text_out = strdup("bye");
        return 0;
    }

    /* ---- registered tools (M4 registry) ---- */
    if (tool_reg_find(sh->tools, c0))
        return run_tool_stage(sh, c0, a, c->raw, pipe_text, text_out, json_out,
                              err, ecap);

    /* ---- natural language: the intent router ----
     *
     * THIS IS THE AI PATH. The human wrote prose; the machine chose the tool
     * and its arguments. So the descriptor's origin is ORIGIN_AI, and the
     * descriptor already carries the hash of the human line that prompted it
     * (built in sh_route). If a human could get ORIGIN_HUMAN by typing
     * prose, origin tagging would be a formality. */
    sh_decision dec;
    aios_status rst = sh_route(sh->cortex_mode, c->raw ? c->raw : c0,
                               sh->tools, &dec, err, ecap);
    if (rst != AIOS_OK) return 1;
    int rc = 0;

    aios_action *act = sh_decision_to_action(&dec);
    if (!act && dec.kind != SH_ROUTE_REPLY) {
        /* A routed tool/agent with no descriptor is a bug, not a fallback:
         * refuse rather than run something nothing validated. */
        snprintf(err, ecap, "router produced no action descriptor");
        sh_decision_free(&dec);
        return 1;
    }
    if (act) {
        aios_action_stamp_origin(act, ORIGIN_AI);
        /* The router already attempted declaration; if it failed, drop the
         * proposal rather than execute something unexaminable. */
        if (act->n_declared == 0 && act->effects_complete == 0
            && dec.kind == SH_ROUTE_TOOL) {
            snprintf(err, ecap,
                     "action refused: effects of %s cannot be declared",
                     act->target ? act->target : "?");
            aios_action_free(act);
            sh_decision_free(&dec);
            return 1;
        }
    }

    switch (dec.kind) {
    case SH_ROUTE_TOOL:
        if (act) rc = exec_tool_action(sh, act, text_out, json_out, err, ecap);
        break;
    case SH_ROUTE_AGENT:
        /* M7: background wording -> the agents cgroup, not the console */
        if (dec.bg) {
            /* A background job is its own proposal kind: it outlives the
             * command that asked for it, so it is gated as ACT_JOB_SPAWN. */
            rc = spawn_job(sh, dec.goal ? dec.goal : "", text_out, err, ecap);
        } else if (act) {
            rc = run_agent_fg(sh, act, text_out, err, ecap);
        }
        break;
    case SH_ROUTE_REPLY:
        *text_out = xstrdup(dec.text);
        break;
    }
    if (act) aios_action_free(act);
    sh_decision_free(&dec);
    return rc;
}

static int run_pipeline(sh_shell *sh, spipe *pl,
                        char **text_out, json_val **json_out,
                        char *err, size_t ecap) {
    *text_out = NULL;
    *json_out = NULL;
    char *pipe_text = NULL;
    json_val *pipe_json = NULL;
    for (size_t i = 0; i < pl->n; i++) {
        char *ctext = NULL;
        json_val *cjson = NULL;
        int rc = run_command(sh, &pl->cmd[i], pipe_text, pipe_json,
                             &ctext, &cjson, err, ecap);
        free(pipe_text);
        json_free(pipe_json);
        if (rc != 0) {
            *text_out = ctext;
            *json_out = cjson;
            return rc;
        }
        pipe_text = ctext;
        pipe_json = cjson;
    }
    *text_out = pipe_text ? pipe_text : strdup("");
    *json_out = pipe_json;
    return 0;
}

/* ---------------- lifecycle + sh_exec ---------------- */

sh_shell *sh_create(const char *jail_dir, const char *store_dir) {
    if (!jail_dir || !store_dir) return NULL;
    sh_shell *sh = (sh_shell *)calloc(1, sizeof *sh);
    if (!sh) return NULL;
    sh->bus = sembus_create();
    sh->mem = mem_open(store_dir);
    sh->tools = sh->mem ? tool_reg_create(jail_dir, store_dir, "sh", sh->mem) : NULL;
    if (!sh->bus || !sh->mem || !sh->tools) {
        if (sh->tools) tool_reg_free(sh->tools);
        if (sh->mem) mem_close(sh->mem);
        if (sh->bus) sembus_destroy(sh->bus);
        free(sh);
        return NULL;
    }
    tool_reg_install_default(sh->tools);
    snprintf(sh->jail,  sizeof sh->jail,  "%s", jail_dir);
    snprintf(sh->store, sizeof sh->store, "%s", store_dir);
    snprintf(sh->cortex_mode, sizeof sh->cortex_mode, "scripted");
    sh->last_out = strdup("");
    sh->last_json = strdup("");
    sh->next_job_id = 1;
    jobs_restore(sh);   /* M8 durable: replay the previous session's jobs */
    return sh;
}

void sh_free(sh_shell *sh) {
    if (!sh) return;
    for (int i = 0; i < sh->nvars; i++) { free(sh->var_name[i]); free(sh->var_val[i]); }
    for (int i = 0; i < sh->hist_n; i++) free(sh->hist[i]);
    for (int i = 0; i < sh->njobs; i++) { free(sh->jobs[i].goal); free(sh->jobs[i].final); }
    free(sh->last_out);
    free(sh->last_json);
    tool_reg_free(sh->tools);
    mem_close(sh->mem);
    sembus_destroy(sh->bus);
    free(sh);
}

void sh_set_cortex(sh_shell *sh, const char *mode) {
    if (!sh || !mode) return;
    if (strcmp(mode, "scripted") == 0 || strcmp(mode, "llm") == 0 ||
        strcmp(mode, "micro") == 0)
        snprintf(sh->cortex_mode, sizeof sh->cortex_mode, "%s", mode);
}

const char *sh_cortex_mode(sh_shell *sh) { return sh ? sh->cortex_mode : "?"; }
sembus  *sh_bus(sh_shell *sh) { return sh ? sh->bus : NULL; }
mem_ctx *sh_mem(sh_shell *sh) { return sh ? sh->mem : NULL; }

void sh_result_free(sh_result *r) {
    if (!r) return;
    free(r->text);
    free(r->error);
    json_free(r->json);
    memset(r, 0, sizeof *r);
}

int sh_exec(sh_shell *sh, const char *line, sh_result *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->text = strdup("");
    if (!sh || !line) {
        out->status = 1;
        out->error = strdup("invalid sh_exec call");
        return 0;
    }
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return 0;          /* blank / comment */

    /* history (raw line) */
    if (sh->hist_n == SH_HIST_MAX) {
        free(sh->hist[0]);
        for (int i = 1; i < sh->hist_n; i++) sh->hist[i-1] = sh->hist[i];
        sh->hist_n--;
    }
    sh->hist[sh->hist_n++] = xstrdup(line);

    sh_poll_jobs(sh);

    double t0 = aios_mono_ms();
    sline lines = {0};
    char err[SH_ERR_CAP] = {0};
    if (parse_line(sh, line, &lines, err, sizeof err) != 0) {
        out->status = 1;
        out->error = xstrdup(err);
        sh->last_status = 1;
        free(sh->last_out);
        sh->last_out = xstrdup(out->text);
        return 0;
    }

    sb combined = {0};
    for (size_t i = 0; i < lines.n; i++) {
        char *ptext = NULL;
        json_val *pjson = NULL;
        int rc = run_pipeline(sh, &lines.pl[i], &ptext, &pjson,
                              err, sizeof err);
        if (combined.n) sb_putc(&combined, '\n');
        sb_puts(&combined, ptext ? ptext : "");
        out->status = rc;
        sh->last_status = rc;
        json_free(out->json);
        out->json = pjson;
        if (rc != 0) {
            out->error = xstrdup(err[0] ? err : aios_status_str(AIOS_ERR));
            free(ptext);
            break;
        }
        free(ptext);
    }
    sline_free(&lines);
    free(out->text);
    out->text = combined.s ? combined.s : strdup("");

    /* $out / $json for the next line */
    free(sh->last_out);
    sh->last_out = xstrdup(out->text);
    free(sh->last_json);
    sh->last_json = out->json ? json_dump(out->json) : strdup("");

    /* publish the line event on the semantic bus */
    json_val *ev = json_new_obj();
    json_obj_set(ev, "line",    json_new_str(line));
    json_obj_set(ev, "status",  json_new_num((double)out->status));
    json_obj_set(ev, "ms",      json_new_num(aios_mono_ms() - t0));
    json_obj_set(ev, "cortex",  json_new_str(sh->cortex_mode));
    char *evj = json_dump(ev);
    json_free(ev);
    if (evj) { sembus_pub(sh->bus, "sh.cmd", evj); free(evj); }

    return sh->exit_requested;
}
