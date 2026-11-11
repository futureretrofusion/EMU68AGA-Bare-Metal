/* AGA-PISTORM - even pixels with the Sharp look (1.114).
 *
 * Sharp scaled with a nearest-neighbour kernel. Where the picture is enlarged
 * by a factor that is not whole - Fill, 4:3, Auto: 260 lines to 1080 rows are
 * 4.15 each - nearest gives some source pixels a row or a column more than
 * their neighbours, and text, dot patterns and scrolling show it. The area
 * kernel ("sharp bilinear") instead lets each output pixel take the source
 * pixels it covers, in proportion: every source pixel is as wide as the
 * next, and only the one row or column a pixel edge crosses mixes the two.
 * A whole factor keeps nearest - the two are the same there, and the HVS's
 * eight kernel phases could only blur it. RGB2RTG's even pixels (its
 * VideoCore.card scanlines.c area_kernel / uneven_step, Astair86, 2026).
 *
 * The kernel layout is the Linux vc4 driver's (drivers/gpu/drm/vc4/vc4_hvs.c)
 * on both HVS generations: four taps, eight phases, 32 coefficients at 1/8
 * pixel steps over -2..+2 source pixels, symmetric, so 16 are stored -
 * number i for the distance (15.5 - i) / 8 - three 9-bit ones to a word, the
 * first five words mirrored after the sixth. One phase's four taps are
 * numbers i, 7 - i, 8 + i and 15 - i (i < 4): they add up to 256, at most
 * 255 each. Plain C, so the host tests check it (host/tests/test_sharp.c).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_SHARP_H
#define AGA_SHARP_H

#include <stdint.h>

#define SHARP_KERNEL_WORDS 11

/* `step`: the PPF's, source pixels an output pixel in 16.16. 1 when it
   enlarges by a factor that is not whole, up to the 16.16 division's
   rounding - where nearest gives uneven pixels. Not when it shrinks: the TV
   border can. */
static inline int sharp_uneven(uint32_t step)
{
    if (step == 0 || step >= 0x10000) return 0;
    uint32_t n = (0x10000 + step / 2) / step;          /* the nearest whole factor */
    int32_t err = (int32_t)(n * step) - 0x10000;
    return (err < 0 ? -err : err) > (int32_t)(2 * n);
}

/* The area kernel for `step` into w[11]: coefficient (0.5 + step/2 - |x|) /
   step, between 0 and 1, each phase's four then made to add up to 256. */
static inline void sharp_area_kernel(uint32_t step, uint32_t w[SHARP_KERNEL_WORDS])
{
    int32_t c[16];
    uint32_t half[6] = { 0, 0, 0, 0, 0, 0 };
    if (!step) step = 1;
    for (int i = 0; i < 16; i++) {
        int32_t num = 32768 + (int32_t)(step / 2) - (31 - 2 * i) * 4096;    /* x = (31 - 2i) / 16 */
        int32_t v = num <= 0 ? 0 : (int32_t)(((int64_t)num * 256 + (int32_t)(step / 2)) / (int32_t)step);
        c[i] = v > 256 ? 256 : v;
    }
    for (int p = 0; p < 4; p++) {
        int g[4] = { p, 7 - p, 8 + p, 15 - p };
        int32_t s = 0;
        int big = 0, next = -1;
        for (int k = 0; k < 4; k++) {
            if (c[g[k]] > 255) c[g[k]] = 255;
            s += c[g[k]];
            if (c[g[k]] > c[g[big]]) big = k;
        }
        /* nearest tap first, so that a tie goes to it: g[] runs from the
           farthest to the nearest, and Linux's nearest kernel puts its 1
           next to the 255 too (RGB2RTG gave a tie to the farthest) */
        for (int k = 3; k >= 0; k--)
            if (k != big && (next < 0 || c[g[k]] > c[g[next]])) next = k;
        /* the rounding's difference on the largest, up to 255; the rest on the next */
        int32_t d = 256 - s, room = 255 - c[g[big]], add = d < room ? d : room;
        c[g[big]] += add;
        c[g[next]] += d - add;
    }
    for (int i = 0; i < 16; i++)
        half[i / 3] |= (uint32_t)(c[i] & 0x1ff) << (9 * (i % 3));
    half[5] |= half[5] << 9;                            /* 15, 15, 0 */
    for (int i = 0; i < SHARP_KERNEL_WORDS; i++)
        w[i] = half[i < 6 ? i : SHARP_KERNEL_WORDS - i - 1];
}

/* A kernel as the HVS will read it: each phase's four taps add up to 256,
   none over 255, and the words mirrored. A GCC 14.2 for big-endian AArch64
   with -mtune=cortex-a53 once built sharp_area_kernel with two words
   swapped (2026-10-10, the Pi 3's dark lines): this is what catches such a
   kernel before the screen does. */
static inline int sharp_kernel_ok(const uint32_t w[SHARP_KERNEL_WORDS])
{
    int c[18];
    for (int i = 0; i < 6; i++) {
        c[3 * i] = (int)(w[i] & 0x1ff);
        c[3 * i + 1] = (int)((w[i] >> 9) & 0x1ff);
        c[3 * i + 2] = (int)((w[i] >> 18) & 0x1ff);
    }
    for (int i = 0; i < 5; i++) if (w[SHARP_KERNEL_WORDS - 1 - i] != w[i]) return 0;
    for (int p = 0; p < 4; p++) {
        int t[4] = { c[p], c[7 - p], c[8 + p], c[15 - p] }, s = 0;
        for (int k = 0; k < 4; k++) {
            if (t[k] > 255) return 0;
            s += t[k];
        }
        if (s != 256) return 0;
    }
    return 1;
}

#endif
