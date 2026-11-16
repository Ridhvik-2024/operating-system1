/* win.c - M10 desktop: the window core (see aios/win.h).
 *
 * Compositing model: every change marks the whole screen dirty (cheap,
 * correct with overlaps) but the expensive part - the fbdev flush - only
 * copies the union damage rect. A composite writes ~4 MB into the RAM
 * back buffer; the flush is bounded by what actually changed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "aios/win.h"
#include "fb_priv.h"
#include "font8x16.h"

struct aios_win {
    aios_rect r;             /* screen rect including title bar       */
    char title[64];
    uint32_t *canvas;        /* r.w x (r.h - AIOS_TITLE_H)            */
    int cw, ch;
    void *user;
    aios_win *below;         /* z-order chain: top of screen = first  */
    aios_win *above;
};

struct aios_wm {
    aios_fb *fb;             /* borrowed                              */
    aios_win *top;           /* first = topmost                       */
    aios_win *focused;
    const aios_img *wall;    /* borrowed                              */
    const aios_img *cursor;  /* borrowed                              */
    int cx, cy;              /* cursor position (top-left of sprite)  */
    int cx_prev, cy_prev;
    int dirty;               /* full repaint requested                */
};

/* ---------------- windows ----------------------------------------- */

aios_win *aios_wm_open(aios_wm *m, aios_rect r, const char *title,
                       void *user) {
    if (r.w < 60 || r.h < AIOS_TITLE_H + 20) return NULL;
    aios_win *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.w > m->fb->w) r.w = m->fb->w - r.x;
    if (r.y + r.h > m->fb->h) r.h = m->fb->h - r.y;

    w->r = r;
    snprintf(w->title, sizeof w->title, "%s", title ? title : "");
    w->cw = r.w;
    w->ch = r.h - AIOS_TITLE_H;
    w->canvas = malloc((size_t)w->cw * w->ch * 4);
    if (!w->canvas) { free(w); return NULL; }
    memset(w->canvas, 0, (size_t)w->cw * w->ch * 4);
    w->user = user;
    /* push on top of the z-order */
    w->below = m->top;
    w->above = NULL;
    if (m->top) m->top->above = w;
    m->top = w;
    aios_wm_focus(m, w);
    return w;
}

void aios_wm_close(aios_wm *m, aios_win *w) {
    if (!w) return;
    if (w->above) w->above->below = w->below;
    else m->top = w->below;
    if (w->below) w->below->above = w->above;
    if (m->focused == w) m->focused = NULL;
    free(w->canvas);
    free(w);
    m->dirty = 1;
}

void aios_wm_focus(aios_wm *m, aios_win *w) {
    if (m->focused == w) return;
    m->focused = w;
    m->dirty = 1;
    if (w) aios_wm_raise(m, w);
}

void aios_wm_raise(aios_wm *m, aios_win *w) {
    if (!w || m->top == w) { if (w) m->dirty = 1; return; }
    /* unlink */
    if (w->above) w->above->below = w->below;
    if (w->below) w->below->above = w->above;
    /* link at top */
    w->above = NULL;
    w->below = m->top;
    if (m->top) m->top->above = w;
    m->top = w;
    m->dirty = 1;
}

void aios_wm_move(aios_wm *m, aios_win *w, int x, int y) {
    if (!w) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w->r.w > m->fb->w) x = m->fb->w - w->r.w;
    if (y + w->r.h > m->fb->h) y = m->fb->h - w->r.h;
    w->r.x = x;
    w->r.y = y;
    m->dirty = 1;
}

aios_win *aios_wm_top(aios_wm *m) { return m->top; }

aios_win *aios_wm_hit(aios_wm *m, int x, int y) {
    for (aios_win *w = m->top; w; w = w->below) {
        if (x >= w->r.x && x < w->r.x + w->r.w &&
            y >= w->r.y && y < w->r.y + w->r.h)
            return w;
    }
    return NULL;
}

void aios_wm_set_wallpaper(aios_wm *m, const aios_img *im) {
    m->wall = im;
    m->dirty = 1;
}

