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
#include "aios/audit.h"
#include "aios/snapshot.h"
#include "aios/supervisor.h"
#include "aios/stage.h"
#include "aios/effects.h"
#include "shroute.h"

#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

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

/* M4b: a remembered state of one jail path, so the parent can tell whether the
 * world moved during a staged command. Declared above the shell because the
 * shell keeps an array of them. */
typedef struct {
    char     rel[AIOS_EFFECT_PATH_MAX];
    bool     existed;
    long     size;
    uint64_t hash;
} jail_witness;

struct sh_shell {
    sembus   *bus;
    mem_ctx  *mem;
    tool_reg *tools;
    char      jail[512];
    char      store[512];
    char      cortex_mode[16];
    int       last_needs_sandbox;    /* M2: host requirements from the verdict */
    int       last_needs_rollback;
    int       last_needs_human;      /* M7: the law said a person must decide */
    aios_origin last_origin;         /* M7: what the last line was stamped as */
    /* M7: who can answer a NEEDS_HUMAN. The library does not assume there is
     * anyone: a front-end installs an asker, and the absence of one is a real
     * answer ("nobody is here"), not a reason to pick a default. */
    ui_choice (*ui_ask)(const ui_prompt *p, void *ud);
    void     *ui_ask_ud;
    int       allow_ui_prompt;       /* cleared to force the refusal path */

    /* M3: a machine-originated write lands here instead of the jail. NULL means
     * the tool layer writes directly (a human typed the command, or the caller
     * opted out). Set up per command, torn down after it. */
    char      stage_dir[512];
    int       stage_live;

    /* M4b: what the jail looked like before a staged command. */
    #define SH_MAX_WITNESS 16
    jail_witness witness[SH_MAX_WITNESS];
    int          n_witness;

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
 * policy_eval() was put here by M2, the stage-and-diff by M3/M4, and M6's chained
 * record is written at the point the call actually executes (tools.c), because a
 * decision that never runs is not an effect. What stays true, and is the whole
 * point of M1: no tool call can be constructed anywhere in this file without
 * first having been a validated, origin-stamped, effect-declared aios_action.
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


/* ---------------- M7: the human's side of NEEDS_HUMAN ------------------- *
 *
 * action_gate() parks an action and says "this needs a human". This is the part
 * that asks one. It lives here, in the library, for a reason that is not
 * tidiness: the serial console and the desktop must ask the SAME question with
 * the SAME three words, and two copies of a question drift. It is also the only
 * way to test the answer without a terminal.
 *
 * The rule that has no exceptions is at the bottom: no answer means DISCARD.
 * `promote` is only ever returned when a person chose it. A timeout is a person
 * not being there, and not being there is not consent.
 */

int ui_prompt_text(const ui_prompt *p, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!p) return 0;
    int n = snprintf(out, cap,
                     "%.150s\n"
                     "  because: %.150s\n"
                     "  proposed by: %s\n"
                     "%s"
                     "  [p] promote   [d] discard   [s] show diff\n"
                     "  (no answer = discard: nothing is promoted by waiting)\n",
                     p->title, p->reason,
                     p->origin == ORIGIN_AI ? "a model" : "a person",
                     p->have_detail ? p->detail : "");
    return n < 0 ? 0 : n;
}

ui_choice ui_present(const ui_prompt *p,
                     ui_choice (*ask)(const ui_prompt *p, void *ud), void *ud) {
    /* No asker means nobody to ask. Not "assume yes", and not a prompt printed
     * into a log that a person will read later: DISCARD, which is the answer that
     * changed nothing. */
    if (!p || !ask) return UI_CHOICE_DISCARD;

    /* SHOW_DIFF is a question, not a loop forever: a person may look as many
     * times as they like, but an asker that keeps answering SHOW_DIFF without a
     * human present would spin. Bound it, and then the honest default. */
    for (int i = 0; i < 16; i++) {
        ui_choice c = ask(p, ud);
        if (c == UI_CHOICE_SHOW_DIFF) continue;
        if (c == UI_CHOICE_PROMOTE) return UI_CHOICE_PROMOTE;
        return UI_CHOICE_DISCARD;
    }
    return UI_CHOICE_DISCARD;
}

/* The console's asker. Reads one line; anything unrecognised is a discard.
 *
 * Deliberately not "the first letter": `p` is promote, and someone typing "please"
 * at a security prompt has not asked for anything. The whole line has to be the
 * word (or its first letter alone). */
void sh_set_ui_asker(sh_shell *sh, ui_choice (*ask)(const ui_prompt *p, void *ud),
                     void *ud) {
    if (!sh) return;
    sh->ui_ask = ask;
    sh->ui_ask_ud = ud;
    sh->allow_ui_prompt = ask ? 1 : 0;
}

ui_choice sh_console_ask(const ui_prompt *p, void *ud) {
    (void)ud;
    char text[1024];
    ui_prompt_text(p, text, sizeof text);
    fputs(text, stdout);
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof line, stdin)) return UI_CHOICE_DISCARD; /* EOF */
    line[strcspn(line, "\r\n")] = 0;
    for (char *q = line; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');

    if (strcmp(line, "p") == 0 || strcmp(line, "promote") == 0 ||
        strcmp(line, "yes") == 0 || strcmp(line, "y") == 0)
        return UI_CHOICE_PROMOTE;
    if (strcmp(line, "s") == 0 || strcmp(line, "show") == 0 ||
        strcmp(line, "show diff") == 0 || strcmp(line, "diff") == 0)
        return UI_CHOICE_SHOW_DIFF;
    return UI_CHOICE_DISCARD;   /* including eof, including garbage */
}

static int builtin_as_machine(sh_shell *sh, sargv *a,
                              char **text_out, json_val **json_out,
                              char *err, size_t ecap) {
    if (!sh) { snprintf(err, ecap, "no shell"); return 1; }
    char *line = join_args(a->v, a->n, 1);
    if (!line || !*line) {
        free(line);
        snprintf(err, ecap, "machine needs a line to run, in quotes");
        return 1;
    }
    sh_result r;
    int rc = sh_exec_origin(sh, line, ORIGIN_AI, &r);
    free(line);
    *text_out = r.text ? r.text : strdup("");
    *json_out = r.json ? r.json : NULL;
    r.text = NULL;
    r.json = NULL;
    /* The AI line's failure is reported as this builtin's ERROR, not as a
     * status: the human typed `machine`, the machine's refusal is the answer. */
    if (rc != 0 || r.status != 0) {
        if (r.error) snprintf(err, ecap, "%s", r.error);
        else snprintf(err, ecap, "the machine-originated line failed");
        sh_result_free(&r);
        return 1;
    }
    sh_result_free(&r);
    return 0;
}

/* M7: "could this be undone right now?"
 *
 * Two callers ask it -- the gate, before it decides, and `policy check`, which
 * exists so a person can see the decision before making it -- and they used to
 * get the same hardcoded 0. One function so they cannot drift again: the point
 * of an introspection builtin is to show what the gate would do, not a
 * separately-maintained opinion about it.
 *
 * The question is per-state, not per-build: a COMPLETE snapshot of this store is
 * a rollback that exists. An interrupted one has no DONE marker, is never
 * offered for restore, and counting it would let the shell claim an undo it
 * cannot perform.
 */
static int sh_rollback_available(const sh_shell *sh) {
    if (!sh) return 0;
    snap_list sl;
    memset(&sl, 0, sizeof sl);
    char err[256];
    int have = 0;
    if (snap_list_all(sh->store, &sl, err, sizeof err)) {
        for (size_t i = 0; i < sl.n; i++)
            if (sl.items[i].complete) { have = 1; break; }
        snap_list_free(&sl);
    }
    return have;
}

