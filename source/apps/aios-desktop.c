/* aios-desktop.c - M10 `desktop`: the AI-native graphical shell.
 *
 * The OS front-end as a desktop: wallpaper, a launcher bar whose input
 * box speaks natural language to the SAME aiosh engine the serial console
 * uses (micro-LM cortex, no network), and app windows - terminal
 * (aiosh REPL), files, agents, memory, autopilot. Semibus events surface
 * as toasts. The serial console stays alive for CI markers; the text
 * front-end remains available via aios.gui=0.
 *
 * Assets: /usr/lib/aios/assets/ .aiosimg files (artist PNGs converted at build
 * time). Every asset has a procedural fallback (src/fb.c) - the desktop
 * boots with a full face even with zero artwork.
 *
 * CI: prints "AIOS-DESKTOP-OK w=<w> h=<h>" on stdout (the serial console)
 * after the first successful composite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "aios/fb.h"
#include "aios/win.h"
#include "aios/input.h"
#include "aios/stage.h"
#include "aios/audit.h"
#include "aios/shell.h"
#include "aios/sembus.h"
#include "aios/json.h"
#include "aios/mem.h"
#include "aios/types.h"

/* ---------------- palette ------------------------------------------ */

#define COL_BAR_BG     0xFF161c2au
/* M8: the pending badge sits immediately left of the five app slots, which
 * begin at w-440. It is a badge and a click target, NOT a sixth slot with an
 * icon: RULES §2.4 pins the launcher at five, and i=5 would land off a 1280
 * screen. Computed from the width because the bar is laid out from the right. */
#define BAR_PEND_GAP   8
#define BAR_PEND_W     96
static int bar_pend_x(int w) { return w - 440 - BAR_PEND_GAP - BAR_PEND_W; }
#define COL_BAR_LINE   AIOS_RGB(0x4a, 0xe0, 0x8a)
#define COL_TEXT       AIOS_RGB(0xe8, 0xee, 0xf8)
#define COL_DIM        AIOS_RGB(0x8a, 0x98, 0xb2)
#define COL_ACCENT     AIOS_RGB(0x4a, 0xe0, 0x8a)
#define COL_INPUT_BG   AIOS_RGB(0x1e, 0x26, 0x38)
#define COL_TERM_FG    AIOS_RGB(0xd4, 0xe0, 0xf0)
#define COL_TERM_BG    AIOS_RGB(0x10, 0x14, 0x1d)
#define COL_TERM_DIM   AIOS_RGB(0x62, 0x70, 0x8c)

#define BAR_H 64
#define MAXLINES 64
#define LINELEN 160

/* ---------------- desktop state ------------------------------------- */

typedef enum {
    APP_TERMINAL = 0, APP_FILES, APP_AGENTS, APP_MEMORY, APP_AUTOPILOT,
    APP_COUNT
} app_kind;

static const struct { const char *asset; const char *title; } APP_META[APP_COUNT] = {
    { "icon-terminal",  "terminal"  },
    { "icon-files",     "files"     },
    { "icon-agent",     "agents"    },
    { "icon-memory",    "memory"    },
    { "icon-autopilot", "autopilot" },
};

typedef struct {
    aios_win *win;
    app_kind kind;
} app_slot;

typedef struct {
    aios_fb   *fb;
    aios_wm   *wm;
    aios_input *input;
    sh_shell  *sh;                  /* the desktop's aiosh (micro cortex) */
    sembus    *bus;                 /* borrowed from sh                   */
    app_slot   apps[APP_COUNT];
    /* launcher */
    char       nl[200];             /* natural-language input             */
    int        nl_focus;
    /* M7: WHO typed the launcher line. A person at the keyboard is the only
     * producer today, but UI-07 adds an AI-driven input path, and the point of a
     * field is that the future path sets it explicitly instead of inheriting a
     * default that happens to be "human". Declared as human at creation, so the
     * status quo is written down rather than assumed. */
    aios_origin nl_origin;
    /* M7: a held action, waiting for a person. `hold_win` is the window that
     * asks; while it is open, keys go to it and nowhere else. */
    aios_win   *hold_win;
    ui_prompt   hold;
    ui_choice   hold_answer;
    int         hold_pending;
    /* M8: the pending-changes window. A sixth WINDOW, not a sixth launcher slot:
     * RULES §2.4 pins the launcher at five at `w-440+i*86`, and i=5 would land
     * off a 1280-wide screen. It opens over everything when changes are waiting,
     * which is where a decision belongs -- not in a bar you have to go looking
     * in. */
    aios_win   *pend_win;
    int         pend_sel;
    int         pend_open_req;
    /* toasts */
    char       toast[4][96];
    double     toast_until[4];
    int        toast_n;
    double     now_ms;
    int        ok_printed;
    int        caret;               /* blinking input caret               */
} desktop;

/* ---------------- helpers ------------------------------------------- */

static void toast(desktop *d, const char *fmt, ...) {
    va_list ap;
    char buf[96];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (d->toast_n == 4) {          /* overwrite oldest */
        memmove(d->toast[0], d->toast[1], 3 * 96);
        d->toast_n = 3;
    }
    snprintf(d->toast[d->toast_n], 96, "%s", buf);
    d->toast_until[d->toast_n] = d->now_ms + 6000;
    d->toast_n++;
    aios_wm_damage_all(d->wm);
}

/* scrollback: ring of lines per terminal window (stored in canvas user) */
typedef struct {
    char lines[MAXLINES][LINELEN];
    int head, count;
    char line[LINELEN];             /* current input line                 */
    char hist[16][LINELEN];
    int hist_n, hist_idx;
} term_buf;

static void term_push(term_buf *t, const char *s) {
    /* word-split long lines into LINELEN chunks */
    while (*s) {
        char chunk[LINELEN];
        size_t n = strlen(s);
        if (n > LINELEN - 1) {
            memcpy(chunk, s, LINELEN - 1);
            chunk[LINELEN - 1] = 0;
            s += LINELEN - 1;
        } else {
            snprintf(chunk, sizeof chunk, "%s", s);
            s += n;
        }
        snprintf(t->lines[t->head], LINELEN, "%s", chunk);
        t->head = (t->head + 1) % MAXLINES;
        if (t->count < MAXLINES) t->count++;
    }
}

static const char *term_line(const term_buf *t, int i) {
    /* i: 0 = oldest visible */
    int idx = (t->head - t->count + i + MAXLINES * 2) % MAXLINES;
    return t->lines[idx];
}

/* ---------------- assets -------------------------------------------- */

static aios_img *asset_load(desktop *d, const char *name,
                            aios_img *(*fallback)(int, int), int w, int h,
                            int logo_mode) {
    (void)logo_mode;
    char path[256];
    const char *dir = getenv("AIOS_ASSETS");
    if (!dir || !*dir) dir = "/usr/lib/aios/assets";
    snprintf(path, sizeof path, "%s/%s.aiosimg", dir, name);
    aios_img *im = aios_img_load(path);
    if (im) {
        fprintf(stderr, "[desktop] asset %s: artist image %dx%d\n",
                name, aios_img_w(im), aios_img_h(im));
        return im;
    }
    (void)d;
    if (fallback) {
        im = fallback(w, h);
        fprintf(stderr, "[desktop] asset %s: fallback fn\n", name);
        return im;
    }
    fprintf(stderr, "[desktop] asset %s: procedural fallback\n", name);
    return NULL;
}

/* ---------------- terminal window ----------------------------------- */

