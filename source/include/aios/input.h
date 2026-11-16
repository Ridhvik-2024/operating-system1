/* aios/input.h — M10 desktop: evdev keyboard + pointer.
 *
 * Reads /dev/input/event* directly (no X, no libevdev). The keyboard is
 * the device that carries KEY_A; the pointer is the one carrying
 * ABS_X (virtio tablet) or REL_X (virtio mouse). Both are optional: with
 * no devices the desktop still renders (CI/headless), poll() just times
 * out.
 */
#ifndef AIOS_INPUT_H
#define AIOS_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIOS_EV_NONE = 0,
    AIOS_EV_KEY,          /* key: ascii/extended code; key_down 1|0     */
    AIOS_EV_MOUSE_MOVE,   /* mx,my = absolute position                  */
    AIOS_EV_MOUSE_BTN,    /* btn 1 left|2 right; btn_down 1|0           */
    AIOS_EV_MOUSE_WHEEL   /* wheel +1 up | -1 down                      */
} aios_ev_type;

/* extended key codes (above ASCII) for the shell's line editor */
#define AIOS_KEY_UP     0x101
#define AIOS_KEY_DOWN   0x102
#define AIOS_KEY_LEFT   0x103
#define AIOS_KEY_RIGHT  0x104
#define AIOS_KEY_HOME   0x120
#define AIOS_KEY_END    0x121
#define AIOS_KEY_PGUP   0x122
#define AIOS_KEY_PGDN   0x123
#define AIOS_KEY_F(n)   (0x140 + (n))   /* F1 = 0x141 */

typedef struct {
    aios_ev_type type;
    int key;          /* AIOS_EV_KEY: ascii or AIOS_KEY_*               */
    int key_down;     /* 1 = press, 0 = release                         */
    int mods;         /* 1 shift | 2 ctrl | 4 alt (state at the event)  */
    int mx, my;       /* pointer position                               */
    int btn;          /* 1 left, 2 right                                */
    int btn_down;
    int wheel;
} aios_event;

typedef struct aios_input aios_input;

/* Scan /dev/input/event*; never fails hard — check aios_input_ok(). */
aios_input *aios_input_open(void);
void        aios_input_set_screen(aios_input *in, int w, int h);
void        aios_input_close(aios_input *in);
int         aios_input_ok(const aios_input *in);

/* Wait up to timeout_ms for the next decoded event; returns 1 with
 * out filled, 0 on timeout/quiet, <0 when all devices are gone. */
int aios_input_poll(aios_input *in, aios_event *out, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_INPUT_H */
