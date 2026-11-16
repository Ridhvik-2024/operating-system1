/* fb.c - M10 desktop: aios-fb, the framebuffer core (see aios/fb.h).
 *
 * Kernel side: /dev/fb0 exposed by virtio-gpu's fbdev emulation
 * (virtio_gpudrmfb, XRGB8888 - module stack gunzipped + insmodded by
 * aios-init before this runs). We render into a RAM double buffer and
 * copy only damaged rows to the mmap: one write() less per frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "aios/fb.h"

/* generated at build time by tools/gen_font.py (DejaVu Sans Mono) */
#include "font8x16.h"

#include "fb_priv.h"

struct aios_img {
    int w, h;
    uint8_t *rgba;         /* w*h*4, top-to-bottom           */
};

/* ------------------------------------------------------------------ */
/* clipping                                                            */

static void clip(const aios_fb *f, int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > f->w) *w = f->w - *x;
    if (*y + *h > f->h) *h = f->h - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static void dmg_add(aios_fb *f, int x, int y, int w, int h) {
    clip(f, &x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    if (!f->dirty) {
        f->dmg.x = x; f->dmg.y = y; f->dmg.w = w; f->dmg.h = h;
        f->dirty = 1;
        return;
    }
    int x2 = f->dmg.x + f->dmg.w, y2 = f->dmg.y + f->dmg.h;
    int nx2 = x + w, ny2 = y + h;
    if (x < f->dmg.x) f->dmg.x = x;
    if (y < f->dmg.y) f->dmg.y = y;
    if (nx2 > x2) x2 = nx2;
    if (ny2 > y2) y2 = ny2;
    f->dmg.w = x2 - f->dmg.x;
    f->dmg.h = y2 - f->dmg.y;
}

/* ------------------------------------------------------------------ */
/* open/close                                                          */

aios_fb *aios_fb_open(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) return NULL;
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fi) != 0) {
        close(fd); return NULL;
    }
    if (vi.bits_per_pixel != 32 ||
        vi.red.offset != 16 || vi.green.offset != 8 || vi.blue.offset != 0) {
        close(fd); return NULL;   /* only XRGB8888 (virtio default) */
    }
    size_t len = (size_t)fi.line_length * vi.yres_virtual;
    uint32_t *hw = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hw == MAP_FAILED) { close(fd); return NULL; }

    aios_fb *f = calloc(1, sizeof *f);
    if (!f) { munmap(hw, len); close(fd); return NULL; }
    f->w = vi.xres; f->h = vi.yres;
    f->fd = fd;
    f->hw = hw;
    f->back = malloc((size_t)f->w * f->h * 4);
    if (!f->back) {
        munmap(hw, len); free(f); close(fd);
        return NULL;
    }
    memset(f->back, 0, (size_t)f->w * f->h * 4);
    return f;
}

aios_fb *aios_fb_open_mem(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    aios_fb *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->w = w; f->h = h;
    f->fd = -1;
    f->hw = NULL;
    f->back = malloc((size_t)w * h * 4);
    if (!f->back) { free(f); return NULL; }
    memset(f->back, 0, (size_t)w * h * 4);
    return f;
}

void aios_fb_close(aios_fb *f) {
    if (!f) return;
    if (f->hw) {
        struct fb_fix_screeninfo fi;
        size_t len = (size_t)f->w * f->h * 4;
        if (ioctl(f->fd, FBIOGET_FSCREENINFO, &fi) == 0 && fi.smem_len > 0)
            len = fi.smem_len;
        munmap(f->hw, len);
    }
    if (f->fd >= 0) close(f->fd);
    free(f->back);
    free(f);
}

int aios_fb_w(const aios_fb *f) { return f->w; }
int aios_fb_h(const aios_fb *f) { return f->h; }

/* ------------------------------------------------------------------ */
/* primitives (back buffer)                                            */

