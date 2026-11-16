/* test_fb.c — M10 desktop: aios-fb / window core / input keymap tests.
 *
 * Runs on the host and inside the booted OS (aios-fb has a memory test
 * seam, so no framebuffer device is needed): clipping, damage merging,
 * glyph blits, image loading, procedural fallbacks, the window
 * compositor (z-order, focus, close box, hit testing) and the evdev
 * keymap contract.
 */
#include "t.h"
#include "aios/fb.h"
#include "aios/win.h"
#include "aios/input.h"
#include <unistd.h>

/* ---------------- framebuffer basics -------------------------------- */

static void fb_basics(void) {
    aios_fb *f = aios_fb_open_mem(64, 48);
    CHECK(f != NULL, "mem fb opens");
    CHECK(aios_fb_w(f) == 64 && aios_fb_h(f) == 48, "dims");
    CHECK(aios_fb_px(f, 0, 0) == 0, "starts cleared");

    /* fill + clipping: rect wider than the screen clips at 64 */
    aios_rect r = { 60, 40, 20, 20 };
    aios_fb_fill(f, &r, AIOS_RGB(10, 20, 30));
    CHECK(aios_fb_px(f, 63, 47) == AIOS_RGB(10, 20, 30), "fill inside clip");
    CHECK(aios_fb_px(f, 59, 47) == 0, "left of rect untouched");
    CHECK(aios_fb_px(f, 63, 39) == 0, "above rect untouched");

    /* negative coords clip harmlessly */
    aios_rect neg = { -8, -8, 16, 16 };
    aios_fb_fill(f, &neg, AIOS_RGB(1, 2, 3));
    CHECK(aios_fb_px(f, 0, 0) == AIOS_RGB(1, 2, 3), "clipped at origin");
    CHECK(aios_fb_px(f, 7, 7) == AIOS_RGB(1, 2, 3), "bottom-right of clip");
    CHECK(aios_fb_px(f, 8, 8) == 0, "outside both rects");

    /* outline rect */
    aios_fb *g = aios_fb_open_mem(32, 32);
    aios_rect o = { 4, 4, 10, 6 };
    aios_fb_rect(g, &o, AIOS_RGB(9, 9, 9));
    CHECK(aios_fb_px(g, 4, 4) == AIOS_RGB(9, 9, 9), "outline corner");
    CHECK(aios_fb_px(g, 13, 9) == AIOS_RGB(9, 9, 9), "outline far corner");
    CHECK(aios_fb_px(g, 5, 5) == 0, "interior untouched");

    /* hline/vline */
    aios_fb_hline(g, 0, 31, 8, AIOS_RGB(7, 7, 7));
    CHECK(aios_fb_px(g, 7, 31) == AIOS_RGB(7, 7, 7), "hline");
    aios_fb_vline(g, 31, 0, 8, AIOS_RGB(6, 6, 6));
    CHECK(aios_fb_px(g, 31, 7) == AIOS_RGB(6, 6, 6), "vline");

    aios_fb_close(f);
    aios_fb_close(g);
}

/* ---------------- damage tracking ----------------------------------- */

static void fb_damage(void) {
    aios_fb *f = aios_fb_open_mem(100, 100);
    aios_fb_damage_reset(f);
    CHECK(aios_fb_damage(f) == NULL, "no damage after reset");

    aios_rect a = { 10, 10, 10, 10 };
    aios_fb_damage_add(f, &a);
    const aios_rect *d = aios_fb_damage(f);
    CHECK(d && d->x == 10 && d->y == 10 && d->w == 10 && d->h == 10,
          "first damage rect");

    aios_rect b = { 30, 40, 5, 5 };
    aios_fb_damage_add(f, &b);
    d = aios_fb_damage(f);
    CHECK(d->x == 10 && d->y == 10 && d->w == 25 && d->h == 35,
          "damage unions");

    /* drawing also accumulates damage */
    aios_fb_damage_reset(f);
    aios_rect c = { 2, 2, 4, 4 };
    aios_fb_fill(f, &c, AIOS_RGB(1, 1, 1));
    d = aios_fb_damage(f);
    CHECK(d && d->x == 2 && d->w == 4, "fill adds damage");

    /* text damage: 8 px per glyph */
    aios_fb_damage_reset(f);
    aios_fb_text(f, 20, 20, "abc", AIOS_RGB(1, 1, 1), 0, 0);
    d = aios_fb_damage(f);
    CHECK(d && d->w == 24 && d->h == 16, "text damage 8x16 per glyph");

    /* flush clears */
    aios_fb_flush(f);
    CHECK(aios_fb_damage(f) == NULL, "flush clears damage");

    aios_fb_close(f);
}