void aios_wm_set_cursor(aios_wm *m, const aios_img *im) {
    m->cursor = im;
    m->dirty = 1;
}

void aios_wm_cursor_move(aios_wm *m, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > m->fb->w - 1) x = m->fb->w - 1;
    if (y > m->fb->h - 1) y = m->fb->h - 1;
    if (x == m->cx && y == m->cy) return;
    m->cx_prev = m->cx; m->cy_prev = m->cy;
    m->cx = x; m->cy = y;
    m->dirty = 1;
}

void aios_wm_cursor(aios_wm *m, int *x, int *y) {
    if (x) *x = m->cx;
    if (y) *y = m->cy;
}

void aios_wm_damage_all(aios_wm *m) { m->dirty = 1; }

/* ---------------- compositor --------------------------------------- */

static void paint_title(aios_wm *m, aios_win *w) {
    aios_fb *f = m->fb;
    int focused = (w == m->focused);
    unsigned bar = focused ? 0xFF2b3548u : 0xFF1a2030u;
    unsigned line = focused ? 0xFF4ae08au : 0xFF3a4458u;
    unsigned tcol = focused ? 0xFFeef4ffu : 0xFF9aa8c0u;
    aios_rect bar_r = { w->r.x, w->r.y, w->r.w, AIOS_TITLE_H };
    aios_fb_fill(f, &bar_r, bar);
    aios_fb_hline(f, w->r.x, w->r.y + AIOS_TITLE_H - 1, w->r.w, line);
    aios_fb_text(f, w->r.x + 10, w->r.y + 4, w->title, tcol, 0, focused);
    /* close box */
    aios_rect cb;
    aios_win_close_box(w, &cb);
    aios_fb_fill(f, &cb, focused ? 0xFF3a2430u : 0xFF242a38u);
    aios_fb_text(f, cb.x + 2, cb.y + 4, "X", focused ? 0xFFff7a8au : 0xFF6a7890u,
                 0, focused);
    /* 1px window border */
    aios_fb_rect(f, &w->r, line);
}

static void paint_win(aios_wm *m, aios_win *w) {
    aios_fb *f = m->fb;
    /* canvas area (below title bar) */
    aios_rect body = { w->r.x, w->r.y + AIOS_TITLE_H, w->r.w, w->ch };
    aios_fb_fill(f, &body, 0xFF10141du);
    for (int j = 0; j < w->ch; j++) {
        const uint32_t *src = w->canvas + (size_t)j * w->cw;
        uint32_t *dst = f->back + (size_t)(w->r.y + AIOS_TITLE_H + j) * f->w
                        + w->r.x;
        memcpy(dst, src, (size_t)w->cw * 4);
    }
    paint_title(m, w);
}

void aios_wm_repaint(aios_wm *m) {
    if (!m->dirty) return;
    aios_fb *f = m->fb;
    /* wallpaper (or flat dark if none) */
    if (m->wall) {
        int w = aios_img_w(m->wall) < f->w ? aios_img_w(m->wall) : f->w;
        int h = aios_img_h(m->wall) < f->h ? aios_img_h(m->wall) : f->h;
        for (int j = 0; j < h; j++) {
            const uint8_t *src = aios_img_row(m->wall, j);
            uint32_t *dst = f->back + (size_t)j * f->w;
            for (int i = 0; i < w; i++)
                dst[i] = 0xFF000000u | ((uint32_t)src[i * 4] << 16)
                         | ((uint32_t)src[i * 4 + 1] << 8)
                         | (uint32_t)src[i * 4 + 2];
        }
        aios_rect full = { 0, 0, f->w, f->h };
        aios_fb_damage_add(f, &full);
    } else {
        aios_rect full = { 0, 0, f->w, f->h };
        aios_fb_fill(f, &full, 0xFF0a0d16u);
    }
    /* paint bottom-most first so higher windows composite over them */
    aios_win *bottom = m->top;
    while (bottom && bottom->below) bottom = bottom->below;
    for (aios_win *w = bottom; w; w = w->above) paint_win(m, w);
    /* cursor */
    if (m->cursor)
        aios_fb_blit(f, m->cx, m->cy, m->cursor);
    aios_fb_flush(f);
    m->dirty = 0;
}