static void term_render(desktop *d) {
    aios_win *w = d->apps[APP_TERMINAL].win;
    term_buf *t = aios_win_user(w);
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, COL_TERM_BG);
    int rows = (ch - 4) / 16;
    int first = t->count > rows ? t->count - rows + 1 : 0;
    for (int i = first; i < t->count; i++) {
        int row = 2 + (i - first) * 16;
        const char *ln = term_line(t, i);
        int is_out = strncmp(ln, "aiosh> ", 7) != 0;
        aios_win_text(w, 6, row, ln,
                      is_out ? COL_TERM_FG : COL_ACCENT, 0, !is_out);
    }
    /* input line + caret */
    char prompt[LINELEN + 16];
    snprintf(prompt, sizeof prompt, "aiosh> %s", t->line);
    int row = 2 + (rows - 1) * 16;
    if (row > ch - 20) row = ch - 20;
    aios_win_text(w, 6, row, prompt, COL_TEXT, 0, 1);
    if (d->caret)
        aios_win_fill(w, 6 + aios_fb_text_w(prompt), row, 7, 15, COL_ACCENT);
    aios_wm_damage_all(d->wm);
}

/* M7: the terminal's text box is a person typing. Spelled out once, here, so
 * that there is exactly one place in the desktop that claims a human did
 * something -- and so that adding a second caller makes you read this. */
static void term_exec_origin(desktop *d, const char *cmdline,
                             aios_origin origin);

static void term_exec(desktop *d, const char *cmdline) {
    term_exec_origin(d, cmdline, ORIGIN_HUMAN);
}

static void term_exec_origin(desktop *d, const char *cmdline,
                             aios_origin origin) {
    aios_win *w = d->apps[APP_TERMINAL].win;
    term_buf *t = aios_win_user(w);
    char shown[LINELEN];
    snprintf(shown, sizeof shown, "aiosh> %.150s", cmdline);
    term_push(t, shown);
    sh_result r;
    int exit_req = sh_exec_origin(d->sh, cmdline, origin, &r);
    if (r.error && *r.error) {
        term_push(t, "error:");
        term_push(t, r.error);
    }
    if (r.text && *r.text) term_push(t, r.text);
    else if ((!r.error || !*r.error) && (!r.text || !*r.text))
        term_push(t, "(ok)");
    sh_result_free(&r);
    (void)exit_req;                  /* the desktop does not exit on 'exit' */
    /* agents finishing surface as toasts via the bus; job count in bar */
    term_render(d);
}

static void term_key(desktop *d, int key) {
    aios_win *w = d->apps[APP_TERMINAL].win;
    term_buf *t = aios_win_user(w);
    if (key == '\n' || key == AIOS_KEY_F(5)) {
        if (t->line[0]) {
            char cmd[LINELEN];
            snprintf(cmd, sizeof cmd, "%s", t->line);
            snprintf(t->hist[t->hist_n % 16], LINELEN, "%s", cmd);
            t->hist_n++;
            t->hist_idx = 0;
            t->line[0] = 0;
            term_exec(d, cmd);
        }
        return;
    }
    if (key == 0x08 || key == 0x7f) {
        size_t n = strlen(t->line);
        if (n) t->line[n - 1] = 0;
        term_render(d);
        return;
    }
    if (key == AIOS_KEY_UP) {
        if (t->hist_n) {
            int idx = (t->hist_idx == 0) ? t->hist_n - 1
                                         : (t->hist_idx - 1);
            t->hist_idx = idx;
            snprintf(t->line, sizeof t->line, "%s", t->hist[idx % 16]);
            term_render(d);
        }
        return;
    }
    if (key == AIOS_KEY_DOWN) {
        if (t->hist_n) {
            t->hist_idx = (t->hist_idx + 1) % t->hist_n;
            snprintf(t->line, sizeof t->line, "%s", t->hist[t->hist_idx % 16]);
            term_render(d);
        }
        return;
    }
    if (key >= 32 && key <= 126) {
        size_t n = strlen(t->line);
        if (n < LINELEN - 1) {
            t->line[n] = (char)key;
            t->line[n + 1] = 0;
            term_render(d);
        }
    }
}

/* ---------------- files window -------------------------------------- */

typedef struct {
    char path[512];
    char names[200][80];
    int is_dir[200];
    int count, sel, scroll;
} files_buf;

static void files_reload(files_buf *f) {
    f->count = f->sel = f->scroll = 0;
    DIR *d = opendir(f->path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && f->count < 199) {
        if (!strcmp(e->d_name, ".") || strlen(e->d_name) > 62) continue;
        snprintf(f->names[f->count], 80, "%.62s", e->d_name);
        struct stat st;
        char full[600];
        snprintf(full, sizeof full, "%s/%.62s", f->path, e->d_name);
        f->is_dir[f->count] =
            (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            || !strcmp(e->d_name, "..");
        f->count++;
    }
    closedir(d);
    /* simple sort: dirs first, then alphabetical (n is small) */
    for (int i = 0; i < f->count; i++)
        for (int j = i + 1; j < f->count; j++) {
            int di = f->is_dir[i] ? 0 : 1, dj = f->is_dir[j] ? 0 : 1;
            if (di > dj || (di == dj && strcmp(f->names[i], f->names[j]) > 0)) {
                char tn[80]; memcpy(tn, f->names[i], 80);
                memcpy(f->names[i], f->names[j], 80);
                memcpy(f->names[j], tn, 80);
                int td = f->is_dir[i]; f->is_dir[i] = f->is_dir[j];
                f->is_dir[j] = td;
            }
        }
}

static void files_render(desktop *d) {
    aios_win *w = d->apps[APP_FILES].win;
    files_buf *f = aios_win_user(w);
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, 0xFF121826u);
    aios_win_text(w, 6, 4, f->path, COL_DIM, 0, 0);
    int rows = (ch - 28) / 16;
    for (int i = 0; i < rows && f->scroll + i < f->count; i++) {
        int idx = f->scroll + i;
        int y = 24 + i * 16;
        int selrow = (idx == f->sel);
        if (selrow)
            aios_win_fill(w, 2, y - 2, cw - 4, 15, 0xFF223048u);
        char row[80];
        snprintf(row, sizeof row, "%s%s", f->names[idx],
                 f->is_dir[idx] ? "/" : "");
        aios_win_text(w, 8, y, row, f->is_dir[idx] ? COL_ACCENT : COL_TEXT,
                      0, selrow);
    }
    char foot[80];
    snprintf(foot, sizeof foot, "%d items, dirs in green; enter opens",
             f->count);
    aios_win_text(w, 6, ch - 20, foot, COL_TERM_DIM, 0, 0);
    aios_wm_damage_all(d->wm);
}

static void files_key(desktop *d, int key) {
    aios_win *w = d->apps[APP_FILES].win;
    files_buf *f = aios_win_user(w);
    int rows = (aios_win_ch(w) - 28) / 16;
    if (key == AIOS_KEY_UP && f->sel > 0) f->sel--;
    else if (key == AIOS_KEY_DOWN && f->sel < f->count - 1) f->sel++;
    else if (key == AIOS_KEY_PGUP) f->sel -= rows;
    else if (key == AIOS_KEY_PGDN) f->sel += rows;
    else if (key == '\n') {
        if (f->count && f->is_dir[f->sel]) {
            if (!strcmp(f->names[f->sel], "..")) {
                char *slash = strrchr(f->path, '/');
                if (slash && slash != f->path) *slash = 0;
            } else {
                char next[512];
                snprintf(next, sizeof next, "%.448s/%.62s", f->path,
                         f->names[f->sel]);
                snprintf(f->path, sizeof f->path, "%s", next);
            }
            files_reload(f);
        }
    } else return;
    if (f->sel < 0) f->sel = 0;
    if (f->sel > f->count - 1) f->sel = f->count - 1 ? f->count - 1 : 0;
    if (f->sel < f->scroll) f->scroll = f->sel;
    if (f->sel >= f->scroll + rows) f->scroll = f->sel - rows + 1;
    files_render(d);
}

