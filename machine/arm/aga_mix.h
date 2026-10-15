/* AGA-PISTORM - the Sound window's mix: stereo width, left and right swapped,
 * and the volume of sound over HDMI.
 *
 * Paula's channels are hard left (0, 3) and right (1, 2). Width and swap work
 * on the mix both outputs take - the Amiga's own audio output and HDMI - in
 * aga_audio_stream; at full width, not swapped, the samples are the core's
 * bit for bit. The volume is HDMI's alone (the real Paula has no headroom to
 * give): above 100 % a soft knee from -2.5 dBFS rounds the peaks off instead
 * of clipping them.
 *
 * The width and knee follow RGB2RTG's kernel/rgb_hdmi.c mix() and soft()
 * (GPL-2.0-only, by the same author). Pure C with no kernel headers:
 * host/tests/test_mix.c includes it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_MIX_H
#define AGA_MIX_H

#include <stdint.h>

/* the Sound window's steps, index -> per cent (functions, not tables: an
   unused static const table is an error under the kernel's -Werror) */
static inline int mix_vol_pc(int i)          /* HDMI volume; 200 = 1.113's GAIN 2 */
{
    static const int t[5] = { 100, 150, 200, 300, 400 };
    return (i >= 0 && i < 5) ? t[i] : 200;
}
static inline int mix_width_pc(int i)        /* stereo: the Amiga's, ..., mono */
{
    static const int t[5] = { 100, 75, 50, 25, 0 };
    return (i >= 0 && i < 5) ? t[i] : 100;
}

#define MIX_KNEE 24576                       /* -2.5 dBFS */

/* one frame's width and swap, in place. width in 16.16 (0x10000 = the
   Amiga's own separation, 0 = mono). The result is a weighted average of
   the two sides, so it never exceeds the louder of them. */
static inline void mix_stereo(int16_t *l, int16_t *r, int32_t width16, int swap)
{
    int32_t a = *l, b = *r;
    if (width16 != 0x10000) {
        int64_t m = (int64_t)(a + b) << 16, s = (int64_t)(a - b) * width16;
        a = (int32_t)((m + s) >> 17);
        b = (int32_t)((m - s) >> 17);
    }
    if (swap) { int32_t t = a; a = b; b = t; }
    *l = (int16_t)a;
    *r = (int16_t)b;
}

/* the soft knee: straight up to MIX_KNEE, then towards full scale and never past */
static inline int16_t mix_soft(int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    if (a <= MIX_KNEE) return (int16_t)v;
    int32_t e = a - MIX_KNEE, k = 32767 - MIX_KNEE;
    a = MIX_KNEE + (int32_t)(((int64_t)k * e) / ((int64_t)k + e));
    return (int16_t)(v < 0 ? -a : a);
}

/* one HDMI sample at gain16 (16.16; 0x10000 = 100 %): up to 100 % straight
   and clipped as before, above it through the knee */
static inline int16_t mix_level(int32_t v, int32_t gain16)
{
    if (gain16 <= 0x10000) {
        if (gain16 != 0x10000) v = (int32_t)(((int64_t)v * gain16) >> 16);
        return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    int64_t g = ((int64_t)v * gain16) >> 16;
    if (g > 0x7fffffff) g = 0x7fffffff;
    if (g < -0x7fffffff) g = -0x7fffffff;
    return mix_soft((int32_t)g);
}

#endif
