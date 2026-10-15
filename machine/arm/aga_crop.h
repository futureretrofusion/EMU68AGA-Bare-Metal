/* AGA-PISTORM - the custom crop (agaboot RTG Image: Size Custom, Crop...):
 * the four edges the user cuts off the frame, PAL's and NTSC's, and the
 * control words that carry them. Plain C on its own, so the host tests check
 * it (host/tests/test_crop.c); aga_hvs.c is the only kernel user.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_CROP_H
#define AGA_CROP_H

#include <stdint.h>

#define CROP_FRAME_W    800     /* AGA_OUT_W: hires pixels from lores column $50 */
#define CROP_ROWS_PAL   286     /* AGA_OUT_H */
#define CROP_ROWS_NTSC  237     /* NTSC's frame ends at line 262, row 236 (aga_hvs.c) */
#define CROP_MIN_W      320     /* what is left at least, hires pixels... */
#define CROP_MIN_H      100     /* ...and lines */
enum { CUT_L, CUT_T, CUT_R, CUT_B };

/* Until the user cuts their own: 720 x 272's edges, the size a TV shows
   (aga_hvs.c zoom_rect: x 74, y 10 in both standards) */
#define CROP_DEFAULT_PAL  { 74, 10, 6, 4 }
#define CROP_DEFAULT_NTSC { 74, 10, 6, 0 }

/* The rectangle the edges leave: x, y, w, h in the frame. Left and right in
   steps of two hires pixels (one lores pixel), which keeps the plane pointer
   on eight bytes; edges that would leave less than the minimum give way -
   the right one to the left, the bottom one to the top. */
static inline void crop_cut_rect(const uint16_t *cut, int ntsc, int *x, int *y, int *w, int *h)
{
    int rows = ntsc ? CROP_ROWS_NTSC : CROP_ROWS_PAL;
    int l = cut[CUT_L] & ~1, t = cut[CUT_T], r = cut[CUT_R] & ~1, b = cut[CUT_B];
    if (l > CROP_FRAME_W - CROP_MIN_W) l = CROP_FRAME_W - CROP_MIN_W;
    if (r > CROP_FRAME_W - CROP_MIN_W - l) r = CROP_FRAME_W - CROP_MIN_W - l;
    if (t > rows - CROP_MIN_H) t = rows - CROP_MIN_H;
    if (b > rows - CROP_MIN_H - t) b = rows - CROP_MIN_H - t;
    *x = l;
    *y = t;
    *w = CROP_FRAME_W - l - r;
    *h = rows - t - b;
}

/* Size Auto (1.114, RGB2RTG's Crop: Auto): the game's own display window,
   as the core measured it on the lines with a picture (aga_last_frame_window:
   hires pixels [x0, x1), lines y0..y1, in the frame), followed while the
   game runs. A bigger window is taken at once, a smaller one only once a
   whole period of CROP_AUTO_QUIET frames has used nothing bigger - 2 to 4 s
   after the last bigger frame, the period that frame was in not counting -
   so that a menu or a fade does not pump the picture. Left and right whole lores pixels; a window smaller than
   160 x 64 is not taken for the picture. */
#define CROP_AUTO_QUIET 100     /* 2 s at 50 Hz */
struct crop_auto {
    int x0, x1, y0, y1;         /* shown; x0 < 0: none yet */
    int cx0, cx1, cy0, cy1;     /* what the frames since the last change used */
    int quiet;
};
static inline void crop_auto_reset(struct crop_auto *s) { s->x0 = -1; s->quiet = 0; }

/* one frame's window (ok 0: none this frame); 1 when the one shown changed */
static inline int crop_auto_track(struct crop_auto *s, int rows, int ok, int x0, int x1, int y0, int y1)
{
    if (!ok) return 0;
    if (x0 < 0) x0 = 0;
    if (x1 > CROP_FRAME_W) x1 = CROP_FRAME_W;
    if (y0 < 0) y0 = 0;
    if (y1 > rows - 1) y1 = rows - 1;
    x0 &= ~1;
    x1 = (x1 + 1) & ~1;
    if (x1 - x0 < 160 || y1 - y0 + 1 < 64) return 0;
    if (s->x0 < 0 || x0 < s->x0 || x1 > s->x1 || y0 < s->y0 || y1 > s->y1) {
        if (s->x0 < 0) {
            s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1;
        } else {                                /* the two together: nothing shown is lost */
            if (x0 < s->x0) s->x0 = x0;
            if (x1 > s->x1) s->x1 = x1;
            if (y0 < s->y0) s->y0 = y0;
            if (y1 > s->y1) s->y1 = y1;
        }
        s->cx0 = x0; s->cx1 = x1; s->cy0 = y0; s->cy1 = y1;
        s->quiet = 0;
        return 1;
    }
    if (x0 < s->cx0) s->cx0 = x0;
    if (x1 > s->cx1) s->cx1 = x1;
    if (y0 < s->cy0) s->cy0 = y0;
    if (y1 > s->cy1) s->cy1 = y1;
    if (++s->quiet < CROP_AUTO_QUIET) return 0;
    int moved = s->cx0 != s->x0 || s->cx1 != s->x1 || s->cy0 != s->y0 || s->cy1 != s->y1;
    s->x0 = s->cx0; s->x1 = s->cx1; s->y0 = s->cy0; s->y1 = s->cy1;
    s->cx0 = x0; s->cx1 = x1; s->cy0 = y0; s->cy1 = y1;
    s->quiet = 0;
    return moved;
}

/* The words at $DFF1F2. Bits 1 and 15 stay clear in every one: a kernel
   without them stores the word as the core's aga_ctrl, where those two mean
   "windowed" and "use the register" (vectors.c).
     $58C0 + E5(d)   five more bits of a value: latch = latch << 5 | d
     $5870 + E3(s)   the value into slot s = edge + 4 x NTSC, latch cleared
     $5880 + E3(s)   the crop screen (desktop only): standard s >> 2, the
                     edge s & 3 marked; $5930 ends it, as it ends Position
   A value is two data words, ten bits: an edge is 480 at most. */
#define CROP_E3(s)  ((uint16_t)(((s) & 1) | (((s) & 6) << 1)))
#define CROP_E5(d)  ((uint16_t)(((d) & 1) | (((d) & 0x1E) << 1)))
#define CROP_D3(v)  ((int)(((v) & 1) | (((v) >> 1) & 6)))
#define CROP_D5(v)  ((int)(((v) & 1) | (((v) >> 1) & 0x1E)))
#define CROP_W_DATA   0x58C0
#define CROP_W_STORE  0x5870
#define CROP_W_SCREEN 0x5880

#endif