/* ---------------- agents window ------------------------------------- */

typedef struct {
    char rows[12][96];
    int count;
} agents_buf;

static void agents_reload(desktop *d, agents_buf *a) {
    a->count = 0;
    sh_poll_jobs(d->sh);
    const char *store = getenv("AIOS_STORE");
    if (!store || !*store) store = "/var/lib/aios/store";
    char dirpath[300];
    snprintf(dirpath, sizeof dirpath, "%s/jobs", store);
    DIR *dir = opendir(dirpath);
    if (!dir) {
        snprintf(a->rows[a->count < 12 ? a->count++ : 11], 96,
                 "(no job store yet - spawn an agent)");
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && a->count < 11) {
        const char *dot = strstr(e->d_name, ".json");
        if (!dot || strlen(e->d_name) > 40) continue;
        char full[400];
        snprintf(full, sizeof full, "%s/%.40s", dirpath, e->d_name);
        json_val *j = json_parse_file(full);
        if (!j) continue;
        json_val *goal = json_obj_get(j, "goal");
        json_val *status = json_obj_get(j, "status");
        snprintf(a->rows[a->count++], 96, "%.28s %.8s  %s",
                 dot - e->d_name > 28 ? e->d_name + (dot - e->d_name - 28)
                                      : e->d_name,
                 status ? json_str(status) : "?",
                 goal ? json_str(goal) : "");
        json_free(j);
    }
    closedir(dir);
    if (!a->count)
        snprintf(a->rows[a->count++], 96, "(no agents yet - try: spawn <goal>)");
}

static void agents_render(desktop *d) {
    aios_win *w = d->apps[APP_AGENTS].win;
    agents_buf *a = aios_win_user(w);
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, 0xFF121a26u);
    aios_win_text(w, 6, 4, "background agents (jobs)", COL_DIM, 0, 1);
    for (int i = 0; i < a->count; i++)
        aios_win_text(w, 6, 26 + i * 16, a->rows[i],
                      i == 0 && a->rows[i][0] == '(' ? COL_TERM_DIM : COL_TEXT,
                      0, 0);
    size_t alive = sh_jobs_alive(d->sh);
    char foot[64];
    snprintf(foot, sizeof foot, "alive: %zu", alive);
    aios_win_text(w, 6, ch - 20, foot, COL_ACCENT, 0, 1);
    aios_wm_damage_all(d->wm);
}

/* ---------------- memory window ------------------------------------- */

typedef struct {
    char input[160];
    char out[6][160];
    int out_n;
} mem_buf;

static void mem_render(desktop *d) {
    aios_win *w = d->apps[APP_MEMORY].win;
    mem_buf *m = aios_win_user(w);
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, 0xFF171322u);
    aios_win_text(w, 6, 4, "OS memory (remember / recall)", COL_DIM, 0, 1);
    aios_win_fill(w, 6, 24, cw - 12, 20, COL_INPUT_BG);
    aios_win_text(w, 10, 26, m->input, COL_TEXT, 0, 0);
    if (d->caret)
        aios_win_fill(w, 10 + aios_fb_text_w(m->input), 26, 7, 15, COL_ACCENT);
    for (int i = 0; i < m->out_n && i < 6; i++)
        aios_win_text(w, 6, 52 + i * 16, m->out[i],
                      strncmp(m->out[i], "stored", 6) == 0 ? COL_ACCENT
                                                              : COL_TEXT,
                      0, 0);
    aios_win_text(w, 6, ch - 20,
                  "type> text to remember | ?query to recall", COL_TERM_DIM,
                  0, 0);
    aios_wm_damage_all(d->wm);
}

static void mem_exec(desktop *d, const char *s) {
    aios_win *w = d->apps[APP_MEMORY].win;
    mem_buf *m = aios_win_user(w);
    mem_ctx *mem = sh_mem(d->sh);
    if (s[0] == '?') {
        mem_entry *ents = NULL;
        int n = mem ? mem_recall(mem, s + 1, 5, &ents) : 0;
        m->out_n = 0;
        if (n <= 0) {
            snprintf(m->out[m->out_n++ % 6], 160, "(no recall hits)");
        }
        for (int i = 0; i < n && m->out_n < 6; i++)
            snprintf(m->out[m->out_n++ % 6], 160, "%.2f %s",
                     ents[i].score, ents[i].text);
        free(ents);
    } else if (s[0]) {
        aios_status st = mem ? mem_remember(mem, MEM_SEMANTIC, s, NULL)
                             : AIOS_ERR;
        snprintf(m->out[m->out_n++ % 6], 160, "stored: %.120s (%s)",
                 s, st == AIOS_OK ? "ok" : "err");
    }
    mem_render(d);
}

static void mem_key(desktop *d, int key) {
    aios_win *w = d->apps[APP_MEMORY].win;
    mem_buf *m = aios_win_user(w);
    if (key == '\n') {
        if (m->input[0]) {
            char cmd[160];
            snprintf(cmd, sizeof cmd, "%s", m->input);
            m->input[0] = 0;
            mem_exec(d, cmd);
        }
        return;
    }
    if (key == 0x08 || key == 0x7f) {
        size_t n = strlen(m->input);
        if (n) m->input[n - 1] = 0;
        mem_render(d);
        return;
    }
    if (key >= 32 && key <= 126) {
        size_t n = strlen(m->input);
        if (n < 150) {
            m->input[n] = (char)key;
            m->input[n + 1] = 0;
            mem_render(d);
        }
    }
}

/* ---------------- autopilot window ---------------------------------- */

static void ap_render(desktop *d) {
    aios_win *w = d->apps[APP_AUTOPILOT].win;
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, 0xFF101a18u);
    aios_win_text(w, 6, 4, "autopilot (closed-loop scheduler)", COL_DIM, 0, 1);
    json_val *st = json_parse_file("/var/lib/aios/autopilot-state.json");
    int row = 26;
    if (!st) {
        aios_win_text(w, 6, row, "(state file not present yet)", COL_TERM_DIM,
                      0, 0);
        aios_win_text(w, 6, row + 16,
                      "governor writes it every tick", COL_TERM_DIM, 0, 0);
    } else {
        char line[96];
        json_val *v;
        v = json_obj_get(st, "mode");
        snprintf(line, sizeof line, "mode: %s", v ? json_str(v) : "?");
        aios_win_text(w, 6, row, line, COL_ACCENT, 0, 1);
        v = json_obj_get(st, "tick");
        snprintf(line, sizeof line, "tick: %d", (int)json_num(v));
        aios_win_text(w, 6, row + 18, line, COL_TEXT, 0, 0);
        v = json_obj_get(st, "tools_pct");
        double tp = json_num(v);
        v = json_obj_get(st, "agents_pct");
        double gp = json_num(v);
        snprintf(line, sizeof line, "budgets: tools %.0f%%  agents %.0f%%",
                 tp, gp);
        aios_win_text(w, 6, row + 36, line, COL_TEXT, 0, 0);
        /* budget bars */
        aios_win_fill(w, 6, row + 58, cw - 12, 10, 0xFF243040u);
        int tw = (int)((cw - 12) * tp / 100.0);
        aios_win_fill(w, 6, row + 58, tw, 4, COL_ACCENT);
        aios_win_fill(w, 6, row + 64, (int)((cw - 12) * gp / 100.0), 4,
                      AIOS_RGB(0x6a, 0xb8, 0xe8));
        v = json_obj_get(st, "reason");
        snprintf(line, sizeof line, "last: %.60s", v ? json_str(v) : "-");
        aios_win_text(w, 6, row + 76, line, COL_TERM_DIM, 0, 0);
        v = json_obj_get(st, "adjustments_total");
        snprintf(line, sizeof line, "adjustments: %d", (int)json_num(v));
        aios_win_text(w, 6, row + 94, line, COL_TEXT, 0, 0);
        json_free(st);
    }
    aios_win_text(w, 6, ch - 20, "change mode: aios.autopilot= in cmdline",
                  COL_TERM_DIM, 0, 0);
    aios_wm_damage_all(d->wm);
}