static uint32_t blend(uint32_t dst, uint32_t src) {
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0xFF) return src | 0xFF000000u;
    if (a == 0) return dst;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t r = (sr * a + dr * (255 - a)) / 255;
    uint32_t g = (sg * a + dg * (255 - a)) / 255;
    uint32_t b = (sb * a + db * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void px(aios_fb *f, int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return;
    f->back[(size_t)y * f->w + x] = blend(f->back[(size_t)y * f->w + x], c);
}

unsigned aios_fb_px(const aios_fb *f, int x, int y) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return 0;
    return f->back[(size_t)y * f->w + x];
}

void aios_fb_fill(aios_fb *f, const aios_rect *r, unsigned argb) {
    int x = r->x, y = r->y, w = r->w, h = r->h;
    clip(f, &x, &y, &w, &h);
    for (int j = 0; j < h; j++) {
        uint32_t *row = f->back + (size_t)(y + j) * f->w + x;
        for (int i = 0; i < w; i++) row[i] = blend(row[i], argb);
    }
    dmg_add(f, x, y, w, h);
}

void aios_fb_hline(aios_fb *f, int x, int y, int w, unsigned argb) {
    aios_rect r = { x, y, w, 1 };
    aios_fb_fill(f, &r, argb);
}

void aios_fb_vline(aios_fb *f, int x, int y, int h, unsigned argb) {
    aios_rect r = { x, y, 1, h };
    aios_fb_fill(f, &r, argb);
}

void aios_fb_rect(aios_fb *f, const aios_rect *r, unsigned argb) {
    aios_fb_hline(f, r->x, r->y, r->w, argb);
    aios_fb_hline(f, r->x, r->y + r->h - 1, r->w, argb);
    aios_fb_vline(f, r->x, r->y, r->h, argb);
    aios_fb_vline(f, r->x + r->w - 1, r->y, r->h, argb);
}

void aios_fb_blit(aios_fb *f, int x, int y, const aios_img *im) {
    if (!im) return;
    for (int j = 0; j < im->h; j++) {
        const uint8_t *src = im->rgba + (size_t)j * im->w * 4;
        for (int i = 0; i < im->w; i++) {
            uint32_t c = 0;
            c |= (uint32_t)src[i * 4 + 3] << 24;
            c |= (uint32_t)src[i * 4 + 0] << 16;
            c |= (uint32_t)src[i * 4 + 1] << 8;
            c |= (uint32_t)src[i * 4 + 2];
            if ((c & 0xFF000000u))
                px(f, x + i, y + j, c);
        }
    }
    dmg_add(f, x, y, im->w, im->h);
}

/* ------------------------------------------------------------------ */
/* text                                                                */

static void glyph(aios_fb *f, int x, int y, unsigned char ch,
                  uint32_t fg, uint32_t bg) {
    if (ch < AIOS_FONT_FIRST || ch > AIOS_FONT_LAST) ch = '?';
    const unsigned char *g = AIOS_FONT8X16[ch - AIOS_FONT_FIRST];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = g[row];
        for (int col = 0; col < 8; col++) {
            int on = bits & (0x80 >> col);
            if (on) px(f, x + col, y + row, fg);
            else if (bg) px(f, x + col, y + row, bg);
        }
    }
}

static void glyph_b(aios_fb *f, int x, int y, unsigned char ch,
                    uint32_t fg, uint32_t bg) {
    if (ch < AIOS_FONT_FIRST || ch > AIOS_FONT_LAST) ch = '?';
    const unsigned char *g = AIOS_FONT8X16B[ch - AIOS_FONT_FIRST];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = g[row];
        for (int col = 0; col < 8; col++) {
            int on = bits & (0x80 >> col);
            if (on) px(f, x + col, y + row, fg);
            else if (bg) px(f, x + col, y + row, bg);
        }
    }
}

void aios_fb_text(aios_fb *f, int x, int y, const char *s,
                  unsigned fg, unsigned bg, int bold) {
    for (const char *p = s; *p; p++, x += 8) {
        if (x > f->w) break;
        if (bold) glyph_b(f, x, y, (unsigned char)*p, fg, bg);
        else glyph(f, x, y, (unsigned char)*p, fg, bg);
    }
    int n = (int)strlen(s);
    dmg_add(f, x - 8 * n, y, 8 * n, 16);
}

int aios_fb_text_w(const char *s) { return 8 * (int)strlen(s); }

