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
#include "aios/shell.h"
#include "aios/sembus.h"
#include "aios/json.h"
#include "aios/mem.h"
#include "aios/types.h"

/* ---------------- palette ------------------------------------------ */

#define COL_BAR_BG     0xFF161c2au
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

static void term_exec(desktop *d, const char *cmdline) {
    aios_win *w = d->apps[APP_TERMINAL].win;
    term_buf *t = aios_win_user(w);
    char shown[LINELEN];
    snprintf(shown, sizeof shown, "aiosh> %.150s", cmdline);
    term_push(t, shown);
    sh_result r;
    int exit_req = sh_exec(d->sh, cmdline, &r);
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
    aios_fb_text(f, w - 440 - 170, h - BAR_H + 16, st, COL_DIM, 0, 0);
}

static void bar_click(desktop *d, int x, int y) {
    int w = aios_fb_w(d->fb), h = aios_fb_h(d->fb);
    if (x >= 150 && x < w - 440 && y > h - BAR_H && y < h) {
        d->nl_focus = 1;
        aios_wm_damage_all(d->wm);
        return;
    }
    d->nl_focus = 0;
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
            sh_exec(d->sh, cmd, &r);
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
                        if (d->nl_focus) nl_key(d, ev.key);
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
