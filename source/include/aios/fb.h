/* aios/fb.h — M10 desktop: aios-fb, the framebuffer core.
 *
 * A thin, zero-dependency drawing layer over /dev/fb0 (virtio-gpu fbdev,
 * XRGB8888). Everything renders into an in-RAM double buffer; damage
 * rectangles are merged and flushed row-wise to the mmap'd framebuffer.
 *
 * Build-time pipeline (tools/img2aiosimg.py + tools/gen_font.py) keeps
 * the C side free of any image codec: images are pre-converted raw RGBA
 * (.aiosimg), text comes from a generated 8x16 bitmap table.
 *
 * Every artist asset (desktop/assets/ASSETS.md) has a procedural
 * fallback compiled in here — art never blocks the build or boot.
 */
#ifndef AIOS_FB_H
#define AIOS_FB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aios_fb  aios_fb;
typedef struct aios_img aios_img;

/* rectangle in screen space */
typedef struct { int x, y, w, h; } aios_rect;

/* colours are 0xAARRGGBB; alpha 255 = opaque */
#define AIOS_RGB(r, g, b) (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))

/* ---- framebuffer ---------------------------------------------------- */

/* Open a fbdev device (e.g. "/dev/fb0"). Requires 32bpp; returns NULL
 * (and leaves errno) when absent or unusable — callers fall back to the
 * M1-M9 text console. */
aios_fb *aios_fb_open(const char *dev);
void     aios_fb_close(aios_fb *f);

int aios_fb_w(const aios_fb *f);
int aios_fb_h(const aios_fb *f);

/* drawing into the back buffer (all clip against the screen) */
void aios_fb_fill(aios_fb *f, const aios_rect *r, unsigned argb);
void aios_fb_rect(aios_fb *f, const aios_rect *r, unsigned argb);
void aios_fb_hline(aios_fb *f, int x, int y, int w, unsigned argb);
void aios_fb_vline(aios_fb *f, int x, int y, int h, unsigned argb);
/* alpha-blended image blit (top-left at x,y) */
void aios_fb_blit(aios_fb *f, int x, int y, const aios_img *im);

/* text: 8x16 glyphs; fg blended, bg = 0 -> transparent over background */
void aios_fb_text(aios_fb *f, int x, int y, const char *s,
                  unsigned fg, unsigned bg, int bold);
int  aios_fb_text_w(const char *s);   /* pixels (8 * chars) */

/* damage tracking + flush: rows of the union rect are memcpy'd from the
 * back buffer to the fbdev mmap */
void aios_fb_damage_reset(aios_fb *f);
void aios_fb_damage_add(aios_fb *f, const aios_rect *r);
const aios_rect *aios_fb_damage(const aios_fb *f);
void aios_fb_flush(aios_fb *f);

/* host test seam: in-memory fb, identical drawing/flush paths */
aios_fb *aios_fb_open_mem(int w, int h);
/* read one composited pixel (test seam; returns 0 out of range) */
unsigned aios_fb_px(const aios_fb *f, int x, int y);

/* ---- images (.aiosimg: "AIOSIMG1" + w,h LE + raw RGBA) --------------- */

aios_img *aios_img_load(const char *path);   /* NULL if missing/broken */
void      aios_img_free(aios_img *im);
/* borrowed internals for compositors (raw RGBA rows, top-to-bottom) */
int aios_img_w(const aios_img *im);
int aios_img_h(const aios_img *im);
const unsigned char *aios_img_row(const aios_img *im, int y);

/* procedural fallbacks (compiled in; used when no artist PNG was built) */
aios_img *aios_img_wallpaper_fallback(int w, int h);
aios_img *aios_img_logo_fallback(int size);
aios_img *aios_img_icon_fallback(const char *name, int size);
aios_img *aios_img_cursor_fallback(void);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_FB_H */