/* ------------------------------------------------------------------ */
/* damage + flush                                                      */

void aios_fb_damage_reset(aios_fb *f) { f->dirty = 0; }
void aios_fb_damage_add(aios_fb *f, const aios_rect *r) {
    dmg_add(f, r->x, r->y, r->w, r->h);
}
const aios_rect *aios_fb_damage(const aios_fb *f) {
    return f->dirty ? &f->dmg : NULL;
}

void aios_fb_flush(aios_fb *f) {
    const aios_rect *d = aios_fb_damage(f);
    if (!d || !f->hw) { f->dirty = 0; return; }
    int x = d->x, y = d->y, w = d->w, h = d->h;
    clip(f, &x, &y, &w, &h);
    for (int j = 0; j < h; j++) {
        memcpy(f->hw + (size_t)(y + j) * f->w + x,
               f->back + (size_t)(y + j) * f->w + x,
               (size_t)w * 4);
    }
    f->dirty = 0;
}

/* ------------------------------------------------------------------ */
/* .aiosimg loading                                                    */

aios_img *aios_img_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    char magic[8];
    uint8_t hdr[8];
    if (fread(magic, 1, 8, fp) != 8 || memcmp(magic, "AIOSIMG1", 8) != 0 ||
        fread(hdr, 1, 8, fp) != 8) {
        fclose(fp);
        return NULL;
    }
    int w = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((int)hdr[3] << 24);
    int h = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((int)hdr[7] << 24);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { fclose(fp); return NULL; }
    size_t n = (size_t)w * h * 4;
    aios_img *im = malloc(sizeof *im + n);
    if (!im) { fclose(fp); return NULL; }
    im->w = w; im->h = h;
    im->rgba = (uint8_t *)(im + 1);
    if (fread(im->rgba, 1, n, fp) != n) { free(im); fclose(fp); return NULL; }
    fclose(fp);
    return im;
}

void aios_img_free(aios_img *im) { free(im); }

int aios_img_w(const aios_img *im) { return im ? im->w : 0; }
int aios_img_h(const aios_img *im) { return im ? im->h : 0; }
const unsigned char *aios_img_row(const aios_img *im, int y) {
    if (!im || y < 0 || y >= im->h) return NULL;
    return im->rgba + (size_t)y * im->w * 4;
}

/* ------------------------------------------------------------------ */
/* procedural fallbacks - the OS never ships without a face            */
/* (ASSETS.md: every artist asset has a compiled-in fallback)          */

/* tiny img-space rasterizer used only by the fallback painters */
typedef struct { int w, h; uint8_t *px; } canvas;

static void cpx(canvas *c, int x, int y, unsigned argb) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    uint8_t *p = c->px + ((size_t)y * c->w + x) * 4;
    uint32_t a = (argb >> 24) & 0xFF;
    uint32_t sa = p[3] ? ((uint32_t)p[3] * (255 - a)) / 255 : 0;
    p[0] = (uint8_t)(((argb >> 16) & 0xFF) * a / 255 + ((uint32_t)p[0] * sa) / 255);
    p[1] = (uint8_t)(((argb >> 8) & 0xFF) * a / 255 + ((uint32_t)p[1] * sa) / 255);
    p[2] = (uint8_t)((argb & 0xFF) * a / 255 + ((uint32_t)p[2] * sa) / 255);
    p[3] = (uint8_t)(a + sa);
}

static void cfill(canvas *c, int x, int y, int w, int h, unsigned argb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) cpx(c, x + i, y + j, argb);
}

static void crect(canvas *c, int x, int y, int w, int h, unsigned argb) {
    for (int i = 0; i < w; i++) { cpx(c, x + i, y, argb); cpx(c, x + i, y + h - 1, argb); }
    for (int j = 0; j < h; j++) { cpx(c, x, y + j, argb); cpx(c, x + w - 1, y + j, argb); }
}

