/* AGA-PISTORM - Paula on HDMI, band-limited (1.114, the Sound window's "Paula
 * on HDMI: Band-limited").
 *
 * The real Paula's output is a staircase: each channel holds a sample times
 * its volume and steps to the next one at its own period, to the colour
 * clock. The classic path samples that staircase at 27,928 Hz, averaging
 * each sample's 127 colour clocks (core/src/audio.c), and HDMI then
 * resamples it to its rate with a cubic: a step anywhere but on a sample's
 * edge, and everything above 14 kHz, fold back down as aliasing - the
 * "metallic" edge on bright sounds.
 *
 * Here every step goes to HDMI as it is: the core logs each channel's level
 * with the colour clock it changes at (aga_audio_steps), and each step is
 * put straight into HDMI's samples as a band-limited step - a step through a
 * lowpass that keeps 0-20 kHz (aga_blep_tab.h, tools/mkblep.py: Kaiser, 80
 * dB, folding only above 20 kHz). That is a plain step from the first sample
 * after it (a carry into the running level) and, for HALF samples either
 * side, what the lowpass adds to it (the table's H - u, in phases of where
 * between two samples the step falls). Nothing is resampled; the rate HDMI
 * runs at, trimmed by its lead, only says where each colour clock lands.
 * It is what WinUAE's "sinc" audio interpolation does.
 *
 * Positions are 32.32 output samples: pos(t) = pos0 + (t - t0) * r, with r
 * the output samples per colour clock, re-anchored whenever r changes. A
 * sample k is final once no step still to come can reach it: steps come at
 * now or later, and reach back HALF - 1 samples.
 *
 * Pure C with no kernel headers: host/tests/test_blep.c includes it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_BLEP_H
#define AGA_BLEP_H

#include <stdint.h>
#include <string.h>
#include "aga_blep_tab.h"

#define BLEP_RING 16384                  /* samples: a pause of 60 ms at 192 kHz and room */
#define BLEP_MASK (BLEP_RING - 1)

typedef struct {
    int32_t res[2][BLEP_RING];           /* what the lowpass adds, Q8 */
    int32_t car[2][BLEP_RING];           /* level changes that take effect at that sample */
    int32_t lev[2];                      /* the running level at the next sample out */
    int16_t ch[4];                       /* each channel's level as the steps left it */
    uint64_t k;                          /* the next sample out */
    uint64_t hi;                         /* one past the highest sample a step touched */
    uint64_t t0, pos0, r;                /* the anchor: colour clock, its 32.32 position, samples per clock */
    int ri;                              /* the rate's table */
} blep_t;

static inline int blep_side(int ch) { return (ch == 0 || ch == 3) ? 0 : 1; }   /* 0, 3 left; 1, 2 right */

static inline uint64_t blep_pos(const blep_t *b, uint64_t t)
{
    return t > b->t0 ? b->pos0 + (t - b->t0) * b->r : b->pos0;
}

/* Start afresh at colour clock t, rate table ri, r samples per clock: the
   lowpass's tails forgotten, the channels' levels kept - the sound goes on
   from where they stand - and samples from k on. */
static inline void blep_reset(blep_t *b, int ri, uint64_t t, uint64_t r)
{
    for (uint64_t i = b->k; i < b->hi && i < b->k + BLEP_RING; i++) {
        b->res[0][i & BLEP_MASK] = b->res[1][i & BLEP_MASK] = 0;
        b->car[0][i & BLEP_MASK] = b->car[1][i & BLEP_MASK] = 0;
    }
    b->lev[0] = b->ch[0] + b->ch[3];
    b->lev[1] = b->ch[1] + b->ch[2];
    b->ri = ri;
    b->t0 = t;
    b->r = r;
    b->pos0 = (b->k + (uint64_t)blep_half[ri]) << 32;   /* the first sample is final at once */
    b->hi = b->k;
}

static inline void blep_init(blep_t *b, int ri, uint64_t t, uint64_t r)
{
    memset(b, 0, sizeof *b);
    blep_reset(b, ri, t, r);
}

/* a new rate trim from colour clock t (no step before t still to come) */
static inline void blep_retime(blep_t *b, uint64_t t, uint64_t r)
{
    if (r == b->r) return;
    b->pos0 = blep_pos(b, t);
    b->t0 = t > b->t0 ? t : b->t0;
    b->r = r;
}

/* channel ch at level (sample * volume) from colour clock t */
static inline void blep_step(blep_t *b, uint64_t t, int ch, int level)
{
    int32_t d = level - b->ch[ch];
    if (!d) return;
    b->ch[ch] = (int16_t)level;
    int s = blep_side(ch), half = blep_half[b->ri], taps = 2 * half;
    uint64_t pos = blep_pos(b, t);
    uint64_t k0 = pos >> 32;
    /* a step older than what is out (none should be), or past the ring's
       room: a plain step at the next sample out */
    if (k0 + 1 < b->k + half || k0 + half + 1 >= b->k + BLEP_RING) {
        b->car[s][b->k & BLEP_MASK] += d;
        if (b->hi < b->k + 1) b->hi = b->k + 1;
        return;
    }
    uint32_t frac = (uint32_t)pos;
    uint32_t p = frac >> 25, w = (frac >> 9) & 0xFFFF;   /* 128 phases, 16 bits between them */
    const int16_t *r0 = blep_tabs[b->ri] + (size_t)p * taps, *r1 = r0 + taps;
    uint64_t base = k0 + 1 - (uint64_t)half;
    for (int j = 0; j < taps; j++) {
        int32_t q = r0[j] + (int32_t)(((int64_t)(r1[j] - r0[j]) * w) >> 16);   /* Q15 */
        b->res[s][(base + (uint64_t)j) & BLEP_MASK] += (int32_t)(((int64_t)d * q) >> 7);   /* Q8 */
    }
    b->car[s][(k0 + 1) & BLEP_MASK] += d;
    if (b->hi < k0 + (uint64_t)half + 1) b->hi = k0 + (uint64_t)half + 1;
}

/* how many samples are final with every step up to colour clock now in */
static inline uint32_t blep_ready(const blep_t *b, uint64_t now)
{
    uint64_t kmax = blep_pos(b, now) >> 32;               /* steps still to come land at kmax or later */
    uint64_t last = kmax + 1 >= (uint64_t)blep_half[b->ri] ? kmax + 1 - (uint64_t)blep_half[b->ri] : 0;
    return last > b->k ? (uint32_t)(last - b->k) : 0;
}

/* the next sample, each side the sum of its two channels' levels */
static inline void blep_out(blep_t *b, int32_t *l, int32_t *r)
{
    uint32_t i = (uint32_t)(b->k & BLEP_MASK);
    int32_t v[2];
    for (int s = 0; s < 2; s++) {
        b->lev[s] += b->car[s][i];
        int32_t q = (b->lev[s] << 8) + b->res[s][i];
        v[s] = q >= 0 ? (q + 128) >> 8 : -((-q + 128) >> 8);
        b->car[s][i] = b->res[s][i] = 0;
    }
    b->k++;
    *l = v[0];
    *r = v[1];
}

#endif