/* ---------------- text + glyphs ------------------------------------- */

static void fb_text(void) {
    aios_fb *f = aios_fb_open_mem(64, 32);
    aios_fb_text(f, 0, 0, "A", AIOS_RGB(255, 255, 255), 0, 0);
    int on = 0, untouched = 1;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (aios_fb_px(f, x, y)) on++;
    for (int x = 8; x < 16; x++)
        if (aios_fb_px(f, x, 0)) untouched = 0;
    CHECK(on > 4, "glyph has ink");
    CHECK(untouched, "adjacent cell untouched");
    CHECK(aios_fb_px(f, 0, 20) == 0, "below cell untouched");

    /* background fill variant */
    aios_fb *g = aios_fb_open_mem(32, 32);
    aios_fb_text(g, 0, 0, "W", AIOS_RGB(1, 2, 3), AIOS_RGB(40, 40, 40), 0);
    int bgpix = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            if (aios_fb_px(g, x, y) == AIOS_RGB(40, 40, 40)) bgpix++;
    CHECK(bgpix > 60, "bg variant paints the cell");

    /* bold glyph draws too */
    aios_fb_text(g, 16, 0, "X", AIOS_RGB(5, 5, 5), 0, 1);
    int bold_on = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 16; x < 24; x++)
            if (aios_fb_px(g, x, y)) bold_on++;
    CHECK(bold_on > 4, "bold variant has ink");

    /* width helper */
    CHECK(aios_fb_text_w("hello") == 40, "width = 8/char");
    CHECK(aios_fb_text_w("") == 0, "empty width 0");

    aios_fb_close(f);
    aios_fb_close(g);
}

/* ---------------- images -------------------------------------------- */