static void ctext(canvas *c, int x, int y, const char *s, unsigned fg,
                  unsigned bg, int bold) {
    for (const char *p = s; *p; p++, x += 8) {
        if (x >= c->w) break;
        unsigned char ch = (unsigned char)*p;
        if (ch < AIOS_FONT_FIRST || ch > AIOS_FONT_LAST) ch = '?';
        const unsigned char *tbl = bold ? AIOS_FONT8X16B[ch - AIOS_FONT_FIRST]
                                        : AIOS_FONT8X16[ch - AIOS_FONT_FIRST];
        const unsigned char *g = tbl;
        for (int row = 0; row < 16; row++) {
            unsigned char bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) cpx(c, x + col, y + row, fg);
                else if (bg) cpx(c, x + col, y + row, bg);
            }
        }
    }
}

static aios_img *img_new(int w, int h) {
    aios_img *im = calloc(1, sizeof *im + (size_t)w * h * 4);
    if (!im) return NULL;
    im->w = w; im->h = h;
    im->rgba = (uint8_t *)(im + 1);
    return im;
}

/* scheduler-green accent, shared by every fallback (one accent colour,
 * as ASSETS.md recommends so chrome can re-tint artist assets) */
#define AP_GREEN AIOS_RGB(0x4a, 0xe0, 0x8a)
#define AP_DARK  AIOS_RGB(0x14, 0x18, 0x24)
#define AP_DEEP  AIOS_RGB(0x0a, 0x0d, 0x16)

aios_img *aios_img_wallpaper_fallback(int w, int h) {
    aios_img *im = img_new(w, h);
    if (!im) return NULL;
    canvas c = { w, h, im->rgba };
    /* vertical gradient deep -> dark with a subtle scheduler-green horizon */
    for (int y = 0; y < h; y++) {
        int t = (y * 255) / (h > 1 ? h - 1 : 1);
        int r = 10 + (20 - 10) * t / 255;
        int g = 13 + (24 - 13) * t / 255;
        int b = 22 + (36 - 22) * t / 255;
        cfill(&c, 0, y, w, 1, 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b);
    }
    /* faint grid dots (the "planner" texture) */
    for (int y = 24; y < h; y += 24)
        for (int x = 24; x < w; x += 24)
            cpx(&c, x, y, 0x40314a52u);
    /* horizon accent line */
    cfill(&c, 0, h - 88, w, 1, 0x30314a52u);
    return im;
}

aios_img *aios_img_logo_fallback(int size) {
    aios_img *im = img_new(size, size);
    if (!im) return NULL;
    canvas c = { size, size, im->rgba };
    /* rounded dark square with a green core - "the scheduler chip" */
    int m = size / 8, r = size / 6;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int dx = x - size / 2, dy = y - size / 2;
            /* rounded-rect test */
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int qx = ax - (size / 2 - m - r), qy = ay - (size / 2 - m - r);
            if (ax > size / 2 - m || ay > size / 2 - m) continue;
            unsigned col = AP_DARK | 0xFF000000u;
            if (qx > 0 && qy > 0 && qx * qx + qy * qy > r * r) continue;
            cpx(&c, x, y, col);
        }
    int inner = size / 2 + size / 5;
    cfill(&c, size / 2 - inner / 2, size / 2 - inner / 2, inner, inner / 3, AP_GREEN | 0xE0000000u);
    cfill(&c, size / 2 - inner / 2, size / 2 + inner / 6, inner / 2, inner / 3, AIOS_RGB(0xb8, 0xc4, 0xd8) | 0xD0000000u);
    return im;
}

