/* input.c — M10 desktop: raw evdev keyboard + pointer (see aios/input.h).
 *
 * The keymap is the standard US layout over linux/input-event-codes.h
 * codes (a stable kernel API). The pointer understands virtio tablets
 * (absolute) and mice (relative); positions are scaled/clamped to the
 * screen size set by aios_input_set_screen().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <linux/input.h>

#include "aios/input.h"

#define AIOS_MOD_SHIFT 1
#define AIOS_MOD_CTRL  2
#define AIOS_MOD_ALT   4

#define EVQ 64

struct aios_input {
    int kfd;              /* keyboard, -1 when none   */
    int pfd;              /* pointer, -1 when none    */
    int abs;              /* pointer is absolute      */
    int ax_max, ay_max;   /* ABS axis maxima          */
    int sw, sh;           /* screen size (scaling)    */
    int mx, my;
    int move_pending;
    int shift, ctrl, alt, caps, numlock;
    /* small decoded-event ring (single consumer) */
    aios_event q[EVQ];
    int qh, qt;
};

/* US layout: evdev code -> (normal, shifted) */
static int key_ascii(unsigned code, int shift) {
    static const struct { unsigned code; char a, b; } map[] = {
        { 2, '1', '!' }, { 3, '2', '@' }, { 4, '3', '#' }, { 5, '4', '$' },
        { 6, '5', '%' }, { 7, '6', '^' }, { 8, '7', '&' }, { 9, '8', '*' },
        { 10, '9', '(' }, { 11, '0', ')' },
        { 12, '-', '_' }, { 13, '=', '+' },
        { 16, 'q', 'Q' }, { 17, 'w', 'W' }, { 18, 'e', 'E' }, { 19, 'r', 'R' },
        { 20, 't', 'T' }, { 21, 'y', 'Y' }, { 22, 'u', 'U' }, { 23, 'i', 'I' },
        { 24, 'o', 'O' }, { 25, 'p', 'P' },
        { 26, '[', '{' }, { 27, ']', '}' },
        { 30, 'a', 'A' }, { 31, 's', 'S' }, { 32, 'd', 'D' }, { 33, 'f', 'F' },
        { 34, 'g', 'G' }, { 35, 'h', 'H' }, { 36, 'j', 'J' }, { 37, 'k', 'K' },
        { 38, 'l', 'L' },
        { 39, ';', ':' }, { 40, '\'', '"' }, { 41, '`', '~' },
        { 43, '\\', '|' },
        { 44, 'z', 'Z' }, { 45, 'x', 'X' }, { 46, 'c', 'C' }, { 47, 'v', 'V' },
        { 48, 'b', 'B' }, { 49, 'n', 'N' }, { 50, 'm', 'M' },
        { 51, ',', '<' }, { 52, '.', '>' }, { 53, '/', '?' },
        { 0, 0, 0 },
    };
    for (int i = 0; map[i].code; i++)
        if (map[i].code == code)
            return shift ? map[i].b : map[i].a;
    return 0;
}

/* keypad with numlock on */
static int keypad_ascii(unsigned code) {
    switch (code) {
        case 71: return '7'; case 72: return '8'; case 73: return '9';
        case 75: return '4'; case 76: return '5'; case 77: return '6';
        case 79: return '1'; case 80: return '2'; case 81: return '3';
        case 82: return '0'; case 83: return '.';
        case 74: return '-'; case 78: return '+';
        case 96: return '\n';
        default: return 0;
    }
}

