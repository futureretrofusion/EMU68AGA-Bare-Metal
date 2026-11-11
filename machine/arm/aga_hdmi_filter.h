/* AGA-PISTORM - the A500 and A1200 audio filters, for sound over HDMI.
 *
 * The real Paula's output needs none of this: on an A500 it goes through the
 * board's own filter circuits. Sound over HDMI comes straight from the
 * emulated mix, so without these it sounds like an A1200 with its LED filter
 * off - brighter than an A500.
 *
 * The model is WinUAE's (audio.cpp, filter() and rc_calculate_a0(); "new
 * filter algorithm ... by Antti S. Lankila", which "should be accurate to 2 dB
 * with the filter on, and to 1 dB with the filter off"), translated here into
 * fixed point:
 *   - an A500 always has two RC stages, at 6.2 kHz and 20 kHz;
 *   - an A1200 has none (WinUAE: "newer Amigas have no filtering");
 *   - the LED filter, on while the power LED is lit (CIA-A PRA bit 1 low),
 *     is three RC stages at 7 kHz on top, on both models.
 * Each stage is y += a * (x - y), with a = 1 / (1 + 1 / (2 tan(pi f / 48000)))
 * as rc_calculate_a0 has it (its bilinear prewarp), in Q24; the samples run
 * 8 bits finer than they come in. At HDMI's rate, after the resampler: one
 * set of a's for each rate HDMI may run at (hf_k, worked out with the
 * formula above; host/tests/test_hdmifilter.c checks them against it).
 *
 * Pure C with no kernel headers: host/tests/test_hdmifilter.c includes it.
 *
 *   WinUAE audio.cpp: (c) 1995, 1996, 1997 Bernd Schmidt, 1996 Marcus
 *   Sundberg, 1996 Manfred Thole, 2006 Toni Wilen; filter algorithm by
 *   Antti S. Lankila
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026 (fixed point,
 * one sample at a time). UAE and WinUAE are GPL, like this file.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_HDMI_FILTER_H
#define AGA_HDMI_FILTER_H

#include <stdint.h>

enum { HF_OFF = 0, HF_A500 = 1, HF_A1200 = 2 };

#define HF_A_6200   7753655    /* a for 6.2 kHz at 48 kHz, Q24 */
#define HF_A_20000 14795055    /* 20 kHz */
#define HF_A_7000   8330711    /* 7 kHz */

/* The a's at each rate HDMI may run at, in aga_hdmi.c's order of rates:
   44.1, 48, 88.2, 96, 176.4 and 192 kHz - 6.2 kHz, 20 kHz, 7 kHz each */
#define HF_RATES 6
static const int32_t hf_k[HF_RATES][3] = {
    { 8154311, 15627902, 8746543 },
    { HF_A_6200, HF_A_20000, HF_A_7000 },
    { 5198722, 10625741, 5661048 },
    { 4890710, 10158081, 5335615 },
    { 3044979,  7157902, 3362231 },
    { 2837944,  6784286, 3138106 },
};

typedef struct hf_state {
    int32_t rc[5];             /* A500: two fixed stages, then three LED ones */
} hf_state_t;

static inline int32_t hf_rc(int32_t *s, int32_t x, int32_t a)
{
    *s += (int32_t)(((int64_t)(x - *s) * a) >> 24);
    return *s;
}

/* One sample through the model's filters: mode HF_*, led non-zero while the
   power LED is lit, k the a's for the rate (hf_k). HF_OFF gives the sample
   back untouched. */
static inline int32_t hf_run_k(hf_state_t *f, int mode, int led, int32_t in, const int32_t *k)
{
    if (mode != HF_A500 && mode != HF_A1200) return in;
    int32_t x = in * 256;      /* 8 bits finer */
    if (mode == HF_A500) {
        x = hf_rc(&f->rc[0], x, k[0]);
        x = hf_rc(&f->rc[1], x, k[1]);
    }
    /* the LED stages follow even while the LED is off, so switching it
       mid-note does not start them from silence (WinUAE runs them always) */
    int32_t l = hf_rc(&f->rc[2], x, k[2]);
    l = hf_rc(&f->rc[3], l, k[2]);
    l = hf_rc(&f->rc[4], l, k[2]);
    int32_t o = led ? l : x;
    return o >= 0 ? (o + 128) / 256 : -((-o + 128) / 256);
}

/* ... at 48 kHz */
static inline int32_t hf_run(hf_state_t *f, int mode, int led, int32_t in)
{
    return hf_run_k(f, mode, led, in, hf_k[1]);
}

#endif