static void fb_images(void) {
    aios_img *wall = aios_img_wallpaper_fallback(320, 200);
    CHECK(wall != NULL, "wallpaper fallback");
    CHECK(aios_img_w(wall) == 320 && aios_img_h(wall) == 200, "wall dims");
    const unsigned char *top = aios_img_row(wall, 0);
    const unsigned char *bot = aios_img_row(wall, 199);
    CHECK(top[0] < bot[0] || top[2] < bot[2], "wallpaper is a gradient");
    aios_img_free(wall);

    aios_img *logo = aios_img_logo_fallback(64);
    CHECK(logo != NULL && aios_img_w(logo) == 64, "logo fallback");
    const unsigned char *corner = aios_img_row(logo, 0);
    CHECK(corner[3] == 0, "logo corner transparent (rounded)");
    const unsigned char *mid = aios_img_row(logo, 32) + 32 * 4;
    CHECK(mid[3] > 200, "logo center opaque");
    aios_img_free(logo);

    const char *names[] = { "terminal", "files", "agent", "memory",
                            "autopilot", "settings", "network", "shutdown" };
    for (int i = 0; i < 8; i++) {
        aios_img *ic = aios_img_icon_fallback(names[i], 48);
        CHECK(ic != NULL && aios_img_w(ic) == 48, "icon fallback opens");
        int ink = 0;
        for (int y = 0; y < 48; y++) {
            const unsigned char *row = aios_img_row(ic, y);
            for (int x = 0; x < 48; x++)
                if (row[x * 4 + 3] > 100) ink++;
        }
        CHECK(ink > 20, "icon has ink");
        aios_img_free(ic);
    }

    aios_img *cur = aios_img_cursor_fallback();
    CHECK(cur != NULL && aios_img_w(cur) == 32, "cursor fallback");
    const unsigned char *tip = aios_img_row(cur, 0);
    CHECK(tip[0] < 40 && tip[3] > 200, "cursor tip is dark ink");
    aios_img_free(cur);

    /* .aiosimg round trip through a temp file */
    const char *path = "/tmp/aios-test-img.aiosimg";
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL, "temp img file");
    unsigned char hdr[16] = { 'A', 'I', 'O', 'S', 'I', 'M', 'G', '1',
                              8, 0, 0, 0, 4, 0, 0, 0 };  /* 8x4 */
    fwrite(hdr, 1, 16, fp);
    for (int i = 0; i < 8 * 4; i++) {
        unsigned char px[4] = { 200, 30, 90, 255 };
        fwrite(px, 1, 4, fp);
    }
    fclose(fp);
    aios_img *im = aios_img_load(path);
    CHECK(im != NULL, "aiosimg loads");
    CHECK(aios_img_w(im) == 8 && aios_img_h(im) == 4, "aiosimg dims");
    const unsigned char *r0 = aios_img_row(im, 0);
    CHECK(r0[0] == 200 && r0[1] == 30 && r0[2] == 90 && r0[3] == 255,
          "aiosimg pixel values");
    aios_img_free(im);

    /* broken: bad magic, truncated body, missing file */
    fp = fopen(path, "wb");
    fwrite("XXXXXXXX", 1, 8, fp);
    fwrite(hdr + 8, 1, 8, fp);
    fclose(fp);
    CHECK(aios_img_load(path) == NULL, "bad magic rejected");
    fp = fopen(path, "wb");
    fwrite("AIOSIMG1", 1, 8, fp);
    unsigned char sz[8] = { 32, 0, 0, 0, 32, 0, 0, 0 };
    fwrite(sz, 1, 8, fp);
    fclose(fp);
    CHECK(aios_img_load(path) == NULL, "truncated body rejected");
    CHECK(aios_img_load("/tmp/does-not-exist.aiosimg") == NULL,
          "missing file rejected");
    unlink(path);

    /* blit with alpha respects transparency */
    aios_fb *f = aios_fb_open_mem(32, 32);
    aios_rect bg = { 0, 0, 32, 32 };
    aios_fb_fill(f, &bg, AIOS_RGB(50, 50, 50));
    aios_img *sp = aios_img_logo_fallback(16);
    aios_fb_blit(f, 0, 0, sp);
    CHECK(aios_fb_px(f, 0, 0) == AIOS_RGB(50, 50, 50),
          "transparent pixel keeps background");
    aios_img_free(sp);
    aios_fb_close(f);
}

/* ---------------- window core --------------------------------------- */