/* ---------------- M7: a held action, asked in a window --------------- *
 *
 * The serial console asks the same question with the same three words (see
 * ui_present in the shell). A desktop that decided differently -- or worse, that
 * quietly discarded because nobody wired it up -- would make the milestone's
 * promise depend on which front-end you happened to use.
 *
 * No timeout. The window stays until a key is pressed, and closing it is a
 * discard, because closing a question is not answering it.
 */
static void hold_render(desktop *d) {
    aios_win *w = d->hold_win;
    if (!w) return;
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, 0xFF1a1410u);        /* amber-ish: attention */
    aios_win_text(w, 6, 4, "A CHANGE IS WAITING FOR YOU", COL_ACCENT, 0, 1);
    aios_win_text(w, 6, 26, d->hold.title, COL_TEXT, 0, 1);
    aios_win_text(w, 6, 46, d->hold.reason, COL_DIM, 0, 0);
    char who[80];
    snprintf(who, sizeof who, "proposed by: %s",
             d->hold.origin == ORIGIN_AI ? "a model" : "a person");
    aios_win_text(w, 6, 66, who,
                  d->hold.origin == ORIGIN_AI ? AIOS_RGB(0xe8, 0xb8, 0x6a)
                                              : COL_DIM, 0, 0);
    if (d->hold.have_detail)
        aios_win_text(w, 6, 86, d->hold.detail, COL_TERM_DIM, 0, 0);
    int y = ch - 66;
    aios_win_text(w, 6, y, "[P] promote    [D] discard    [S] show detail",
                  COL_TEXT, 0, 1);
    aios_win_text(w, 6, y + 18, "closing this window = discard (nothing runs)", COL_TERM_DIM,
                  0, 0);
    aios_wm_damage_all(d->wm);
}

/* The asker the shell calls. It BLOCKS? No -- it cannot, the shell runs inline
 * on the desktop's own thread, and a blocking asker would deadlock against the
 * very event loop that reads the keystroke.
 *
 * So the answer is DEFER, and it is a real answer: the shell keeps the pending
 * action alive (sh_deferred()) instead of discarding it. The window opens now,
 * the loop keeps running, and the keystroke arrives later -- on the next tick,
 * through hold_key(), which calls sh_resume() to apply the person's answer to the
 * action the shell actually held.
 *
 * This used to answer DISCARD and open a window anyway. That was worse than a
 * missing feature: action_gate() discarded the action *before* the window
 * opened, so a person could read a proposal, press P, watch a toast say "re-run
 * the command to apply it", and have nothing happen. The question was real; the
 * answer went nowhere. */
static ui_choice desktop_ask(const ui_prompt *p, void *ud) {
    desktop *d = ud;
    d->hold = *p;
    d->hold_pending = 1;
    d->hold_answer = UI_CHOICE_DEFER;
    return UI_CHOICE_DEFER;
}

static void hold_open(desktop *d) {
    if (!d->hold_pending || d->hold_win) return;
    int W = aios_fb_w(d->fb);
    aios_rect r = { W / 2 - 240, 160, 480, 240 };
    d->hold_win = aios_wm_open(d->wm, r, "confirm a held action", NULL);
    d->hold_pending = 0;
    hold_render(d);
}

static void hold_key(desktop *d, int key) {
    if (key == 'p' || key == 'P') {
        d->hold_answer = UI_CHOICE_PROMOTE;
    } else if (key == 's' || key == 'S') {
        /* "show detail" re-renders the detail the prompt already carried. It is
         * deliberately NOT a content diff: a held ACTION has a descriptor, not a
         * staging session, so there are no bytes to compare. The pending-changes
         * window (`pending` / `diff <id>`) is where a staged change gets read, and
         * it is a different question: "what would this change?" versus "what did
         * this proposal ask to do?".
         *
         * Note the return: asking to look must not close the window, and must not
         * be mistaken for an answer. */
        d->hold_answer = UI_CHOICE_SHOW_DIFF;
        return;
    } else if (key == 'd' || key == 'D' || key == 27) {  /* 27 = ESC */
        d->hold_answer = UI_CHOICE_DISCARD;
    } else {
        return;
    }

    ui_choice c = d->hold_answer;
    aios_wm_close(d->wm, d->hold_win);
    d->hold_win = NULL;

    /* The person chose. Say so on the bus, and say which way: an audit that
     * records "a human was involved" without recording the answer is a record of
     * a conversation, not of a decision. */
    const char *cname = c == UI_CHOICE_PROMOTE ? "promote"
                      : c == UI_CHOICE_SHOW_DIFF ? "show_detail" : "discard";
    char ev[320];
    snprintf(ev, sizeof ev, "{\"what\":\"%.120s\",\"choice\":\"%s\"}",
             d->hold.title, cname);
    sembus_pub(d->bus, "sh.ui.hold_answered", ev);
    toast(d, "you chose: %s", cname);

    /* The answer has to reach the action, not just the screen.
     *
     * sh_resume() is where the held descriptor lives: PROMOTE runs it, DISCARD
     * drops it, and anything else leaves it held. The desktop does not re-run the
     * command and does not re-derive the action -- a re-run would have to be
     * gated again, would take a different descriptor id, and would quietly apply
     * whatever the store looked like *now* rather than what the person was shown.
     * ("Show detail" never gets here: hold_key() returns before closing.) */
    if (c == UI_CHOICE_PROMOTE || c == UI_CHOICE_DISCARD) {
        sh_result r;
        memset(&r, 0, sizeof r);
        int rc = sh_resume(d->sh, c, &r);
        /* rc is "did the answer get applied", not "was the answer yes": a
         * discard applies cleanly and still has something to say. The message
         * is what the person reads either way. */
        if (r.error && *r.error)
            toast(d, "%.150s", r.error);
        else if (r.text && *r.text)
            toast(d, "%.150s", r.text);
        else if (rc != 0)
            toast(d, "the held action could not be applied");
        sh_result_free(&r);
        aios_wm_damage_all(d->wm);
    }
}

/* ---------------- M8: pending changes --------------------------------- *
 *
 * In the console the same information is `pending` / `diff <id>`: a list of
 * changes, and one change read on its own. This window is that, and it uses the
 * same engine calls -- stage_pending_scan() and stage_diff_compute() -- so the
 * two front-ends cannot drift into describing different changes.
 *
 * Layout is on the 8x16 text grid, as everything else is: one row per change
 * (id, origin, summary), and the selected change's diff underneath with a green
 * "+" per added line and a red "-" per removed one. The counts come out of
 * stage_diff; the row itself is drawn as the files it names, because inventing
 * line content here would be a picture of a diff rather than a diff.
 */

/* The store the shell was built with. The desktop already reads this env var in
 * main() and in agents_reload(); one accessor keeps the window from inventing a
 * third spelling of the same path. */
static const char *desk_store(void) {
    const char *s = getenv("AIOS_STORE");
    return (s && *s) ? s : "/var/lib/aios/store";
}

#define PEND_MAX_ROWS 32

typedef struct {
    stage_pending_list l;
    stage_diff         diff;
    int                diff_ok;    /* diff below belongs to `diff_for` */
    int                diff_for;   /* index in l.items               */
    char               err[200];
} pend_buf;

/* An id like "1737...-1" is stable but unreadable. Show the tail, which is the
 * part that distinguishes two changes in the same run, and keep the whole thing
 * for the promote call. */