static int decode_key(aios_input *in, unsigned code, int down) {
    /* modifiers + locks first */
    switch (code) {
        case 29: case 97: in->ctrl = down; return 0;
        case 42: case 54: in->shift = down; return 0;
        case 56: case 100: in->alt = down; return 0;
        case 58: if (down) in->caps = !in->caps; return 0;
        case 69: if (down) in->numlock = !in->numlock; return 0;
        default: break;
    }
    if (!down) return 0;
    int shift = in->shift ^ in->caps;
    switch (code) {
        case 1:  return 0x1b;                       /* ESC      */
        case 14: return 0x08;                       /* BKSP     */
        case 15: return '\t';
        case 28: return '\n';
        case 57: return ' ';
        case 102: return AIOS_KEY_HOME;
        case 103: return AIOS_KEY_UP;
        case 104: return AIOS_KEY_PGUP;
        case 105: return AIOS_KEY_LEFT;
        case 106: return AIOS_KEY_RIGHT;
        case 107: return AIOS_KEY_END;
        case 108: return AIOS_KEY_DOWN;
        case 109: return AIOS_KEY_PGDN;
        case 111: return 0x7f;                      /* DEL      */
        default: break;
    }
    if (code >= 59 && code <= 68) return AIOS_KEY_F(code - 58);  /* F1-F10 */
    if (code == 87 || code == 88) return AIOS_KEY_F(code - 76);  /* F11/F12 */
    int kp = keypad_ascii(code);
    if (kp) return (in->numlock || kp == '\n') ? kp : 0;
    return key_ascii(code, shift);
}

/* ---- device discovery --------------------------------------------- */

static int has_cap(int fd, int evtype, unsigned bit) {
    unsigned char bits[(KEY_MAX + 7) / 8 + 1];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(evtype, sizeof bits), bits) < 0) return 0;
    return bits[bit / 8] & (1 << (bit % 8));
}

aios_input *aios_input_open(void) {
    aios_input *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->kfd = in->pfd = -1;
    in->numlock = 1;
    in->ax_max = in->ay_max = 4095;
    DIR *d = opendir("/dev/input");
    if (!d) return in;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        if (strlen(e->d_name) > 20) continue;   /* event names are short */
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        int is_kbd = has_cap(fd, EV_KEY, KEY_A) && has_cap(fd, EV_KEY, KEY_SPACE);
        int is_abs = has_cap(fd, EV_ABS, ABS_X);
        int is_rel = has_cap(fd, EV_REL, REL_X);
        if (is_kbd && in->kfd < 0) {
            in->kfd = fd;
        } else if ((is_abs || is_rel) && in->pfd < 0) {
            in->pfd = fd;
            in->abs = is_abs;
            if (is_abs) {
                struct input_absinfo ax, ay;
                if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ax.maximum > 0)
                    in->ax_max = ax.maximum;
                if (ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ay.maximum > 0)
                    in->ay_max = ay.maximum;
            }
        } else {
            close(fd);
        }
    }
    closedir(d);
    return in;
}

void aios_input_set_screen(aios_input *in, int w, int h) {
    if (in && w > 0 && h > 0) { in->sw = w; in->sh = h; }
}

void aios_input_close(aios_input *in) {
    if (!in) return;
    if (in->kfd >= 0) close(in->kfd);
    if (in->pfd >= 0) close(in->pfd);
    free(in);
}

int aios_input_ok(const aios_input *in) {
    return in && (in->kfd >= 0 || in->pfd >= 0);
}

/* ---- event pump ---------------------------------------------------- */

static void evq_push(aios_input *in, const aios_event *e) {
    int next = (in->qt + 1) % EVQ;
    if (next == in->qh) return;           /* full: drop */
    in->q[in->qt] = *e;
    in->qt = next;
}

static void clamp_pos(aios_input *in) {
    if (in->mx < 0) in->mx = 0;
    if (in->my < 0) in->my = 0;
    if (in->sw > 0 && in->mx > in->sw - 1) in->mx = in->sw - 1;
    if (in->sh > 0 && in->my > in->sh - 1) in->my = in->sh - 1;
}

/* decode one raw evdev event into the queue */
static int g_input_debug = -1;