aios_img *aios_img_icon_fallback(const char *name, int size) {
    aios_img *im = img_new(size, size);
    if (!im) return NULL;
    canvas c = { size, size, im->rgba };
    unsigned fg = AIOS_RGB(0xd8, 0xe2, 0xf0) | 0xFF000000u;
    unsigned dim = AIOS_RGB(0x6a, 0x78, 0x90) | 0xC0000000u;
    unsigned grn = AP_GREEN | 0xFF000000u;
    int s = size;
    if (!strcmp(name, "terminal")) {
        crect(&c, s / 8, s / 6, s * 3 / 4, s * 2 / 3, fg);
        ctext(&c, s / 8 + s / 10, s / 6 + s / 8, ">_", grn, 0, 1);
    } else if (!strcmp(name, "files")) {
        cfill(&c, s / 6, s / 4, s * 2 / 3, s / 6, dim);
        cfill(&c, s / 8, s * 3 / 8, s * 3 / 4, s / 2, fg);
        cfill(&c, s / 8, s * 3 / 8, s * 3 / 4, s / 8, grn);
    } else if (!strcmp(name, "agent")) {
        crect(&c, s / 4, s / 5, s / 2, s / 2, fg);
        cfill(&c, s / 3, s / 3, s / 8, s / 8, grn);
        cfill(&c, s * 7 / 12, s / 3, s / 8, s / 8, grn);
        cfill(&c, s * 5 / 12, s * 3 / 5, s / 6, s / 8, dim);
        cfill(&c, s * 7 / 16, s * 7 / 10, s / 8, s / 5, dim);
    } else if (!strcmp(name, "memory")) {
        for (int i = 0; i < 3; i++)
            cfill(&c, s / 5, s / 3 + i * s / 5, s * 3 / 5 - (i * s) / 8, s / 7,
                  i == 1 ? grn : fg);
    } else if (!strcmp(name, "autopilot")) {
        /* gauge */
        for (int a = 0; a < 180; a += 2) {
            double rad = 3.14159 * a / 180.0;
            int cx = s / 2 + (int)(s / 3 * -cos(rad));
            int cy = s * 2 / 3 + (int)(s / 3 * sin(rad));
            cpx(&c, cx, cy, fg);
            cpx(&c, cx, cy + 1, fg);
        }
        cfill(&c, s / 2 - 1, s / 2, 2, s / 5, grn);
    } else if (!strcmp(name, "settings")) {
        crect(&c, s / 3, s / 3, s / 3, s / 3, fg);
        cfill(&c, s * 7 / 16, s * 7 / 16, s / 8, s / 8, grn);
        for (int i = 0; i < 4; i++) {
            int o = s * 3 / 16;
            cfill(&c, s / 3 - o / 2 + i * s * 5 / 24, s / 2 - s / 24, o / 2, s / 12, dim);
        }
    } else if (!strcmp(name, "shutdown")) {
        cfill(&c, s / 2 - 1, s / 6, 2, s / 3, fg);
        for (int a = -60; a <= 60; a += 2) {
            double rad = 3.14159 * a / 180.0;
            int cx = s / 2 + (int)(s / 4 * sin(rad));
            int cy = s * 7 / 12 + (int)(s / 4 * -cos(rad));
            cpx(&c, cx, cy, fg);
        }
    } else if (!strcmp(name, "network")) {
        cfill(&c, s / 2 - 1, s / 4, 2, s / 2, dim);
        for (int i = 0; i < 3; i++) {
            int w = s * (i + 1) / 4;
            cfill(&c, s / 2 - w / 2, s * 2 / 3 - i * s / 6, w, 2,
                  i == 2 ? grn : dim);
        }
    } else {
        crect(&c, s / 4, s / 4, s / 2, s / 2, fg);
    }
    return im;
}

aios_img *aios_img_cursor_fallback(void) {
    /* classic arrow, 32x32, white with black outline (hard-coded sprite) */
    static const char *rows[32] = {
        "X...............................", "XX..............................",
        "X#X.............................", "X##X............................",
        "X###X...........................", "X####X..........................",
        "X#####X.........................", "X######X........................",
        "X#######X.......................", "X########X......................",
        "X#########X.....................", "X##########X....................",
        "X###########X...................", "X############X..................",
        "X#########XXXXX.................", "X#X#X####X......................",
        "X#X.X#X###X.....................", "XX..X#X####X....................",
        ".....X#X###X....................", ".....X#X####X...................",
        "......X..X####X.................", "..........X####X................",
        "...........X###X................", "...........X##X.................",
        "............XX..................", "............X...................",
        "................................", "................................",
        "................................", "................................",
        "................................", "................................",
    };
    aios_img *im = img_new(32, 32);
    if (!im) return NULL;
    canvas c = { 32, 32, im->rgba };
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            char k = rows[y][x];
            if (k == 'X') cpx(&c, x, y, 0xFF101418u);
            else if (k == '#') cpx(&c, x, y, 0xFFF0F4FAu);
        }
    return im;
}