static const char *pend_short_id(const char *id) {
    size_t n = strlen(id);
    return n > 18 ? id + n - 18 : id;
}

static void pend_reload(desktop *d, pend_buf *pb) {
    stage_pending_free(&pb->l);
    memset(&pb->l, 0, sizeof pb->l);
    char err[200] = {0};
    if (!stage_pending_scan(desk_store(), &pb->l, err, sizeof err)) {
        snprintf(pb->err, sizeof pb->err, "cannot list pending: %.150s", err);
        pb->l.n = 0;
        return;
    }
    pb->err[0] = 0;
    if (d->pend_sel < 0) d->pend_sel = 0;
    if (pb->l.n == 0) d->pend_sel = 0;
    else if ((size_t)d->pend_sel >= pb->l.n) d->pend_sel = (int)pb->l.n - 1;
}

static void pend_diff_load(desktop *d, pend_buf *pb) {
    stage_diff_free(&pb->diff);
    memset(&pb->diff, 0, sizeof pb->diff);
    pb->diff_ok = 0;
    pb->diff_for = -1;
    if (pb->l.n == 0) return;
    int i = d->pend_sel;
    if (i < 0 || (size_t)i >= pb->l.n) return;
    stage_session s;
    char err[200] = {0};
    if (!stage_open(pb->l.items[i].dir, &s, err, sizeof err)) return;
    if (stage_diff_compute(&s, &pb->diff, err, sizeof err)) {
        pb->diff_ok = 1;
        pb->diff_for = i;
    }
    stage_end(&s);
}

static void pend_render(desktop *d) {
    aios_win *w = d->pend_win;
    if (!w) return;
    pend_buf *pb = aios_win_user(w);
    int cw = aios_win_cw(w), ch = aios_win_ch(w);
    aios_win_fill(w, 0, 0, cw, ch, 0xFF121826u);

    if (pb->l.n == 0) {
        aios_win_text(w, 8, 8, "nothing is pending", COL_TEXT, 0, 1);
        aios_win_text(w, 8, 28, "a staged change appears here before it can",
                      COL_DIM, 0, 0);
        aios_win_text(w, 8, 44, "be promoted into the jail.", COL_DIM, 0, 0);
        aios_win_text(w, 8, ch - 20, "esc closes", COL_TERM_DIM, 0, 0);
        aios_wm_damage_all(d->wm);
        return;
    }

    char head[96];
    snprintf(head, sizeof head, "%zu pending change%s", pb->l.n,
             pb->l.n == 1 ? "" : "s");
    aios_win_text(w, 8, 4, head, COL_ACCENT, 0, 1);

    /* rows */
    int y = 26;
    size_t nshow = pb->l.n < PEND_MAX_ROWS ? pb->l.n : PEND_MAX_ROWS;
    int list_h = (int)nshow * 16;
    for (size_t i = 0; i < nshow; i++) {
        stage_pending *p = &pb->l.items[i];
        int sel = ((int)i == d->pend_sel);
        int ry = y + (int)i * 16;
        if (sel) aios_win_fill(w, 4, ry - 2, cw - 8, 15, 0xFF223048u);
        /* id, what it does, and the AI badge. The badge is a separate draw at
         * the right edge so it reads as a mark on the row rather than as part of
         * the change's name. */
        char row[240];
        if (p->summary[0])
            snprintf(row, sizeof row, "%-18s  %.36s", pend_short_id(p->id),
                     p->summary);
        else
            snprintf(row, sizeof row, "%-18s  %zu file%s", pend_short_id(p->id),
                     p->n_files, p->n_files == 1 ? "" : "s");
        aios_win_text(w, 8, ry, row,
                      sel ? COL_TEXT : COL_DIM, 0, sel);
        if (audit_stage_record(desk_store(), p->id) >= 0)
            aios_win_text(w, cw - 40, ry, "AI", AIOS_RGB(0xe8, 0xb8, 0x6a), 0, 1);
    }

    /* diff pane: the selected change, read on its own */
    int dy = y + list_h + 8;
    if (dy + 40 < ch) {
        aios_win_fill(w, 4, dy - 4, cw - 8, 1, 0x3a4458u);
        if (pb->diff_ok && pb->diff_for == d->pend_sel) {
            int py = dy + 2, rows = 0;
            for (size_t i = 0; i < pb->diff.n && rows < (ch - py - 36) / 16; i++) {
                stage_diff_entry *e = &pb->diff.entries[i];
                const char *op = e->op == STAGE_OP_ADD ? "+"
                               : e->op == STAGE_OP_DELETE ? "-" : "~";
                unsigned col = e->op == STAGE_OP_ADD ? AIOS_RGB(0x5a, 0xd6, 0x8a)
                             : e->op == STAGE_OP_DELETE ? AIOS_RGB(0xe8, 0x6a, 0x7a)
                             : COL_TEXT;
                char line[256];
                /* %.40s, not %-40s: a path can be 512 bytes and the window is
                 * 520 wide, so the long name is cut, not the row. */
                if (e->op == STAGE_OP_DELETE)
                    snprintf(line, sizeof line, "- %.40s  removed", e->path);
                else
                    snprintf(line, sizeof line, "%s %.40s  %+ld bytes  +%d/-%d",
                             op, e->path, e->bytes, e->plus, e->minus);
                aios_win_text(w, 8, py, line, col, 0, 0);
                py += 16;
                rows++;
            }
            if (pb->diff.n == 0)
                aios_win_text(w, 8, py, "(no file differences recorded)",
                              COL_TERM_DIM, 0, 0);
        } else {
            aios_win_text(w, 8, dy + 2, "(cannot read this change's diff)",
                          COL_TERM_DIM, 0, 0);
        }
    }
    char foot[120];
    snprintf(foot, sizeof foot, "[p] promote   [d] discard   up/down select");
    aios_win_text(w, 8, ch - 20, foot, COL_TEXT, 0, 1);
    aios_wm_damage_all(d->wm);
}

static void pend_open(desktop *d) {
    if (d->pend_win) { aios_wm_focus(d->wm, d->pend_win); return; }
    pend_buf *pb = calloc(1, sizeof *pb);
    if (!pb) return;
    pb->diff_for = -1;
    int W = aios_fb_w(d->fb), H = aios_fb_h(d->fb);
    aios_rect r = { W / 2 - 260, 90, 520, H - 90 - BAR_H - 20 };
    /* A sixth window, and it is allowed to overlap the five launcher apps:
     * this is a modal decision, and burying the thing waiting on a person
     * behind the terminal is how a queue gets ignored. The launcher bar stays
     * visible underneath so the box never looks like it lost its chrome. */
    d->pend_win = aios_wm_open(d->wm, r, "pending changes", pb);
    pend_reload(d, pb);
    pend_diff_load(d, pb);
    pend_render(d);
}

static void pend_close(desktop *d) {
    if (!d->pend_win) return;
    pend_buf *pb = aios_win_user(d->pend_win);
    if (pb) {
        stage_pending_free(&pb->l);
        stage_diff_free(&pb->diff);
        free(pb);
    }
    aios_wm_close(d->wm, d->pend_win);
    d->pend_win = NULL;
    aios_wm_damage_all(d->wm);
}

/* Resolve + apply. The window decides, but it does not reimplement promote:
 * it asks the shell to run the same builtin a person would type, so a promote
 * from the window and a promote from the console go through one code path --
 * including the audit gate and the refusal that comes with it. */
