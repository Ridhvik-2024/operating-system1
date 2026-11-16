/* aios/win.h — M10 desktop: the window core.
 *
 * Small, honest window manager: z-ordered windows with title bars and a
 * focus ring, an image cursor, full-screen damage-driven compositing over
 * a wallpaper. Apps draw into their own canvas (below the title bar);
 * the compositor paints chrome + canvas + cursor and flushes only the
 * damaged rows through aios-fb.
 */
#ifndef AIOS_WIN_H
#define AIOS_WIN_H

#include "aios/fb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_TITLE_H 24
#define AIOS_CURSOR_W 32
#define AIOS_CURSOR_H 32

typedef struct aios_win aios_win;
typedef struct aios_wm  aios_wm;

aios_wm *aios_wm_create(aios_fb *fb);
void     aios_wm_destroy(aios_wm *m);

/* rect is the full window INCLUDING the title bar, in screen coords */
aios_win *aios_wm_open(aios_wm *m, aios_rect r, const char *title,
                       void *user);
void      aios_wm_close(aios_wm *m, aios_win *w);
void      aios_wm_focus(aios_wm *m, aios_win *w);
void      aios_wm_move(aios_wm *m, aios_win *w, int x, int y);
void      aios_wm_raise(aios_wm *m, aios_win *w);
aios_win *aios_wm_top(aios_wm *m);
aios_win *aios_wm_hit(aios_wm *m, int x, int y);   /* topmost at point */

/* wallpaper + cursor (borrowed images; owned by the caller) */
void aios_wm_set_wallpaper(aios_wm *m, const aios_img *im);
void aios_wm_set_cursor(aios_wm *m, const aios_img *im);
void aios_wm_cursor_move(aios_wm *m, int x, int y);
void aios_wm_cursor(aios_wm *m, int *x, int *y);

/* full repaint of everything on the next repaint() call */
void aios_wm_damage_all(aios_wm *m);

/* composite + flush. Cheap no-op when nothing changed. */
void aios_wm_repaint(aios_wm *m);

/* ---- per-window drawing (clipped to the window's canvas) ------------ */

void aios_win_fill(aios_win *w, int x, int y, int width, int height,
                   unsigned argb);
void aios_win_rect(aios_win *w, int x, int y, int width, int height,
                   unsigned argb);
void aios_win_px(aios_win *w, int x, int y, unsigned argb);
void aios_win_text(aios_win *w, int x, int y, const char *s, unsigned fg,
                   unsigned bg, int bold);
void aios_win_blit(aios_win *w, int x, int y, const aios_img *im);
void aios_win_dirty(aios_win *w);          /* force repaint of this win */

int        aios_win_cw(const aios_win *w);   /* canvas width           */
int        aios_win_ch(const aios_win *w);   /* canvas height          */
const char *aios_win_title(const aios_win *w);
void      *aios_win_user(const aios_win *w);
void       aios_win_set_user(aios_win *w, void *user);
/* close "X" box inside the title bar (for click hit-testing) */
void       aios_win_close_box(const aios_win *w, aios_rect *out);
/* true when the point is on this window's title bar */
int        aios_win_on_title(const aios_win *w, int x, int y);
/* drag offset: window title origin minus grab point (title-drag support) */
int        aios_win_x(const aios_win *w);
int        aios_win_y(const aios_win *w);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_WIN_H */