static void wm(void) {
    aios_fb *f = aios_fb_open_mem(200, 150);
    aios_wm *m = aios_wm_create(f);
    CHECK(m != NULL, "wm creates");

    aios_img *wall = aios_img_wallpaper_fallback(200, 150);
    aios_wm_set_wallpaper(m, wall);
    aios_img *cur = aios_img_cursor_fallback();
    aios_wm_set_cursor(m, cur);

    aios_rect r1 = { 10, 10, 100, 60 };
    aios_win *w1 = aios_wm_open(m, r1, "one", NULL);
    CHECK(w1 != NULL, "win1 opens");
    CHECK(aios_win_cw(w1) == 100, "canvas width = window width");
    CHECK(aios_win_ch(w1) == 60 - AIOS_TITLE_H, "canvas excludes title");
    CHECK(strcmp(aios_win_title(w1), "one") == 0, "title stored");

    aios_rect r2 = { 50, 50, 80, 50 };
    aios_win *w2 = aios_wm_open(m, r2, "two", NULL);
    CHECK(w2 != NULL, "win2 opens");

    /* last opened is top */
    CHECK(aios_wm_top(m) == w2, "top = last opened");
    /* hit test inside overlap returns the top window */
    CHECK(aios_wm_hit(m, 60, 60) == w2, "hit in overlap = top win");
    CHECK(aios_wm_hit(m, 20, 20) == w1, "hit in w1 only");
    CHECK(aios_wm_hit(m, 0, 0) == NULL, "hit on wallpaper = NULL");

    /* raise w1 -> now top */
    aios_wm_raise(m, w1);
    CHECK(aios_wm_top(m) == w1, "raise puts on top");
    CHECK(aios_wm_hit(m, 60, 60) == w1, "hit follows z-order");

    /* close box placement */
    aios_rect cb;
    aios_win_close_box(w1, &cb);
    CHECK(cb.w == 18 && cb.h == 18, "close box size");
    CHECK(cb.x == aios_win_x(w1) + 100 - 22, "close box in title right");
    CHECK(aios_win_on_title(w1, aios_win_x(w1) + 5, aios_win_y(w1) + 5),
          "on_title true on the bar");
    CHECK(!aios_win_on_title(w1, aios_win_x(w1) + 5, aios_win_y(w1) + 40),
          "on_title false in the body");

    /* move clamps to screen */
    aios_wm_move(m, w2, 500, 500);
    CHECK(aios_win_x(w2) <= 200 - 80, "move clamps x");
    CHECK(aios_win_y(w2) <= 150 - 50, "move clamps y");

    /* window drawing lands in the canvas, composited to the fb */
    aios_win_fill(w1, 0, 0, aios_win_cw(w1), aios_win_ch(w1),
                  AIOS_RGB(20, 40, 60));
    aios_win_text(w1, 4, 4, "hello", AIOS_RGB(255, 255, 255), 0, 0);
    aios_wm_damage_all(m);
    aios_wm_repaint(m);
    /* w1 body starts at y=10+24=34; check inside the body, away from the
     * 1px window border (the border outline is drawn over the body edge) */
    CHECK(aios_fb_px(f, 50, 40) == AIOS_RGB(20, 40, 60),
          "canvas composited to fb");
    CHECK(aios_fb_px(f, 14, 38) != 0, "window glyph ink");
    int title_ink = 0;
    for (int x = 10; x < 110; x++)
        for (int y = 10; y < 34; y++)
            if (aios_fb_px(f, x, y)) title_ink++;
    CHECK(title_ink > 100, "title bar rendered");

    /* repaint is a no-op when clean */
    aios_fb_damage_reset(f);
    aios_wm_repaint(m);
    CHECK(aios_fb_damage(f) == NULL, "clean repaint cheap");

    /* cursor move marks dirty again */
    aios_wm_cursor_move(m, 100, 80);
    aios_wm_repaint(m);
    CHECK(aios_fb_px(f, 100, 80) != 0, "cursor sprite pixel");

    /* close: z-order update; closing everything restores wallpaper */
    aios_wm_close(m, w2);
    CHECK(aios_wm_hit(m, 150, 145) == NULL, "closed win gone");
    aios_wm_close(m, w1);
    aios_wm_damage_all(m);
    aios_wm_repaint(m);
    CHECK(aios_fb_px(f, 50, 40) != AIOS_RGB(20, 40, 60),
          "wallpaper restored after close");

    aios_img_free(wall);
    aios_img_free(cur);
    aios_wm_destroy(m);
    aios_fb_close(f);
}

/* ---------------- input keymap contract ------------------------------ */

static void input_keymap(void) {
    /* open scans /dev/input — on hosts without evdev devices it still
     * returns a usable object (headless mode) */
    aios_input *in = aios_input_open();
    CHECK(in != NULL, "input opens (headless ok)");
    aios_input_set_screen(in, 1280, 800);
    aios_event ev;
    aios_input_poll(in, &ev, 1);        /* must not hang, no crash */
    aios_input_close(in);

    /* extended key constants are API-stable */
    CHECK(AIOS_KEY_UP == 0x101, "KEY_UP");
    CHECK(AIOS_KEY_DOWN == 0x102, "KEY_DOWN");
    CHECK(AIOS_KEY_F(1) == 0x141, "F1");
    CHECK(AIOS_KEY_HOME == 0x120, "HOME");
}

static void run(void) {
    fb_basics();
    fb_damage();
    fb_text();
    fb_images();
    wm();
    input_keymap();
}

int main(void) {
    run();
    printf("%s: %d checks, %d failed\n",
           g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