static void pend_act(desktop *d, const char *verb) {
    pend_buf *pb = aios_win_user(d->pend_win);
    if (!pb || pb->l.n == 0) return;
    int i = d->pend_sel;
    if (i < 0 || (size_t)i >= pb->l.n) return;
    char cmd[300];
    snprintf(cmd, sizeof cmd, "%s %.60s", verb, pb->l.items[i].id);
    sh_result r;
    memset(&r, 0, sizeof r);
    sh_exec_origin(d->sh, cmd, ORIGIN_HUMAN, &r);
    if (r.error && *r.error)      toast(d, "%.150s", r.error);
    else if (r.text && *r.text)   toast(d, "%.150s", r.text);
    sh_result_free(&r);
    sembus_pub_kv(d->bus, "sh.ui.pending_action",
                  (const char *const[]){
                      "change", pb->l.items[i].id, "verb", verb, NULL});
    /* The list changed underneath the selection: re-read both. */
    pend_reload(d, pb);
    pend_diff_load(d, pb);
    if (pb->l.n == 0) pend_close(d);
    else pend_render(d);
}

static void pend_key(desktop *d, int key) {
    pend_buf *pb = aios_win_user(d->pend_win);
    if (!pb) return;
    if (key == AIOS_KEY_UP && d->pend_sel > 0) d->pend_sel--;
    else if (key == AIOS_KEY_DOWN && (size_t)(d->pend_sel + 1) < pb->l.n) d->pend_sel++;
    else if (key == 'p' || key == 'P') { pend_act(d, "promote"); return; }
    else if (key == 'd' || key == 'D') { pend_act(d, "discard"); return; }
    else if (key == 27 || key == '\n') {
        /* enter opens the change's diff on its own -- the console's `diff <id>`
         * -- which is already the pane below; enter therefore re-reads it. */
        pend_diff_load(d, pb);
        pend_render(d);
        return;
    } else return;
    pend_diff_load(d, pb);
    pend_render(d);
}

/* ---------------- launcher bar -------------------------------------- */

static void bar_render(desktop *d) {
    aios_fb *f = d->fb;
    int w = aios_fb_w(f), h = aios_fb_h(f);
    aios_rect bar = { 0, h - BAR_H, w, BAR_H };
    aios_fb_fill(f, &bar, COL_BAR_BG);
    aios_fb_hline(f, 0, h - BAR_H, w, COL_BAR_LINE);
    /* logo */
    extern aios_img *g_logo_small;   /* set by main; avoid another struct */
    if (g_logo_small)
        aios_fb_blit(f, 8, h - BAR_H + 8, g_logo_small);
    aios_fb_text(f, 8 + 64 + 8, h - BAR_H + 10, "AI-OS", COL_TEXT, 0, 1);
    aios_fb_text(f, 8 + 64 + 8, h - BAR_H + 28, AIOS_VERSION, COL_DIM, 0, 0);
    /* NL input */
    int ix = 150, iw = w - 150 - 460;
    aios_rect in = { ix, h - BAR_H + 12, iw, 26 };
    aios_fb_fill(f, &in, d->nl_focus ? COL_INPUT_BG : 0xFF1a2130u);
    aios_fb_rect(f, &in, d->nl_focus ? COL_ACCENT : AIOS_RGB(0x3a, 0x44, 0x58));
    char shown[200];
    snprintf(shown, sizeof shown, "%s", d->nl);
    aios_fb_text(f, ix + 8, h - BAR_H + 16, shown, COL_TEXT, 0, 0);
    if (d->nl_focus && d->caret) {
        aios_rect caret = { ix + 8 + aios_fb_text_w(shown), h - BAR_H + 16,
                            7, 15 };
        aios_fb_fill(f, &caret, COL_ACCENT);
    }
    aios_fb_text(f, ix, h - BAR_H + 44, "ask in natural language; enter runs it in aiosh",
                 COL_TERM_DIM, 0, 0);
    /* app icons */
    for (int i = 0; i < APP_COUNT; i++) {
        int bx = w - 440 + i * 86;
        aios_rect ib = { bx, h - BAR_H + 6, 80, 52 };
        aios_fb_fill(f, &ib, 0xFF1c2536u);
        aios_fb_rect(f, &ib, COL_DIM);
        extern aios_img *g_icons[APP_COUNT];
        if (g_icons[i])
            aios_fb_blit(f, bx + 8, h - BAR_H + 8, g_icons[i]);
        aios_fb_text(f, bx + 8, h - BAR_H + 36, APP_META[i].title,
                     i == APP_TERMINAL ? COL_ACCENT : COL_DIM, 0, 0);
    }
    /* status */
    size_t alive = sh_jobs_alive(d->sh);
    char st[64];
    snprintf(st, sizeof st, "cortex: %s  jobs: %zu", sh_cortex_mode(d->sh),
             alive);
    aios_fb_text(f, w - 440 - 300, h - BAR_H + 16, st, COL_DIM, 0, 0);

    /* M8: how many changes are waiting on a person, and the click target that
     * opens the window. Deliberately NOT a sixth launcher slot -- the bar shows
     * a COUNT, because "3 pending" is a thing you should not have to go looking
     * for, and the five app slots stay where RULES §2.4 put them. */
    stage_pending_list pl;
    char perr[128] = {0};
    int npend = 0;
    if (stage_pending_scan(desk_store(), &pl, perr, sizeof perr)) {
        npend = (int)pl.n;
        stage_pending_free(&pl);
    }
    if (npend > 0) {
        aios_rect pb = { bar_pend_x(w), h - BAR_H + 8, BAR_PEND_W, 22 };
        aios_fb_fill(f, &pb, 0xFF3a2a10u);
        aios_fb_rect(f, &pb, AIOS_RGB(0xe8, 0xb8, 0x6a));
        char pt[48];
        snprintf(pt, sizeof pt, "%d pending", npend);
        aios_fb_text(f, pb.x + 8, pb.y + 3, pt, AIOS_RGB(0xe8, 0xb8, 0x6a), 0, 1);
    }
}

static void bar_click(desktop *d, int x, int y) {
    int w = aios_fb_w(d->fb), h = aios_fb_h(d->fb);
    if (x >= 150 && x < w - 440 && y > h - BAR_H && y < h) {
        d->nl_focus = 1;
        aios_wm_damage_all(d->wm);
        return;
    }
    d->nl_focus = 0;
    int pbx = bar_pend_x(w);
    if (x >= pbx && x < pbx + BAR_PEND_W && y > h - BAR_H && y < h) {
        pend_open(d);
        return;
    }
    for (int i = 0; i < APP_COUNT; i++) {
        int bx = w - 440 + i * 86;
        if (x >= bx && x < bx + 86 && y > h - BAR_H && y < h) {
            aios_win *win = d->apps[i].win;
            if (win) aios_wm_focus(d->wm, win);
            aios_wm_damage_all(d->wm);
            return;
        }
    }
    aios_wm_damage_all(d->wm);
}

static void nl_key(desktop *d, int key) {
    if (key == '\n') {
        if (d->nl[0]) {
            char cmd[220];
            snprintf(cmd, sizeof cmd, "%s", d->nl);
            d->nl[0] = 0;
            /* transcript lands in the terminal window (opens if hidden) */
            aios_wm_focus(d->wm, d->apps[APP_TERMINAL].win);
            char shown[240];
            snprintf(shown, sizeof shown, "aiosh> %.150s", cmd);
            term_buf *t = aios_win_user(d->apps[APP_TERMINAL].win);
            term_push(t, shown);
            sh_result r;
            /* M7: d->nl_origin, not ORIGIN_HUMAN. When UI-07 lets the model
             * drive this box, the only line that has to change is the one that
             * sets the field -- the execution path already asks. */
            sh_exec_origin(d->sh, cmd, d->nl_origin, &r);
            if (r.error && *r.error) term_push(t, r.error);
            if (r.text && *r.text) term_push(t, r.text);
            else if ((!r.text || !*r.text) && (!r.error || !*r.error))
                term_push(t, "(ok)");
            sh_result_free(&r);
            term_render(d);
            toast(d, "aiosh handled your request");
        }
        return;
    }
    if (key == 0x08 || key == 0x7f) {
        size_t n = strlen(d->nl);
        if (n) d->nl[n - 1] = 0;
        aios_wm_damage_all(d->wm);
        return;
    }
    if (key >= 32 && key <= 126) {
        size_t n = strlen(d->nl);
        if (n < 190) {
            d->nl[n] = (char)key;
            d->nl[n + 1] = 0;
            aios_wm_damage_all(d->wm);
        }
    }
}

