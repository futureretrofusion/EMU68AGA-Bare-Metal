/* AGA-PISTORM - HDMI sync (1.114): the HDMI frame follows the emulated one.
 *
 * With a fixed HDMI rate a PAL frame (49.92 Hz) on 1080p50 (50.00 Hz) has to
 * be repeated or dropped every 12.5 s, and on 60 Hz it judders all the time.
 * The emulated chipset keeps its exact PAL or NTSC rate - its sound, the real
 * Paula's and HDMI's, stays as it is - and the HDMI frame is made a line or
 * two longer instead: the vertical front porch of the PixelValve that times
 * HDMI (aga_hvs.c), within the mode's own porch and 16 lines more. RGB2RTG's
 * HDMI sync, which bends HDMI to a real A1200's frames (its rgb_follow.c,
 * Astair86, 2026); here the frames are the kernel's own, so the phase is
 * simply the HVS's line when one is presented.
 *
 * This is the controller, plain C so the host tests drive it
 * (host/tests/test_sync.c): from the phase of each frame it learns how many
 * porch lines the frame length needs - in whole HDMI lines a fraction more
 * than the mode has (1080p50: PAL needs about 1.8) - and steers the phase to
 * a target in the middle of the picture, where a late or early frame still
 * lands in the right HDMI frame. Far from it, the porch pulls the phase in up
 * to four lines a frame; near it, only the two whole numbers either side of
 * the need alternate, and only when the phase has drifted 3/20 of the
 * picture - RGB2RTG's steady mode: an LCD that flickered as the frame length
 * changed 8 times a second sees it change a few times a minute.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_SYNC_H
#define AGA_SYNC_H

#include <stdint.h>

#define HS_KMAX   16          /* porch lines over the mode's own, at most */
#define HS_FIRST  32          /* the first measure, frames */
#define HS_BLOCK  128         /* every later one */
#define HS_PULL   4           /* far from the target: up to this many lines past the need */
#define HS_NEAR   128         /* a frame's phase moved at most this far: measured (a need of 16 lines
                                 and 1 ms of jitter at either end; PAL on 60 Hz moves 227, NTSC on 50 Hz 185) */
#define HS_FAR    32          /* frames in a row it moved further: the need is out of reach */

struct hsync {
    int prev;                 /* the last frame's phase, -1 none */
    int n;                    /* frames in the measure under way */
    int32_t moved, ksum;      /* ...the phase's movement over them, the porch lines in force */
    int blocks;               /* measures done */
    int32_t need16;           /* the porch lines a frame needs, 16.16 */
    int32_t ph16;             /* the phase, smoothed, 16.16; -1 none */
    int k;                    /* the porch lines asked for */
    int steady;               /* inside the band: only the two either side of the need */
    int far;                  /* frames in a row the phase moved too far to measure */
    int reach;                /* 0 learning, 1 steering, -1 out of reach (needs more or less than the porch allows) */
    uint32_t slips;           /* phase jumps of half a picture or more: a frame shown twice or not at all */
};

static inline void hsync_reset(struct hsync *s)
{
    s->prev = -1; s->n = 0; s->moved = 0; s->ksum = 0; s->blocks = 0; s->need16 = 0; s->ph16 = -1;
    s->k = 0; s->steady = 0; s->far = 0; s->reach = 0; s->slips = 0;
}

/* One frame: `phase` the HVS line it was presented at (0..vactive-1; -1 in
   the blank, where the line counter stands still), `k_now` the porch lines
   in force while it ran. Returns the porch lines wanted from now on.

   The need is measured over a block of frames: the phase's whole movement
   across it, plus the porch lines in force, divided by the frames - a
   present a little early or late then only counts at the block's two ends.
   A jump of half a picture (a frame repeated or dropped, a stall) starts
   the block again; frames moving too far to be any porch's, many in a row,
   mean the mode's rate is out of reach (PAL on 60 Hz). */
static inline int hsync_step(struct hsync *s, int phase, int vactive, int k_now)
{
    if (phase < 0 || vactive < 200) { s->prev = -1; return s->k; }
    int restart = 0;
    if (s->prev >= 0) {
        int d = phase - s->prev, ad = d < 0 ? -d : d;
        if (ad <= HS_NEAR) {
            s->far = 0;
            s->moved += d;
            s->ksum += k_now;
            if (++s->n >= (s->blocks ? HS_BLOCK : HS_FIRST)) {
                int32_t est = (int32_t)(((int64_t)(s->moved + s->ksum) << 16) / s->n);
                s->need16 = s->blocks ? s->need16 + (est - s->need16) / 2 : est;
                s->blocks++;
                s->n = 0; s->moved = 0; s->ksum = 0;
            }
        } else {
            if (ad > vactive / 2) s->slips++;
            else s->far++;
            restart = 1;
        }
    }
    if (restart) { s->n = 0; s->moved = 0; s->ksum = 0; }
    s->prev = phase;
    s->ph16 = (s->ph16 < 0 || restart) ? phase << 16 : s->ph16 + ((phase << 16) - s->ph16) / 8;
    if (s->far >= HS_FAR) {                     /* PAL on 60 Hz, NTSC on 50 Hz: the mode's own porch */
        s->reach = -1; s->blocks = 0; s->steady = 0;
        return s->k = 0;
    }
    if (!s->blocks) return s->k;
    if (s->need16 < -(1 << 15) || s->need16 > (HS_KMAX << 16) + (1 << 15)) {
        s->reach = -1; s->steady = 0;
        return s->k = 0;
    }
    s->reach = 1;
    int need = (int)((s->need16 + (1 << 15)) >> 16);                 /* rounded */
    int lo = s->need16 < 0 ? 0 : (int)(s->need16 >> 16), hi = lo + 1;
    if (hi > HS_KMAX) hi = HS_KMAX;
    if (lo > HS_KMAX) lo = HS_KMAX;
    int target = vactive * 11 / 20, band = vactive * 3 / 20;
    int err = (s->ph16 >> 16) - target;         /* late: a longer HDMI frame pulls it back */
    int k;
    if (err > 2 * band || err < -2 * band) {
        int pull = err / 32;
        if (pull > HS_PULL) pull = HS_PULL;
        if (pull < -HS_PULL) pull = -HS_PULL;
        k = need + pull;
        s->steady = 0;
    } else if (!s->steady && (err > band || err < -band)) {
        k = err > 0 ? hi : lo;                  /* coming in: gently */
    } else {
        s->steady = 1;
        k = s->k;
        if (k != lo && k != hi) k = need > lo ? hi : lo;
        if (err > band) k = hi;
        else if (err < -band) k = lo;
    }
    if (k < 0) k = 0;
    if (k > HS_KMAX) k = HS_KMAX;
    return s->k = k;
}

#endif