static int action_gate(sh_shell *sh, const aios_action *act, aios_origin origin,
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
    /* M7: was a hardcoded 0, which made every destructive AI action report "not
     * undoable yet" long after M5 shipped snapshots -- a stale truth is a lie
     * with a good excuse. sh_rollback_available() asks the real question. */
    pctx.rollback_available = sh_rollback_available(sh);

    aios_decision d = aios_policy_eval(act, sh->jail, SH_CAPS, SH_N_CAPS,
                                       &pctx, aios_now_ms());

    char *act_json = aios_action_to_json(act);
    /* THREE verdicts, and the host must treat them as three.
     *
     * Before M7 this was `if (d.v != VERDICT_ALLOW)` -- so NEEDS_HUMAN was
     * collapsed into a refusal. That is the safe direction but it is not what the
     * law says: NEEDS_HUMAN means "a person must decide", and a host that refuses
     * instead of asking has quietly replaced the verdict with its own. It also
     * made the whole middle of the verdict space unreachable, which is why no test
     * noticed.
     *
     * DENY stays a hard refusal. NEEDS_HUMAN is now reported AS needing a human,
     * and the caller decides what to do about it. */
    if (d.v == VERDICT_DENY) {
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
    if (d.v == VERDICT_NEEDS_HUMAN) {
        /* Not a refusal and not an execution: a proposal parked in front of a
         * person. Three things happen, in this order, and the order is the point:
         *
         *   1. the action is OFFERED through ui_present(), which cannot return
         *      PROMOTE unless someone chose it -- no timeout counts as yes, no
         *      asker at all means discard;
         *   2. if a person chose promote, the gate accepts the action it already
         *      evaluated. Not a re-run of the line: a re-run could race, or read
         *      different arguments the second time, and the thing the person
         *      approved would not be the thing that ran;
         *   3. otherwise the caller is told what was held, in words that name a
         *      person as the missing ingredient rather than the law as a wall.
         *
         * sh->ui_ask is set by the front-end. Unset (a library embedder, or a
         * test that wants the refusal) means nobody can answer -- and the honest
         * result of "nobody can answer" is that nothing happens. */
        sh->last_needs_human = 1;
        int accepted = 0;
        if (sh->ui_ask && sh->allow_ui_prompt) {
            ui_prompt up;
            memset(&up, 0, sizeof up);
            snprintf(up.title, sizeof up.title, "%s%s%s",
                     aios_action_kind_name(act->kind),
                     act->target ? " " : "", act->target ? act->target : "");
            snprintf(up.reason, sizeof up.reason, "%s: %.140s",
                     aios_policy_reason_str(d.r), d.why);
            up.origin = origin;
            /* `show diff` needs something to show. A tool action has no staged
             * diff (M3 stages writes, not arbitrary tools), so say what it would
             * touch rather than pretending to have a patch. */
            snprintf(up.detail, sizeof up.detail, "  target: %.200s\n",
                     act->target ? act->target : "(none)");
            up.have_detail = 1;
            ui_choice c = ui_present(&up, sh->ui_ask, sh->ui_ask_ud);
            if (c == UI_CHOICE_PROMOTE) {
                accepted = 1;
                sembus_pub_kv(sh->bus, "sh.action.promoted_by_human",
                              (const char *const[]){
                                  "kind", aios_action_kind_name(act->kind),
                                  "reason", aios_policy_reason_str(d.r), NULL});
            } else {
                sembus_pub_kv(sh->bus, "sh.action.discarded",
                              (const char *const[]){
                                  "kind", aios_action_kind_name(act->kind),
                                  "why", "the human did not promote it", NULL});
            }
        }
        if (accepted) {
            /* A person said yes. Fall into the ALLOW path below rather than
             * duplicating it: two execution paths drift, and the second one is
             * always the one nobody tests. */
            sembus_pub_kv(sh->bus, "sh.action.log",
                          (const char *const[]){
                              "verdict", "ALLOW(on human promote)",
                              "reason", aios_policy_reason_str(d.r),
                              "action", act_json ? act_json : "", NULL});
            free(act_json);
            if (d.needs_sandbox) sh->last_needs_sandbox = 1;
            if (d.needs_rollback) sh->last_needs_rollback = 1;
            return 0;
        }
        snprintf(err, ecap,
                 "this needs a human [%s]: %s%s",
                 aios_policy_reason_str(d.r), d.why,
                 origin == ORIGIN_AI
                     ? " -- a machine proposed it, and only a person can accept it"
                     : " -- run it yourself to accept it");
        sembus_pub_kv(sh->bus, "sh.action.needs_human",
                      (const char *const[]){
                          "kind", aios_action_kind_name(act->kind),
                          "target", act->target ? act->target : "",
                          "reason", aios_policy_reason_str(d.r),
                          "why", d.why,
                          "origin", origin == ORIGIN_AI ? "ai" : "human", NULL});
        sembus_pub_kv(sh->bus, "sh.action.log",
                      (const char *const[]){
                          "verdict", "NEEDS_HUMAN",
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

/* Build (and publish) the record of an action we accepted.
 *
 * This is the decision being made observable on the bus; the durable, chained
 * record of what actually EXECUTED is written by tools.c through audit.c. Both
 * matter and they are not the same record: this one says "a proposal was accepted",
 * that one says "a tool ran and here is what it did". */
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

static bool hash_file(const char *abs, uint64_t *out, long *size_out) {
    char *buf = NULL;
    size_t len = 0;
    if (aios_read_file(abs, &buf, &len, 1u * 1024 * 1024) != AIOS_OK) return false;
    *out = aios_fnv1a(buf, len);
    if (size_out) *size_out = (long)len;
    free(buf);
    return true;
}

/* Record the current state of a jail-relative path. */
static void witness_take(const char *jail, const char *rel, jail_witness *w) {
    memset(w, 0, sizeof *w);
    snprintf(w->rel, sizeof w->rel, "%s", rel);
    char abs[AIOS_EFFECT_PATH_MAX * 2];
    int n = snprintf(abs, sizeof abs, "%s/%s", jail, rel);
    if (n < 0 || (size_t)n >= sizeof abs) { w->existed = true; return; }  /* unnameable: assume it exists and "changed" */
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) { w->existed = false; return; }
    w->existed = true;
    w->size = (long)st.st_size;
    if (!hash_file(abs, &w->hash, NULL)) w->hash = 0;
}

/* Did it change? A missing-then-present file is a change; so is a different
 * hash. A present-then-missing file is a change too (a delete by a write). */
static bool witness_changed(const char *jail, const jail_witness *w) {
    jail_witness now;
    witness_take(jail, w->rel, &now);
    if (now.existed != w->existed) return true;
    if (!now.existed) return false;
    return now.hash != w->hash || now.size != w->size;
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

    /* Witness the jail BEFORE the worker runs. This is what makes the
     * guarantee independent of the worker's honesty. */
    sh->n_witness = 0;
    for (int i = 0; i < act->n_declared && sh->n_witness < SH_MAX_WITNESS; i++) {
        const char *eff = act->declared[i];
        if (!eff || strncmp(eff, "write:", 6) != 0) continue;
        if (eff[6]) witness_take(sh->jail, eff + 6,
                                 &sh->witness[sh->n_witness++]);
    }

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

/* Abandon a staged session: throw the directory away AND forget it. Used when
 * the result must not be offered to anyone -- an undeclared effect, a jail that
 * moved. Dropping the pointer alone leaves an empty "open" session in the store,
 * which looks to `staged` like something waiting for review. */
static void sh_stage_abandon(sh_shell *sh) {
    if (!sh) return;
    if (sh->stage_live && sh->stage_dir[0]) {
        stage_session sess;
        char derr[192] = {0};
        if (stage_open(sh->stage_dir, &sess, derr, sizeof derr)) {
            (void)stage_discard(&sess, derr, sizeof derr);
            stage_end(&sess);
        }
    }
    sh->stage_live = 0;
    sh->stage_dir[0] = 0;
    sh->n_witness = 0;
}

static void sh_stage_drop(sh_shell *sh) {
    if (!sh) return;
    sh->stage_live = 0;
    sh->stage_dir[0] = 0;
}

/* M7: is there anything in the staging area, or is the manifest all there is?
 *
 * The shell pre-notes every declared write, so a session can have rows and no
 * files. Ask the directory, not the notes: this is the difference between "a
 * change is waiting for you" and "a change was declared". */
static int sh_stage_has_bytes(const char *stage_dir) {
    char up[700];
    snprintf(up, sizeof up, "%s/upper", stage_dir);
    DIR *d = opendir(up);
    if (!d) return 0;
    int have = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        have = 1;
        break;
    }
    closedir(d);
    return have;
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

        /* M7: a staged session can be EMPTY and still look full.
         *
         * The shell pre-notes every declared write so `staged` has rows even if
         * the worker never ran -- which is a good property (a refused worker
         * still owes the human an explanation). But it means the diff has a row
         * for a file that does not exist in upper/, and the report below used to
         * say "nothing was written to the jail, promote to apply it" over an
         * empty stage. A human who read that and typed `promote` got "promoted 0
         * files into the jail" and no file: a promise the shell could not keep,
         * made in the one place whose whole job is to be trustworthy about what
         * did and did not happen.
         *
         * So ask the staging area, not the manifest. If nothing is in upper/,
         * say that the write did not happen and why it matters, rather than
         * offering to promote an emptiness. This fired in the boot image, where
         * the sandbox worker could not be found -- an environment problem that
         * presented as a successful agent run. */
        int have_bytes = sh_stage_has_bytes(s.dir);
        if (asprintf(&msg,
                     "%s"
                     "%s",
                     body ? body : "",
                     have_bytes
                         ? "  nothing was written to the jail.\n"
                           "  promote            apply it\n"
                           "  discard            throw it away\n"
                         : "  NOTHING WAS STAGED. The write did not happen, so "
                           "there is\n"
                           "  nothing to promote -- the rows above are the "
                           "declared\n"
                           "  effects, not files. A staged set that is empty "
                           "usually\n"
                           "  means the sandbox worker could not be run.\n") < 0)
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

/* Did the jail move where it was told not to? Returns 1 and names the path. */
static int sh_jail_moved(sh_shell *sh, char *first, size_t fcap) {
    if (first && fcap) first[0] = 0;
    for (int i = 0; i < sh->n_witness; i++) {
        if (witness_changed(sh->jail, &sh->witness[i])) {
            if (first && fcap) snprintf(first, fcap, "%s", sh->witness[i].rel);
            return 1;
        }
    }
    return 0;
}

/* ---- M4b: the parent watches the jail, not the story ----
 *
 * Everything in M4 compares what the worker SAID against what was DECLARED.
 * That is a real check, and it is not enough, because both halves can be lies
 * from the same source. The test that proved it: swap `tool-exec` for a worker
 * that ignores the `stage` argument and writes into the jail, then reports only
 * the file it was asked about. Its account matches the declaration exactly, so
 * every rule above passes -- and the file is already in the jail.
 *
 * M3's guarantee was therefore being enforced by the worker. The worker is the
 * component we least control, so the guarantee has to be checked from outside
 * it: the parent looks at the jail itself, before and after, and refuses if the
 * world moved. Testimony is what we ASK; the witness is what we CHECK.
 *
 * Cost: a bounded read (hash) of each declared write path, twice per staged
 * command. Files are already capped at 1 MiB, and only for AI writes.
 */


/* ---- M4: did it do what it said? ----
 *
 * Everything up to here has trusted the declaration. This is the first place
 * that asks whether the declaration was true, and it is the only place that can
 * do anything about the answer, because it is the only place that holds both
 * halves: what the descriptor claimed and what the worker reported.
 *
 * The asymmetry is the whole design:
 *
 *   clean account          -> accept, log the numbers
 *   under-delivered        -> accept, log it (nothing happened; not dangerous)
 *   undeclared effect      -> FAIL CLOSED. The result is discarded, the staging
 *                             directory is discarded, the action is denied
 *                             after the fact and the reason names the path.
 *
 * Discarding the staging directory rather than "reviewing it anyway" is the
 * point: a worker that touched a file it never mentioned has demonstrated it
 * cannot be trusted about the files it did mention.
 */
static int sh_check_effects(sh_shell *sh, const aios_action *act,
                            char *err, size_t ecap) {
    if (!sh || !act || !act->target) return 0;

    /* Only tools that run in the worker can account for themselves. A native
     * tool runs in this process: there is no worker to interrogate, and its
     * effects are this process's effects, which the shell already knows because
     * it is the one running them. Treating "no document" as a violation here
     * would fail-closed on every native tool, which teaches people that the
     * check is noise -- the worst outcome for a safety mechanism. */
    const tool_def *def = tool_reg_find(sh->tools, act->target);
    if (!def || !def->sandboxed) return 0;

    aios_effects actual;
    char perr[192] = {0};
    if (!aios_effects_from_array(tool_reg_last_effects(sh->tools),
                                 tool_reg_last_complete(sh->tools),
                                 &actual, perr, sizeof perr)) {
        snprintf(err, ecap, "the worker's effect report was malformed: %s", perr);
        return 1;      /* malformed is not "no news", it is bad news */
    }

    char first[256] = {0};
    aios_effects_verdict v = aios_effects_compare(&actual, (const char *const *)act->declared,
                                                 act->n_declared, first, sizeof first);

    if (v == EFFECTS_MISSING) {
        /* A sandboxed tool that returns no account is not the build we think it
         * is. That is worth failing on: silence cannot be compared with
         * anything, so it cannot be cleared. */
        sembus_pub_kv(sh->bus, "sh.action.refused", (const char *const[]){
            "kind", aios_action_kind_name(act->kind),
            "target", act->target, "verdict", "DENY",
            "reason", "EFFECTS_MISSING", "why", "no effects document", NULL});
        snprintf(err, ecap, "the worker returned no effects document");
        return 1;
    }

    if (v == EFFECTS_OK || v == EFFECTS_UNDER_DELIVERED) {
        /* Under-delivery is recorded but accepted: the world did not change in
         * a way nobody expected, which is the only thing this rule is for. */
        sembus_pub_kv(sh->bus, "sh.effects", (const char *const[]){
            "action", act->target,
            "verdict", aios_effects_verdict_str(v),
            "detail", first, NULL});
        return 0;
    }

    /* Undeclared, incomplete or malformed: fail closed, and throw the staging
     * directory away. A worker that touched a file it never mentioned has shown
     * it cannot be trusted about the files it did mention, so "let the human
     * review it anyway" would launder the lie into a diff. */
    const char *reason = (v == EFFECTS_UNDECLARED) ? "UNDECLARED_EFFECT"
                                                   : "EFFECTS_UNACCOUNTED";
    sembus_pub_kv(sh->bus, "sh.action.refused", (const char *const[]){
        "kind", aios_action_kind_name(act->kind),
        "target", act->target, "verdict", "DENY",
        "reason", reason,
        "why", first[0] ? first : aios_effects_verdict_str(v), NULL});
    sembus_pub_kv(sh->bus, "sh.action.log", (const char *const[]){
        "verdict", "DENY", "reason", reason,
        "action", act->target, "detail", first, NULL});

    sh_stage_abandon(sh);

    if (v == EFFECTS_UNDECLARED)
        snprintf(err, ecap, "undeclared effect: the worker touched %.120s, "
                            "which the descriptor never declared", first);
    else
        snprintf(err, ecap, "the worker could not account for its effects: %s",
                 aios_effects_verdict_str(v));
    return 1;
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
    int       jail_mutated;   /* fatal: the worker ignored staging entirely */

    /* M4: the descriptor the guarded step built, kept so the post-check can
     * compare the worker's account against the SAME claim that was permitted.
     * Rebuilding it afterwards would compare reality against a different
     * question. */
    aios_action *last_act;
} sh_guard_ctx;

static int sh_agent_guard(void *ud, const char *tool, json_val *args,
                          char *err, size_t ecap) {
    sh_guard_ctx *g = (sh_guard_ctx *)ud;
    if (!g || !g->sh) return 1;
    sh_shell *sh = g->sh;
    aios_action_free(g->last_act);
    g->last_act = NULL;

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

    /* The agent loop's own actions are ORIGIN_AI by construction -- the planner
     * cannot choose an origin, which is the property that makes this line
     * trustworthy rather than hopeful. */
    if (action_gate(sh, act, ORIGIN_AI, err, ecap) != 0) {
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

    /* Keep the descriptor for the post-check; the tool gets its own copy of the
     * args, so the loop's json_val is not consumed twice. */
    aios_action_set_args(act, json_new_obj());
    g->last_act = act;
    (void)path;
    return rc;
}

static int sh_agent_after(void *ud, const char *tool, char *err, size_t ecap) {
    sh_guard_ctx *g = (sh_guard_ctx *)ud;
    if (!g || !g->sh || !g->last_act) return 0;
    (void)tool;

    /* 1. what the worker says it did, against what it was permitted to do */
    if (sh_check_effects(g->sh, g->last_act, err, ecap) != 0) return 1;

    /* 2. what the JAIL says, against what the worker said. This is the only
     *    check here that does not rely on the worker's honesty: a swapped or
     *    compromised tool-exec can forge its account, but it cannot forge the
     *    jail it left behind. */
    char moved[AIOS_EFFECT_PATH_MAX] = {0};
    if (sh_jail_moved(g->sh, moved, sizeof moved)) {
        sh_stage_abandon(g->sh);        /* nothing to promote: the run is void */
        g->jail_mutated = 1;
        sembus_pub_kv(g->sh->bus, "sh.action.refused", (const char *const[]){
            "kind", aios_action_kind_name(g->last_act->kind),
            "target", g->last_act->target ? g->last_act->target : "",
            "verdict", "DENY", "reason", "JAIL_MUTATED", "why", moved, NULL});
        snprintf(err, ecap,
                 "the jail changed during a staged write (%.120s): the worker is "
                 "not honouring the staging directory, so its account of what it "
                 "did cannot be trusted", moved);
        return 1;
    }
    g->sh->n_witness = 0;
    return 0;
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
static int exec_tool_action(sh_shell *sh, aios_action *act, aios_origin origin,
                            char **text_out, json_val **json_out,
                            char *err, size_t ecap) {
    if (action_gate(sh, act, origin, err, ecap) != 0) {
        stage_text_of(NULL, NULL, text_out);
        *json_out = NULL;
        return 1;
    }
    action_accept(sh, act);

    /* M3: an AI write is intercepted before it can reach the jail. */
    /* `sh->stage_live` is already set when the agent guardian staged this step
     * earlier; a typed AI write gets here with it clear. Checking both means
     * the witness runs for either route -- the first version of this only
     * handled the typed one, and the swapped-worker test sailed through. */
    bool staged = (sh->stage_live != 0);
    if (!staged && act->origin == ORIGIN_AI && strcmp(act->target, "fs.write") == 0) {
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
    tool_reg_clear_effects(sh->tools);
    int rc = exec_tool(sh, act->target, aios_action_take_args(act),
                       text_out, json_out, err, ecap);
    tool_reg_set_staging(sh->tools, NULL);

    /* M4: the verdict on the worker's own account, before anything is offered
     * to the human as "here is what it wants to do". */
    if (rc == 0 && sh_check_effects(sh, act, err, ecap) != 0) {
        free(*text_out);
        *text_out = strdup("");
        json_free(*json_out);
        *json_out = NULL;
        return 1;
    }

    /* A jail mutation is not a step the agent can route around: it means the
     * worker is not honouring staging at all. Report it as a fatal condition of
     * the run rather than a failed step, or the agent reports success while
     * reality has already been changed. */
    if (staged) {
        char moved[AIOS_EFFECT_PATH_MAX] = {0};
        if (sh_jail_moved(sh, moved, sizeof moved)) {
            sh_stage_abandon(sh);
            snprintf(err, ecap,
                     "the jail changed during a staged write (%.120s): the worker "
                     "is not honouring the staging directory", moved);
            sembus_pub_kv(sh->bus, "sh.action.refused", (const char *const[]){
                "kind", aios_action_kind_name(act->kind),
                "target", act->target ? act->target : "",
                "verdict", "DENY", "reason", "JAIL_MUTATED", "why", moved, NULL});
            free(*text_out);
            *text_out = strdup("");
            json_free(*json_out);
            *json_out = NULL;
            return 1;
        }
        sh->n_witness = 0;
    }

    /* M4b: and the check that does not depend on the worker telling the truth.
     * If the jail moved while the worker was supposed to be writing into
     * staging, the worker is enforcing nothing and its account is worthless --
     * refuse, and say which file moved. */
    if (staged) {
        char moved[AIOS_EFFECT_PATH_MAX] = {0};
        if (sh_jail_moved(sh, moved, sizeof moved)) {
            sh_stage_drop(sh);
            char reason[AIOS_EFFECT_PATH_MAX + 96];
            snprintf(reason, sizeof reason,
                     "the jail changed during a staged write (%.120s): the worker "
                     "is not honouring the staging directory", moved);
            sembus_pub_kv(sh->bus, "sh.action.refused", (const char *const[]){
                "kind", aios_action_kind_name(act->kind),
                "target", act->target ? act->target : "",
                "verdict", "DENY", "reason", "JAIL_MUTATED",
                "why", moved, NULL});
            free(*text_out);
            *text_out = strdup("");
            json_free(*json_out);
            *json_out = NULL;
            snprintf(err, ecap, "%.400s", reason);
            return 1;
        }
        sh->n_witness = 0;
    }

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
 * The origin is the CALLER'S, not this function's to decide (M7). A human typing
 * a tool name and a model producing the same line are different events, and the
 * only place that knows which happened is the caller of sh_exec_origin(). */
static int run_tool_stage(sh_shell *sh, const char *name, sargv *a,
                          aios_origin origin,
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
    aios_action *act = aios_action_new(ACT_TOOL, origin, name);
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
    int rc = exec_tool_action(sh, act, origin, text_out, json_out, err, ecap);
    aios_action_free(act);
    return rc;
}

/* ---------------- agent execution ---------------- */

/* `goal` is the descriptor's target; the descriptor itself carries the
 * intent hash and origin that M6 will log and M2 will evaluate. An agent
 * cannot be started without one. */

/* M5: take a snapshot before anything a rollback might have to undo.
 *
 * This is the whole guarantee of the milestone: "rollback is available" means a
 * snapshot already exists at the moment something went wrong, not that a
 * snapshot COULD have been taken. So it happens automatically, here, before the
 * work -- not as a command a human has to remember at 2am.
 *
 * A failure to snapshot is published and reported but does not stop the work:
 * refusing to run an agent because a backup could not be written would make the
 * backup a single point of failure for the whole OS. The refusal is visible
 * (`sh.snap.failed` on the bus, and the text of the command) because a
 * rollback that silently does not exist is worse than a loud one.
 *
 * Returns true when a snapshot exists afterwards. */

/* ---- M5: asking the supervisor instead of doing it ourselves ---- *
 *
 * Rollback power belongs to PID 1 (include/aios/supervisor.h says why). The shell
 * therefore asks over the socket when the supervisor is there, and only falls
 * back to doing the work in-process when it is not -- which is the case on a
 * developer's machine, where there is no PID 1 to ask.
 *
 * The fallback is honest about itself: it prints "no supervisor". A shell that
 * silently used its own copy while claiming the OS handled it would be claiming
 * a guarantee it did not have.
 */
static bool sup_round_trip(const char *req, char *out, size_t cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", AIOS_SUP_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return false;
    }
    (void)!write(fd, req, strlen(req));

    size_t n = 0;
    struct pollfd pfd = { fd, POLLIN, 0 };
    while (n + 1 < cap) {
        /* Restore can copy a whole tree; a fixed 5 s would fail on a big store
         * where the answer is late but correct. */
        if (poll(&pfd, 1, 30000) <= 0) break;
        ssize_t r = read(fd, out + n, cap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(out, '\n', n)) break;
    }
    out[n] = 0;
    close(fd);
    return n > 0;
}

/* Ask PID 1 to put the trees back. The shell still decides WHICH snapshot and
 * still revokes its own jobs first -- those are the shell's own state, and PID 1
 * has no business knowing about this shell's job table. What it asks for is the
 * part that belongs to the OS: moving two trees. */
static bool sup_restore(const char *which, char *replaced, size_t rcap,
                        char *err, size_t ecap) {
    char req[512];
    snprintf(req, sizeof req, "{\"op\":\"restore\",\"id\":\"%.200s\"}\n", which);
    char out[4096] = {0};
    if (!sup_round_trip(req, out, sizeof out)) {
        snprintf(err, ecap, "the supervisor did not answer");
        return false;
    }
    if (strstr(out, "\"ok\":true") == NULL) {
        const char *e = strstr(out, "\"error\":\"");
        snprintf(err, ecap, "%.400s", e ? e + 9 : "supervisor refused");
        return false;
    }
    if (replaced && rcap) {
        const char *r = strstr(out, "\"replaced\":\"");
        if (r) {
            r += 12;
            size_t n = 0;
            while (r[n] && r[n] != '"' && n + 1 < rcap) { replaced[n] = r[n]; n++; }
            replaced[n] = 0;
        }
    }
    return true;
}

/* Is there a supervisor to ask? A stale socket that nobody is listening on must
 * count as absent, or every `snap` on a developer machine would try the socket
 * first and print a connection error. */
static bool sup_present(const sh_shell *sh) {
    /* If the socket file is not there, do not even try to connect: on a
     * developer's machine there is no supervisor, and every `snap` would pay a
     * failed connection for nothing.
     *
     * If the socket's DIRECTORY exists but the socket does not, the supervisor is
     * starting right now -- PID 1 forks it before any shell, but a shell started
     * by hand in the same second can still win the race. Waiting briefly is the
     * difference between "PID 1 owns rollback" and "PID 1 owns rollback unless you
     * were quick", which is not a guarantee at all. */
    if (access(AIOS_SUP_SOCK, F_OK) != 0) {
        if (access(AIOS_SUP_DIR, F_OK) != 0) return false;
        bool up = false;
        for (int i = 0; i < 50 && !up; i++) {          /* up to ~1 s */
            usleep(20 * 1000);
            up = access(AIOS_SUP_SOCK, F_OK) == 0;
        }
        if (!up) return false;
    }
    char out[2048] = {0};
    if (!sup_round_trip("{\"op\":\"ping\"}\n", out, sizeof out)) return false;
    if (strstr(out, "\"ok\":true") == NULL) return false;

    /* It answered -- but is it serving MY trees? In the image PID 1 serves
     * /workspace and /data/aios/store. The test suite points a shell at a scratch
     * jail and store, and asking PID 1 to snapshot those would write the snapshot
     * into /data/aios/store and then fail to find it in the scratch store. The
     * user-visible symptom was "snap taken" followed by a listing that did not
     * contain it.
     *
     * Comparing the store is the honest test: two shells can share a store and
     * jail and be the same trees; a different store is not this shell's state. */
    const char *sp = strstr(out, "\"store\":\"");
    if (!sp) return false;
    sp += 9;
    size_t n = 0;
    while (sp[n] && sp[n] != '"') n++;
    if (n != strlen(sh->store) || strncmp(sp, sh->store, n) != 0) return false;
    return true;
}

/* Take a snapshot, through PID 1 when it is there. Returns true on success and
 * fills `id` when the supervisor supplied one. */
static bool sup_take(const char *label, char *id, size_t icap, char *err, size_t ecap) {
    char req[512];
    snprintf(req, sizeof req, "{\"op\":\"snap\",\"label\":\"%.200s\"}\n", label);
    char out[4096] = {0};
    if (!sup_round_trip(req, out, sizeof out)) {
        snprintf(err, ecap, "the supervisor did not answer");
        return false;
    }
    if (strstr(out, "\"ok\":true") == NULL) {
        const char *e = strstr(out, "\"error\":\"");
        snprintf(err, ecap, "supervisor refused: %.200s",
                 e ? e + 9 : "no reason given");
        return false;
    }
    const char *i = strstr(out, "\"id\":\"");
    if (i && id && icap) {
        i += 6;
        size_t n = 0;
        while (i[n] && i[n] != '"' && n + 1 < icap) { id[n] = i[n]; n++; }
        id[n] = 0;
    }
    return true;
}

static bool sh_snap_before(sh_shell *sh, const char *why, char *note, size_t ncap) {
    char id[AIOS_SNAP_ID_MAX] = {0};
    char err[256] = {0};
    /* Ask PID 1 when it is there. It takes the snapshot of the SAME jail and
     * store, so the answer is identical; what differs is who holds the power. */
    bool ok;
    if (sup_present(sh))
        ok = sup_take(why, id, sizeof id, err, sizeof err);
    else
        ok = snap_take(sh->jail, sh->store, why, id, sizeof id, err, sizeof err);
    if (!ok) {
        sembus_pub_kv(sh->bus, "sh.snap.failed",
                      (const char *const[]){ "why", why, "error", err, NULL });
        if (note && ncap)
            snprintf(note, ncap, " (no snapshot taken: %.120s)", err);
        return false;
    }
    sembus_pub_kv(sh->bus, "sh.snap.taken",
                  (const char *const[]){ "id", id, "why", why, NULL });
    /* Retention is enforced every time, not by a cron nobody runs. */
    char perr[192] = {0};
    (void)snap_prune(sh->store, AIOS_SNAP_KEEP, perr, sizeof perr);
    if (note && ncap) snprintf(note, ncap, " (snapshot %.24s)", id);
    return true;
}

/* M5: revoke running jobs. A restore that leaves an agent writing into the tree
 * it is restoring is not a rollback, it is a race. Returns how many were killed. */
static int sh_revoke_jobs(sh_shell *sh) {
    int killed = 0;
    for (int i = 0; i < sh->njobs; i++) {
        sh_job *j = &sh->jobs[i];
        if (j->state != JOB_RUNNING) continue;
        kill(j->pid, SIGKILL);
        for (int k = 0; k < 200; k++) {
            int wst = 0;
            if (waitpid(j->pid, &wst, WNOHANG) == j->pid) break;
            usleep(10000);
        }
        j->state = JOB_KILLED;
        killed++;
    }
    if (killed)
        sembus_pub_kv(sh->bus, "sh.jobs.revoked",
                      (const char *const[]){ "n", "revoked", "why", "rollback", NULL });
    return killed;
}

static int run_agent_fg(sh_shell *sh, const aios_action *act, aios_origin origin,
                        char **text_out, char *err, size_t ecap) {
    if (action_gate(sh, act, origin, err, ecap) != 0) {
        *text_out = strdup("");
        return 1;
    }
    action_accept(sh, act);
    /* M5: the snapshot happens before the first tool call, so every write the
     * agent makes has something to be rolled back to. */
    char snapnote[160] = {0};
    sh_snap_before(sh, "before an agent run", snapnote, sizeof snapnote);
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
        .guard = { .check = sh_agent_guard, .after = sh_agent_after, .ud = &gctx }
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
        sh->n_witness = 0;
    } else {
        *text_out = xstrdup(final);
        sh->n_witness = 0;
    }
    if (gctx.jail_mutated) {
        sh_stage_drop(sh);
        snprintf(err, ecap,
                 "agent run refused: the tool worker wrote into the jail instead "
                 "of staging, so its result was not accepted");
        return 1;
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
    "  audit [verify]              walk the hash-chained record and say if it holds\n"
    "  snap [take LABEL]           list snapshots, or take one now (M5)\n"
    "  rollback [latest|ID]        undo to a snapshot; running jobs are revoked\n"
    "  machine \"LINE\"               run LINE as if a MODEL had typed it (M7)\n"
    "                              origin tagging: what a machine may and may not do\n"
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
        /* M7: the same function the gate calls, so this builtin reports what the
         * gate would see rather than its own guess about it. */
        pctx.rollback_available = sh_rollback_available(sh);
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
        snprintf(line, sizeof line, "as judged: sandbox=%s rollback=%s\n",
                 pctx.sandbox_executed ? "done" : "none",
                 pctx.rollback_available ? "available" : "not available");
        sb_puts(&b, line);
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
    /* M7: the same honesty as the report after a run. `staged` is where a human
     * goes to decide, so it must not show a pending change that has no bytes
     * behind it -- the rows come from the manifest, and the manifest is written
     * before the worker runs. */
    if (!sh_stage_has_bytes(dir) && body) {
        char *msg = NULL;
        if (asprintf(&msg, "%s"
                           "  nothing is actually staged: the rows above are "
                           "declared effects,\n"
                           "  not files. There is nothing here to promote.\n",
                     body) >= 0) {
            free(body);
            body = msg;
        }
    }
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
    /* M5: a promote is the moment a change becomes real, so it is the moment
     * worth having a snapshot of the state before it. */
    char snapnote[160] = {0};
    sh_snap_before(sh, "before a promote", snapnote, sizeof snapnote);
    /* M7: the origin travels even though an AI can never get here -- the
     * dispatcher refuses first. Belt, braces, and a reason to believe the
     * belt is on: this is the call the acceptance test names. */
    bool ok = stage_promote_locked(&s, sh->last_origin, &n, err, ecap);
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

/* `snap` -- list, or take, a snapshot. */
static int builtin_snap(sh_shell *sh, sargv *a, char **text_out,
                        char *err, size_t ecap) {
    if (a->n >= 2 && strcmp(a->v[1], "take") == 0) {
        const char *label = a->n >= 3 ? a->v[2] : "manual";
        char id[AIOS_SNAP_ID_MAX] = {0};
        /* The explicit verb goes through PID 1 too. It would be a strange OS
         * where the automatic snapshots are owned by the supervisor and the one a
         * person asks for is made by the shell -- and the difference would be
         * invisible until the day it mattered. */
        bool ok;
        if (sup_present(sh))
            ok = sup_take(label, id, sizeof id, err, ecap);
        else
            ok = snap_take(sh->jail, sh->store, label, id, sizeof id, err, ecap);
        if (!ok) return 1;
        char perr[128] = {0};
        (void)snap_prune(sh->store, AIOS_SNAP_KEEP, perr, sizeof perr);
        char line[256];
        snprintf(line, sizeof line, "snapshot %.48s taken (%s)", id, label);
        *text_out = strdup(line);
        return 0;
    }
    snap_list l;
    if (!snap_list_all(sh->store, &l, err, ecap)) return 1;
    char *txt = snap_list_text(&l);
    size_t need = strlen(txt ? txt : "") + 200;
    char *out = (char *)malloc(need);
    if (out) {
        snprintf(out, need,
                 "snapshots (%zu, keeping %d):\n%s  rollback [latest|<id>] undoes "
                 "the most recent one",
                 l.n, AIOS_SNAP_KEEP, txt ? txt : "");
        free(txt);
        *text_out = out;
    } else {
        free(txt);
        *text_out = strdup("");
    }
    snap_list_free(&l);
    return 0;
}

/* `rollback [latest|<id>]` -- restore the jail and store, revoking jobs.
 *
 * The order is deliberate and is the milestone's whole argument: jobs are
 * stopped FIRST, because restoring a tree that an agent is still writing to
 * produces a state that is neither the old one nor the new one. */
static int builtin_rollback(sh_shell *sh, sargv *a, char **text_out,
                            char *err, size_t ecap) {
    const char *which = a->n >= 2 ? a->v[1] : "latest";

    snap_info si;
    if (!snap_find(sh->store, which, &si, err, ecap)) return 1;

    int killed = sh_revoke_jobs(sh);
    sh_poll_jobs(sh);

    char replaced[600] = {0};
    char rerr[512] = {0};
    bool restored;
    if (sup_present(sh))
        restored = sup_restore(si.id, replaced, sizeof replaced, rerr, sizeof rerr);
    else
        restored = snap_restore(sh->jail, sh->store, si.id,
                                replaced, sizeof replaced, rerr, sizeof rerr);
    if (!restored) {
        snprintf(err, ecap, "rollback failed: %.400s", rerr);
        sembus_pub_kv(sh->bus, "sh.rollback.failed",
                      (const char *const[]){ "id", si.id, "error", rerr, NULL });
        return 1;
    }
    sembus_pub_kv(sh->bus, "sh.rollback.done",
                  (const char *const[]){ "id", si.id, "jobs_revoked", "yes", NULL });

    /* The tool registry and memory hold the OLD tree's state in memory. Reopening
     * them is not optional: a shell that reports a rollback while its memory
     * still describes the rolled-back session is lying about what it did. */
    tool_reg_free(sh->tools);
    mem_close(sh->mem);
    sh->mem = mem_open(sh->store);
    sh->tools = sh->mem ? tool_reg_create(sh->jail, sh->store, "sh", sh->mem) : NULL;
    if (sh->tools) tool_reg_install_default(sh->tools);

    char line[900];
    snprintf(line, sizeof line,
             "rolled back to %.48s (%s)\n"
             "  %d running job%s revoked\n"
             "  the state that was replaced is in %.200s",
             si.id, si.label[0] ? si.label : "no label",
             killed, killed == 1 ? "" : "s",
             replaced[0] ? replaced : "(not recorded)");
    *text_out = strdup(line);
    return 0;
}


/* `audit [verify]` -- read the record the way a stranger would.
 *
 * The point of a hash chain is that verification needs nothing but the file. So
 * this builtin does not ask the recorder whether the recorder was honest: it walks
 * the bytes.
 *
 * The output distinguishes four answers that are easy to conflate:
 *   - intact: every record links and hashes
 *   - a torn tail: a crash (the last record was never finished)
 *   - a broken link or a bad hash: some record does not match its neighbours
 *   - absent: there is no log at all, which is NOT the same as a clean one
 */
static int builtin_audit(sh_shell *sh, sargv *a, char **text_out,
                         char *err, size_t ecap) {
    if (a->n >= 2 && strcmp(a->v[1], "verify") != 0 && strcmp(a->v[1], "check") != 0) {
        snprintf(err, ecap, "usage: audit [verify]");
        return 1;
    }

    audit_check c = audit_verify(sh->store, 0);
    char head[AIOS_AUDIT_HEX + 1] = {0};
    (void)audit_head(sh->store, head);

    char line[768];
    switch (c.v) {
    case AUDIT_OK:
        snprintf(line, sizeof line,
                 "audit: %lld records, chained and intact\n"
                 "  head: %.16s  (the value a next session could commit to)\n"
                 "  limit: fnv1a64 is not cryptographic, and this chain is checked "
                 "only against itself -- an attacker who can rewrite the whole "
                 "file can rewrite the chain. See include/aios/audit.h",
                 c.records, head[0] ? head : "(none)");
        *text_out = strdup(line);
        return 0;

    case AUDIT_TORN_TAIL:
        snprintf(line, sizeof line,
                 "audit: unverified -- %.200s\n"
                 "  %lld complete records before the break, and that is all this "
                 "can say about them\n"
                 "  a torn tail is what a crash looks like; it is not reported as "
                 "tampering", c.why, c.records);
        *text_out = strdup(line);
        return 1;

    default:
        snprintf(line, sizeof line,
                 "audit: BROKEN at seq %lld -- %s: %.200s\n"
                 "  records before the break: %lld\n"
                 "  the file has been changed somewhere this cannot explain; the "
                 "only honest next step is to compare it with a copy you trust",
                 c.seq, audit_verdict_str(c.v), c.why, c.records);
        *text_out = strdup(line);
        return 1;
    }
}

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
    {
        /* A count is not a health report. "N tool calls" says the file has lines
         * in it; verification says whether those lines are the ones that were
         * written. The difference is the whole milestone. */
        audit_check ac = audit_verify(sh->store, 0);
        if (ac.v == AUDIT_OK) {
            char head[AIOS_AUDIT_HEX + 1] = {0};
            (void)audit_head(sh->store, head);
            /* %.16s was meant for a hash and got handed a sentence, so it printed
             * "(6 records, chain)" -- a diagnostic that looks truncated is worse
             * than none, because the reader assumes the rest was unimportant. */
            snprintf(line, sizeof line,
                     "\n  audit:      %zu tool calls, chain intact, head %.16s",
                     audit_n, head[0] ? head : "(none)");
        } else if (ac.v == AUDIT_TORN_TAIL) {
            snprintf(line, sizeof line,
                     "\n  audit:      %zu tool calls, tail torn (a crash, not an edit)",
                     audit_n);
        } else {
            snprintf(line, sizeof line,
                     "\n  audit:      %zu tool calls, CHAIN BROKEN at seq %lld "
                     "(%s) -- run `audit verify`", audit_n, ac.seq,
                     audit_verdict_str(ac.v));
        }
        sb_puts(&b, line);
    }
    snprintf(line, sizeof line, "\n  jobs:       %zu spawned / %zu journal records",
             spawns, jrecs);
    sb_puts(&b, line);

    /* M5: rollback is only real if a person can SEE it is available. A durable
     * store with no snapshots is one where `rollback` will refuse, and the shell
     * should say so here rather than at the moment someone needs it. */
    {
        snap_list sl;
        memset(&sl, 0, sizeof sl);
        if (snap_list_all(sh->store, &sl, NULL, 0)) {
            if (sl.n == 0) {
                snprintf(line, sizeof line,
                         "\n  snapshots:  none yet — `snap take <label>` "
                         "before a risky change");
            } else {
                const snap_info *newest = &sl.items[0];
                snprintf(line, sizeof line,
                         "\n  snapshots:  %zu (keeping %d), newest %.48s \"%.48s\" "
                         "— `rollback` undoes it",
                         sl.n, AIOS_SNAP_KEEP, newest->id, newest->label);
            }
            sb_puts(&b, line);
            snap_list_free(&sl);
        } else {
            sb_puts(&b, "\n  snapshots:  cannot be listed");
        }
    }
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

static int spawn_job(sh_shell *sh, const char *goal, aios_origin origin,
                     char **text_out, char *err, size_t ecap) {
    if (!*goal) { snprintf(err, ecap, "spawn needs a goal"); return 1; }
    /* M7: a spawn INHERITS the caller's origin, and a machine may not spawn.
     *
     * Both halves matter. Without inheritance, an AI line that spawned work would
     * produce a job whose origin is whatever the default happened to be -- and the
     * background work would then be judged by a different standard than the work
     * that asked for it. Without the refusal, "an AI may not spawn" would be a
     * policy rule (2c) that the shell's own grammar walked around: the rule
     * exists because an agent spawning agents is how a bounded action becomes an
     * unbounded swarm, and the shell is where spawn is spelled.
     *
     * The policy still has 2c, and this is not a replacement for it: a spawn that
     * got here by another route -- an agent loop, a routed intent -- is checked
     * there, with a descriptor. This is the door in the wall the policy lives
     * behind. */
    if (origin == ORIGIN_AI) {
        snprintf(err, ecap,
                 "a machine-originated line may not spawn background work: "
                 "an agent that starts agents is how a bounded request becomes "
                 "an unbounded swarm, and how the record stops being complete");
        sembus_pub_kv(sh->bus, "sh.action.refused_origin",
                      (const char *const[]){
                          "command", "spawn", "origin", "ai",
                          "why", "policy 2c: an AI may not spawn", NULL});
        return 1;
    }
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

static int builtin_spawn(sh_shell *sh, sargv *a, aios_origin origin,
                         char **text_out, char *err, size_t ecap) {
    char *goal = join_args(a->v, a->n, 1);
    int rc = spawn_job(sh, goal ? goal : "", origin, text_out, err, ecap);
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


/* ---- M7: the human's hand on the machine ---- *
 *
 * Some builtins do not compute a value; they make a change real, or they end the
 * session. `promote` accepts a machine's proposal. `rollback` rewinds the
 * machine. `discard` throws work away. `exit` ends the accountability.
 *
 * A model that can type these has not been mediated, it has been given the
 * keyboard: everything M3-M6 built assumes a person is the one accepting a
 * proposal, and an AI that can call `promote` makes the human a formality in its
 * own audit trail. So an AI-origin line cannot reach them, and the refusal says
 * so in words rather than returning "unknown command" -- a model that gets
 * "unknown" will try another spelling, and one that gets "a person has to do
 * this" has been told something true.
 *
 * This is enforced HERE, in the one place a command is dispatched, not in the
 * router: a guard that a rewrite of the router could drop is not a guard.
 *
 * The list is deliberately short and deliberate: it names the commands whose
 * effect is a change in what is real, and nothing else. Note what is NOT on it:
 * `policy` and `staged` are introspection. A model that can read the law and see
 * what it has pending is a model that can understand its own refusal, and
 * blinding it would make the machine less predictable without making it safer --
 * the thing being prevented is a change, not a reading.
 */
/* M7: run a line AS A MACHINE, inside this shell.
 *
 * This exists for the boot proof and for anyone who wants to ask "what would
 * happen if a model typed this" without spawning a second process. It is a
 * builtin rather than a spawn for a reason: the point is to exercise the SAME
 * shell, with its staging area and its holds, from a different origin. A
 * subprocess would start with an empty stage and prove much less.
 *
 * It is deliberately NOT powerful: the line is executed at ORIGIN_AI, so a model
 * that could reach this builtin would get nothing it does not already have --
 * and a human using it is asking a question, not delegating.
 */
static int builtin_as_machine(sh_shell *sh, sargv *a,
                              char **text_out, json_val **json_out,
                              char *err, size_t ecap);

static const char *const SH_HUMAN_ONLY[] = {
    "promote", "discard", "rollback", "snap", "exit", "quit", "cortex",
    "autopilot", "gc", "undelete", NULL,
};

static const char *human_only_reason(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "promote") == 0)
        return "accepting a staged change is a person's decision, not a model's";
    if (strcmp(name, "discard") == 0)
        return "throwing away a proposal is a person's decision";
    if (strcmp(name, "rollback") == 0)
        return "rewinding the machine is a person's decision";
    if (strcmp(name, "snap") == 0)
        return "deciding when state is captured is a person's decision";
    if (strcmp(name, "exit") == 0 || strcmp(name, "quit") == 0)
        return "a model may not end the session it is accountable to";
    if (strcmp(name, "cortex") == 0)
        return "a model may not choose which model answers next";
    if (strcmp(name, "autopilot") == 0)
        return "a model may not change the machine's own governor";
    if (strcmp(name, "gc") == 0)
        return "a model may not collect the record of what happened";
    if (strcmp(name, "undelete") == 0)
        return "restoring a deleted file is a person's decision";
    return NULL;
}

static bool sh_is_human_only(const char *name) {
    for (int i = 0; SH_HUMAN_ONLY[i]; i++)
        if (strcmp(SH_HUMAN_ONLY[i], name) == 0) return true;
    return false;
}

static int run_command(sh_shell *sh, scmd *c, aios_origin origin,
                       const char *pipe_text, json_val *pipe_json,
                       char **text_out, json_val **json_out,
                       char *err, size_t ecap) {
    (void)pipe_json; /* stages exchange text; JSON rides the pipeline */
    sargv *a = &c->argv;
    *text_out = NULL;
    *json_out = NULL;
    if (a->n == 0) { snprintf(err, ecap, "empty command in pipeline"); return 1; }
    const char *c0 = a->v[0];

    /* M7: before any builtin can run. See SH_HUMAN_ONLY above for why this is here
     * and not in the router. */
    if (origin == ORIGIN_AI && sh_is_human_only(c0)) {
        const char *why = human_only_reason(c0);
        snprintf(err, ecap,
                 "`%.32s` is not available to a machine-originated line: %.160s.\n"
                 "  This is the point of origin tagging: the shell knows the line "
                 "came from a model, and a model may not do this.",
                 c0, why ? why : "a person has to do it");
        sembus_pub_kv(sh->bus, "sh.action.refused_origin",
                      (const char *const[]){
                          "command", c0, "origin", "ai",
                          "why", why ? why : "human-only command", NULL});
        return 1;
    }

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
    if (strcmp(c0, "machine") == 0)
        return builtin_as_machine(sh, a, text_out, json_out, err, ecap);
    if (strcmp(c0, "discard") == 0) return builtin_discard(sh, a, text_out, json_out, err, ecap);
    if (strcmp(c0, "gc") == 0)      return builtin_gc(sh, text_out, json_out, err, ecap);
    if (strcmp(c0, "store") == 0)   return builtin_store(sh, a, text_out, err, ecap);
    if (strcmp(c0, "jobs") == 0)    return builtin_jobs(sh, text_out);
    if (strcmp(c0, "wait") == 0)    return builtin_wait(sh, a, text_out, err, ecap);
    if (strcmp(c0, "kill") == 0)    return builtin_kill(sh, a, text_out, err, ecap);
    if (strcmp(c0, "spawn") == 0)
        return builtin_spawn(sh, a, origin, text_out, err, ecap);
    if (strcmp(c0, "lm") == 0)      return builtin_lm(sh, a, text_out, err, ecap);
    if (strcmp(c0, "autopilot") == 0) return builtin_autopilot(sh, a, text_out, err, ecap);
    if (strcmp(c0, "tune") == 0)    return builtin_tune(sh, a, text_out, err, ecap);
    if (strcmp(c0, "dur") == 0)     return builtin_dur(sh, a, text_out, err, ecap);
    if (strcmp(c0, "audit") == 0)   return builtin_audit(sh, a, text_out, err, ecap);
    if (strcmp(c0, "snap") == 0)    return builtin_snap(sh, a, text_out, err, ecap);
    if (strcmp(c0, "rollback") == 0) return builtin_rollback(sh, a, text_out, err, ecap);
    if (strcmp(c0, "agent") == 0) {
        char *goal = join_args(a->v, a->n, 1);
        if (!*goal) { free(goal); snprintf(err, ecap, "agent needs a goal"); return 1; }
        /* Whoever typed `agent <goal>` chose the goal, so the origin is the
         * CALLER'S and not this line's to invent. M7 caught this: an AI-origin
         * line saying `agent x` was stamped ORIGIN_HUMAN here, which is the
         * milestone's whole failure mode in one hardcoded constant. */
        aios_action *act = aios_action_new(ACT_AGENT, origin, goal);
        int rc;
        if (!act) {
            snprintf(err, ecap, "oom (descriptor)");
            rc = 1;
        } else {
            aios_action_set_intent(act, c->raw ? c->raw : goal);
            rc = run_agent_fg(sh, act, origin, text_out, err, ecap);
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
        return run_tool_stage(sh, c0, a, origin, c->raw, pipe_text, text_out,
                              json_out, err, ecap);

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
        if (act) rc = exec_tool_action(sh, act, origin, text_out, json_out, err, ecap);
        break;
    case SH_ROUTE_AGENT:
        /* M7: background wording -> the agents cgroup, not the console */
        if (dec.bg) {
            /* A background job is its own proposal kind: it outlives the
             * command that asked for it, so it is gated as ACT_JOB_SPAWN. */
            rc = spawn_job(sh, dec.goal ? dec.goal : "", origin, text_out,
                           err, ecap);
        } else if (act) {
            rc = run_agent_fg(sh, act, origin, text_out, err, ecap);
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

static int run_pipeline(sh_shell *sh, spipe *pl, aios_origin origin,
                        char **text_out, json_val **json_out,
                        char *err, size_t ecap) {
    *text_out = NULL;
    *json_out = NULL;
    char *pipe_text = NULL;
    json_val *pipe_json = NULL;
    for (size_t i = 0; i < pl->n; i++) {
        char *ctext = NULL;
        json_val *cjson = NULL;
        int rc = run_command(sh, &pl->cmd[i], origin, pipe_text, pipe_json,
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
    /* M7: a shell has not been spoken to yet. calloc would leave this 0, and 0
     * is ORIGIN_HUMAN -- "no origin" and "a person" must not share a value, or
     * every fresh shell silently reports that its last line came from a human. */
    sh->last_origin = ORIGIN_UNSPECIFIED;
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
    /* A shell with a prompt is a person talking. Kept as a thin name because every
     * caller that is not a model means exactly this. */
    return sh_exec_origin(sh, line, ORIGIN_HUMAN, out);
}

aios_origin sh_last_origin(const sh_shell *sh) {
    return sh ? sh->last_origin : ORIGIN_UNSPECIFIED;
}

int sh_exec_origin(sh_shell *sh, const char *line, aios_origin origin,
                   sh_result *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->text = strdup("");
    if (!sh || !line) {
        out->status = 1;
        out->error = strdup("invalid sh_exec call");
        return 0;
    }
    /* ORIGIN_UNSPECIFIED is not a third kind of caller; it is a caller that did
     * not say. Refusing is the only safe reading: a default would be the silent
     * assumption this milestone exists to remove. */
    if (origin != ORIGIN_HUMAN && origin != ORIGIN_AI) {
        out->status = 1;
        out->error = strdup("sh_exec_origin needs ORIGIN_HUMAN or ORIGIN_AI");
        return 0;
    }
    sh->last_origin = origin;
    sh->last_needs_human = 0;
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
        int rc = run_pipeline(sh, &lines.pl[i], origin, &ptext, &pjson,
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