/* ---------------- toasts --------------------------------------------- */

static void toasts_render(desktop *d) {
    aios_fb *f = d->fb;
    int w = aios_fb_w(f);
    for (int i = 0; i < d->toast_n; i++) {
        if (d->now_ms > d->toast_until[i]) continue;
        int tw = aios_fb_text_w(d->toast[i]) + 16;
        aios_rect r = { w - tw - 10, 10 + i * 26, tw, 22 };
        aios_fb_fill(f, &r, 0xE016222eu);
        aios_fb_rect(f, &r, COL_ACCENT);
        aios_fb_text(f, r.x + 8, r.y + 3, d->toast[i], COL_TEXT, 0, 0);
    }
}

static void toasts_gc(desktop *d) {
    for (int i = 0; i < d->toast_n; i++) {
        if (d->now_ms > d->toast_until[i]) {
            for (int j = i; j < d->toast_n - 1; j++) {
                memcpy(d->toast[j], d->toast[j + 1], 96);
                d->toast_until[j] = d->toast_until[j + 1];
            }
            d->toast_n--;
            i--;
            aios_wm_damage_all(d->wm);
        }
    }
}

/* ---------------- sembus --------------------------------------------- */

static void bus_cb(const char *topic, const char *payload,
                   size_t payload_len, void *ud) {
    desktop *d = ud;
    char msg[96];
    if (payload_len > 70) payload_len = 70;
    snprintf(msg, sizeof msg, "%s %.70s", topic, payload ? payload : "");
    for (char *p = msg + 5; *p; p++)
        if (*p == '"' || *p == '{' || *p == '}') *p = ' ';
    toast(d, "%s", msg);
}

/* ---------------- open windows --------------------------------------- */

static void open_app(desktop *d, app_kind k) {
    int W = aios_fb_w(d->fb);
    aios_rect r;
    void *user = NULL;
    switch (k) {
        case APP_TERMINAL: {
            r.x = 40; r.y = 40; r.w = W - 560; r.h = 440;
            term_buf *t = calloc(1, sizeof *t);
            term_push(t, "AI-OS desktop - this terminal IS aiosh.");
            term_push(t, "try: 6*7 | list files | remember <fact>");
            user = t;
            break;
        }
        case APP_FILES: {
            r.x = W - 500; r.y = 40; r.w = 460; r.h = 440;
            files_buf *fb2 = calloc(1, sizeof *fb2);
            snprintf(fb2->path, sizeof fb2->path, "%s",
                     getenv("AIOS_WS") ? getenv("AIOS_WS") : "/workspace");
            files_reload(fb2);
            user = fb2;
            break;
        }
        case APP_AGENTS: {
            r.x = 40; r.y = 510; r.w = 380; r.h = 200;
            agents_buf *a = calloc(1, sizeof *a);
            user = a;
            break;
        }
        case APP_MEMORY: {
            r.x = 440; r.y = 510; r.w = 380; r.h = 200;
            mem_buf *m = calloc(1, sizeof *m);
            user = m;
            break;
        }
        default: {
            r.x = 840; r.y = 510; r.w = W - 880; r.h = 200;
            user = NULL;
            break;
        }
    }
    d->apps[k].win = aios_wm_open(d->wm, r, APP_META[k].title, user);
    d->apps[k].kind = k;
}

static void refresh_app(desktop *d, app_kind k) {
    switch (k) {
        case APP_TERMINAL: term_render(d); break;
        case APP_FILES:    files_render(d); break;
        case APP_AGENTS:   agents_reload(d, aios_win_user(d->apps[k].win));
                           agents_render(d); break;
        case APP_MEMORY:   mem_render(d); break;
        case APP_AUTOPILOT: ap_render(d); break;
        default: break;
    }
}

static void key_to_app(desktop *d, int key) {
    aios_win *w = d->apps[APP_TERMINAL].win;
    /* which window has focus? map it to its handler */
    if (w == aios_wm_top(d->wm)) { term_key(d, key); return; }
    if (d->apps[APP_FILES].win == aios_wm_top(d->wm)) { files_key(d, key); return; }
    if (d->apps[APP_MEMORY].win == aios_wm_top(d->wm)) { mem_key(d, key); return; }
    if (d->nl_focus) { nl_key(d, key); return; }
    if (d->apps[APP_AUTOPILOT].win == aios_wm_top(d->wm)) return;
    if (d->apps[APP_AGENTS].win == aios_wm_top(d->wm)) return;
    /* fallback: type into the terminal */
    term_key(d, key);
}

/* ---------------- main ------------------------------------------------ */

aios_img *g_logo_small;
aios_img *g_icons[APP_COUNT];