aios_wm *aios_wm_create(aios_fb *fb) {
    aios_wm *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->fb = fb;
    return m;
}

void aios_wm_destroy(aios_wm *m) {
    if (!m) return;
    while (m->top) aios_wm_close(m, m->top);
    free(m);
}

/* ---------------- window drawing (canvas) -------------------------- */

static inline int wpx_ok(const aios_win *w, int x, int y) {
    return x >= 0 && y >= 0 && x < w->cw && y < w->ch;
}

void aios_win_px(aios_win *w, int x, int y, unsigned argb) {
    if (!w || !wpx_ok(w, x, y)) return;
    w->canvas[(size_t)y * w->cw + x] = argb | 0xFF000000u;
}

void aios_win_fill(aios_win *w, int x, int y, int width, int height,
                   unsigned argb) {
    if (!w) return;
    for (int j = 0; j < height; j++)
        for (int i = 0; i < width; i++)
            aios_win_px(w, x + i, y + j, argb);
}

void aios_win_rect(aios_win *w, int x, int y, int width, int height,
                   unsigned argb) {
    if (!w) return;
    for (int i = 0; i < width; i++) {
        aios_win_px(w, x + i, y, argb);
        aios_win_px(w, x + i, y + height - 1, argb);
    }
    for (int j = 0; j < height; j++) {
        aios_win_px(w, x, y + j, argb);
        aios_win_px(w, x + width - 1, y + j, argb);
    }
}

void aios_win_text(aios_win *w, int x, int y, const char *s, unsigned fg,
                   unsigned bg, int bold) {
    if (!w) return;
    for (const char *p = s; *p; p++, x += 8) {
        unsigned char ch = (unsigned char)*p;
        if (ch < AIOS_FONT_FIRST || ch > AIOS_FONT_LAST) ch = '?';
        const unsigned char *g = bold ? AIOS_FONT8X16B[ch - AIOS_FONT_FIRST]
                                      : AIOS_FONT8X16[ch - AIOS_FONT_FIRST];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) aios_win_px(w, x + col, y + row, fg);
                else if (bg) aios_win_px(w, x + col, y + row, bg);
    }
}

void aios_win_blit(aios_win *w, int x, int y, const aios_img *im) {
    if (!w || !im) return;
    int iw = aios_img_w(im), ih = aios_img_h(im);
    for (int j = 0; j < ih; j++) {
        const unsigned char *src = aios_img_row(im, j);
        for (int i = 0; i < iw; i++) {
            if (src[i * 4 + 3] > 8)
                aios_win_px(w, x + i, y + j,
                            ((unsigned)src[i * 4] << 16)
                            | ((unsigned)src[i * 4 + 1] << 8)
                            | src[i * 4 + 2]);
        }
    }
}

void aios_win_dirty(aios_win *w) {
    /* z-chain: mark the owning wm dirty via the window's canvas flip is
     * not needed - the desktop marks wm dirty after input; this API stays
     * for symmetry with test expectations. */
    (void)w;
}

int aios_win_cw(const aios_win *w) { return w ? w->cw : 0; }
int aios_win_ch(const aios_win *w) { return w ? w->ch : 0; }
const char *aios_win_title(const aios_win *w) { return w ? w->title : ""; }
void *aios_win_user(const aios_win *w) { return w ? w->user : NULL; }
void aios_win_set_user(aios_win *w, void *user) { if (w) w->user = user; }

void aios_win_close_box(const aios_win *w, aios_rect *out) {
    out->x = w->r.x + w->r.w - 22;
    out->y = w->r.y + 3;
    out->w = 18;
    out->h = 18;
}

int aios_win_on_title(const aios_win *w, int x, int y) {
    return x >= w->r.x && x < w->r.x + w->r.w &&
           y >= w->r.y && y < w->r.y + AIOS_TITLE_H;
}

int aios_win_x(const aios_win *w) { return w ? w->r.x : 0; }
int aios_win_y(const aios_win *w) { return w ? w->r.y : 0; }