static void pump_one(aios_input *in, int is_pointer, struct input_event *ie) {
    aios_event e;
    memset(&e, 0, sizeof e);
    if (g_input_debug < 0)
        g_input_debug = getenv("AIOS_INPUT_DEBUG") != NULL;
    if (g_input_debug)
        fprintf(stderr, "[input] fd=%s type=%u code=%u value=%d\n",
                is_pointer ? "ptr" : "kbd", ie->type, ie->code, ie->value);
    if (ie->type == EV_KEY) {
        if (is_pointer) return;
        if (ie->value == 2) return;    /* kernel autorepeat: the desktop
                                          does its own key repeat */
        int k = decode_key(in, ie->code, ie->value == 1);
        if (!k) return;   /* modifier/lock/unmapped: no key event at all
                             (raw codes are NOT ascii - emitting them made
                             shift-down type '*' into the shell) */
        e.type = AIOS_EV_KEY;
        e.key = k;
        e.key_down = ie->value == 1;
        e.mods = (in->shift ? AIOS_MOD_SHIFT : 0)
               | (in->ctrl ? AIOS_MOD_CTRL : 0)
               | (in->alt ? AIOS_MOD_ALT : 0);
        evq_push(in, &e);
    } else if (ie->type == EV_ABS && is_pointer) {
        if (ie->code == ABS_X) {
            in->mx = ie->value * in->sw / (in->ax_max > 0 ? in->ax_max : 1);
            in->move_pending = 1;
        } else if (ie->code == ABS_Y) {
            in->my = ie->value * in->sh / (in->ay_max > 0 ? in->ay_max : 1);
            in->move_pending = 1;
        }
    } else if (ie->type == EV_REL && is_pointer) {
        if (ie->code == REL_X) { in->mx += ie->value; in->move_pending = 1; }
        else if (ie->code == REL_Y) { in->my += ie->value; in->move_pending = 1; }
        else if (ie->code == REL_WHEEL) {
            e.type = AIOS_EV_MOUSE_WHEEL;
            e.wheel = ie->value > 0 ? 1 : -1;
            evq_push(in, &e);
        }
    } else if (ie->type == EV_KEY && is_pointer) {
        if (ie->code == BTN_LEFT || ie->code == BTN_RIGHT) {
            e.type = AIOS_EV_MOUSE_BTN;
            e.btn = ie->code == BTN_LEFT ? 1 : 2;
            e.btn_down = ie->value != 0;
            e.mx = in->mx; e.my = in->my;
            evq_push(in, &e);
        }
    } else if (ie->type == EV_SYN) {
        if (in->move_pending) {
            in->move_pending = 0;
            clamp_pos(in);
            e.type = AIOS_EV_MOUSE_MOVE;
            e.mx = in->mx; e.my = in->my;
            evq_push(in, &e);
        }
    }
}

int aios_input_poll(aios_input *in, aios_event *out, int timeout_ms) {
    if (!in) return -1;

    if (in->qh != in->qt) {
        *out = in->q[in->qh];
        in->qh = (in->qh + 1) % EVQ;
        return 1;
    }

    int nfds = 0;
    struct pollfd pfds[2];
    if (in->kfd >= 0) { pfds[nfds].fd = in->kfd; pfds[nfds].events = POLLIN; nfds++; }
    if (in->pfd >= 0) { pfds[nfds].fd = in->pfd; pfds[nfds].events = POLLIN; nfds++; }
    if (nfds == 0) {
        /* headless/CI: sleep out the timeout so the desktop keeps its
         * frame pace without busy-spinning */
        struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        return 0;
    }

    int r = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (r <= 0) return 0;
    for (int i = 0; i < nfds; i++) {
        if (!(pfds[i].revents & POLLIN)) continue;
        struct input_event ev[32];
        ssize_t n = read(pfds[i].fd, ev, sizeof ev);
        if (n <= 0) continue;
        size_t count = (size_t)n / sizeof ev[0];
        int is_pointer = (pfds[i].fd == in->pfd);
        for (size_t k = 0; k < count; k++)
            pump_one(in, is_pointer, &ev[k]);
    }
    if (in->qh != in->qt) {
        *out = in->q[in->qh];
        in->qh = (in->qh + 1) % EVQ;
        return 1;
    }
    return 0;
}