int main(int argc, char **argv) {
    int headless_fallback = (argc > 1 && !strcmp(argv[1], "--text"));

    desktop D;
    memset(&D, 0, sizeof D);
    desktop *d = &D;

    d->fb = aios_fb_open("/dev/fb0");
    if (!d->fb) {
        fprintf(stderr, "[desktop] no framebuffer (%s) - falling back\n",
                strerror(errno));
        return 3;                     /* init respawns the text console */
    }
    fprintf(stderr, "[desktop] framebuffer %dx%d\n", aios_fb_w(d->fb),
            aios_fb_h(d->fb));

    d->wm = aios_wm_create(d->fb);
    d->input = aios_input_open();
    aios_input_set_screen(d->input, aios_fb_w(d->fb), aios_fb_h(d->fb));
    if (!aios_input_ok(d->input))
        fprintf(stderr, "[desktop] no input devices (headless ok)\n");

    /* the OS store + workspace come from init (durable-aware) */
    const char *store = getenv("AIOS_STORE");
    if (!store || !*store) store = "/var/lib/aios/store";
    setenv("AIOS_STORE", store, 1);
    d->sh = sh_create("/workspace", store);
    if (!d->sh) {
        fprintf(stderr, "[desktop] shell engine failed\n");
        return 3;
    }
    /* M7: the launcher is a person typing. Written here, at the one place the
     * desktop is built, so it is a decision in the source rather than a zero in a
     * memset that happens to mean "human". */
    d->nl_origin = ORIGIN_HUMAN;
    /* M7: the desktop answers a hold by showing a window. The asker returns
     * DISCARD synchronously (the shell is on this thread and must not block);
     * hold_open() puts the question in front of the person on the next tick. */
    sh_set_ui_asker(d->sh, desktop_ask, d);
    /* M7 LLM-first: init exports AIOS_SHELL_CORTEX=micro - the desktop
     * launcher speaks through the OS's own language model too */
    const char *cortex = getenv("AIOS_SHELL_CORTEX");
    if (!cortex || !*cortex) cortex = "micro";
    sh_set_cortex(d->sh, cortex);
    d->bus = sh_bus(d->sh);
    sembus_sub(d->bus, "sh.agent.done", bus_cb, d);
    sembus_sub(d->bus, "sh.spawn", bus_cb, d);
    sembus_sub(d->bus, "boot.up", bus_cb, d);

    /* assets: artist images or procedural fallbacks */
    aios_img *wall = asset_load(d, "wallpaper-default", NULL,
                                aios_fb_w(d->fb), aios_fb_h(d->fb), 0);
    if (!wall)
        wall = aios_img_wallpaper_fallback(aios_fb_w(d->fb),
                                           aios_fb_h(d->fb));
    aios_img *logo = asset_load(d, "logo-boot", NULL, 96, 96, 1);
    if (!logo) logo = aios_img_logo_fallback(96);
    g_logo_small = asset_load(d, "logo-small", NULL, 48, 48, 1);
    if (!g_logo_small) g_logo_small = aios_img_logo_fallback(48);
    aios_img *cursor = asset_load(d, "cursor-default", NULL, 32, 32, 0);
    if (!cursor) cursor = aios_img_cursor_fallback();
    for (int i = 0; i < APP_COUNT; i++) {
        g_icons[i] = asset_load(d, APP_META[i].asset, NULL, 48, 48, 0);
        if (!g_icons[i])
            g_icons[i] = aios_img_icon_fallback(APP_META[i].asset, 48);
    }
    aios_wm_set_wallpaper(d->wm, wall);
    aios_wm_set_cursor(d->wm, cursor);

    /* windows */
    for (int i = 0; i < APP_COUNT; i++) open_app(d, (app_kind)i);
    aios_wm_focus(d->wm, d->apps[APP_TERMINAL].win);
    for (int i = APP_COUNT - 1; i >= 0; i--) refresh_app(d, (app_kind)i);
    aios_wm_focus(d->wm, d->apps[APP_TERMINAL].win);

    /* boot toast */
    D.now_ms = aios_now_ms();
    toast(d, "AI-OS %s desktop up - AI is a first-class citizen",
          AIOS_VERSION);

    aios_wm_cursor_move(d->wm, aios_fb_w(d->fb) / 2, aios_fb_h(d->fb) / 2);
    aios_wm_damage_all(d->wm);
    aios_wm_repaint(d->wm);
    bar_render(d);
    toasts_render(d);
    aios_fb_flush(d->fb);

    printf("AIOS-DESKTOP-OK w=%d h=%d\n", aios_fb_w(d->fb),
           aios_fb_h(d->fb));
    fflush(stdout);
    d->ok_printed = 1;

    if (headless_fallback) {           /* dev: draw once, exit */
        aios_img_free(wall);
        aios_img_free(logo);
        return 0;
    }

    /* ---- event loop ------------------------------------------------- */
    for (;;) {
        aios_event ev;
        int have = aios_input_poll(d->input, &ev, 60);
        d->now_ms = aios_now_ms();
        int caret_blink = ((long)(d->now_ms / 350)) & 1;

        if (have) {
            switch (ev.type) {
                case AIOS_EV_MOUSE_MOVE:
                    aios_wm_cursor_move(d->wm, ev.mx, ev.my);
                    break;
                case AIOS_EV_MOUSE_BTN:
                    if (ev.btn == 1 && ev.btn_down) {
                        int mx = ev.mx, my = ev.my;
                        if (my >= aios_fb_h(d->fb) - BAR_H) {
                            bar_click(d, mx, my);
                            break;
                        }
                        aios_win *hit = aios_wm_hit(d->wm, mx, my);
                        if (hit) {
                            aios_rect cb;
                            aios_win_close_box(hit, &cb);
                            if (mx >= cb.x && mx < cb.x + cb.w &&
                                my >= cb.y && my < cb.y + cb.h) {
                                /* close: terminal/files stay (front-ends);
                                 * utility windows may close */
                                if (hit == d->hold_win) {
                                    /* M7: closing the question is not answering
                                     * it -- so it counts as no. The window itself
                                     * says "closing this window = discard", and
                                     * that promise has to be kept in the engine,
                                     * not only on the bus.
                                     *
                                     * This used to close the window and publish
                                     * "discard" without telling the shell. Before
                                     * M8 the action had already been discarded by
                                     * then, so the event happened to be true. With
                                     * a held action it is not: the action stayed
                                     * held, no window could answer it, and the bus
                                     * claimed a decision nobody could make.
                                     *
                                     * sh_resume(DISCARD) is what makes the event
                                     * true. It applies nothing, which is the whole
                                     * content of the promise. */
                                    d->hold_win = NULL;
                                    sembus_pub(d->bus, "sh.ui.hold_answered",
                                               "{\"choice\":\"discard\","
                                               "\"why\":\"closed\"}");
                                    sh_result cr;
                                    memset(&cr, 0, sizeof cr);
                                    sh_resume(d->sh, UI_CHOICE_DISCARD, &cr);
                                    sh_result_free(&cr);
                                    toast(d, "closed: the proposal was dropped, "
                                             "nothing ran");
                                    aios_wm_close(d->wm, hit);
                                    break;
                                }
                                if (hit == d->pend_win) {
                                    /* Closing the LIST is not a decision about
                                     * any change in it. Nothing is promoted and
                                     * nothing is discarded; the staging area is
                                     * untouched and the changes are still there
                                     * next time the window opens. */
                                    pend_close(d);
                                    break;
                                }
                                for (int i = 0; i < APP_COUNT; i++)
                                    if (d->apps[i].win == hit)
                                        d->apps[i].win = NULL;
                                aios_wm_close(d->wm, hit);
                                break;
                            }
                            aios_wm_focus(d->wm, hit);   /* title click = raise */
                            d->nl_focus = 0;
                            if (hit == d->apps[APP_AGENTS].win)
                                refresh_app(d, APP_AGENTS);
                            if (hit == d->apps[APP_AUTOPILOT].win)
                                refresh_app(d, APP_AUTOPILOT);
                        }
                    }
                    break;
                case AIOS_EV_KEY:
                    if (ev.key_down) {
                        /* M7: while a held action is being asked about, nothing
                         * else gets the keyboard. A key that both answers the
                         * question and types into a terminal would be a
                         * confirmation dialog you can type through. */
                        /* M8: the pending list owns the keyboard while it is
                         * open. It is a decision surface: a `p` that also
                         * reached the terminal would promote a change the
                         * person did not select. The hold window still wins
                         * when both are up -- answering a question is more
                         * specific than browsing a list. */
                        if (d->hold_win) hold_key(d, ev.key);
                        else if (d->pend_win) {
                            if (ev.key == 27) {    /* esc closes, decides nothing */
                                pend_close(d);
                            } else pend_key(d, ev.key);
                        }
                        else if (d->nl_focus) nl_key(d, ev.key);
                        else key_to_app(d, ev.key);
                    }
                    break;
                case AIOS_EV_MOUSE_WHEEL:
                default:
                    break;
            }
        } else {
            /* periodic refresh: autopilot + agents tick on idle */
            static int frames;
            frames++;
            if (d->hold_pending) hold_open(d);
            if (d->pend_open_req) { d->pend_open_req = 0; pend_open(d); }
            if (frames % 25 == 0) {
                if (d->apps[APP_AUTOPILOT].win) refresh_app(d, APP_AUTOPILOT);
                if (d->apps[APP_AGENTS].win) refresh_app(d, APP_AGENTS);
            }
            if (frames % 2 == 0 && (d->caret != caret_blink)) {
                d->caret = caret_blink;
                if (d->nl_focus) aios_wm_damage_all(d->wm);
                else if (aios_wm_top(d->wm) == d->apps[APP_TERMINAL].win
                         || aios_wm_top(d->wm) == d->apps[APP_MEMORY].win)
                    refresh_app(d, aios_wm_top(d->wm) == d->apps[APP_TERMINAL].win
                                       ? APP_TERMINAL : APP_MEMORY);
            }
        }

        toasts_gc(d);
        aios_wm_repaint(d->wm);
        bar_render(d);
        toasts_render(d);
        aios_fb_flush(d->fb);
    }
    return 0;
}
