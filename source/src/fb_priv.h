/* fb_priv.h — private struct shared between fb.c and win.c (not part of
 * the public aios/ API; the public surface stays opaque). */
#ifndef AIOS_FB_PRIV_H
#define AIOS_FB_PRIV_H

#include <stdint.h>
#include "aios/fb.h"

struct aios_fb {
    int w, h;
    uint32_t *back;        /* w*h, 0xAARRGGBB double buffer  */
    uint32_t *hw;          /* mmap'd framebuffer (NULL mem)  */
    int fd;
    int dirty;             /* damage valid                   */
    aios_rect dmg;
};

#endif /* AIOS_FB_PRIV_H */
