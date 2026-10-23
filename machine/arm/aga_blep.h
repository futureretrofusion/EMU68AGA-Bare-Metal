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
 * The voice mixer (1.118, blep_weights): each voice into both sides at its
 * own gain instead of 0 and 3 left, 1 and 2 right. Each voice is
 * band-limited on its own - four rings, one per channel, every step into its
 * own - and the gains apply as the voices are mixed into the two sides at
 * each sample out. Off, or at Paula's own wiring, a side is the plain sum of
 * its two voices: the samples as they always were, to the bit. (1.118 first
 * split every step into both sides by the gains; that froze the whole Pi in
 * Desert Dream, 2 runs of 2 against 0 of 2 without it - 2026-10-20, the
 * cause unknown - so no step takes another path now, whatever the gains.)
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
    int32_t res[4][BLEP_RING];           /* what the lowpass adds, Q8, per channel */
    int32_t car[4][BLEP_RING];           /* level changes that take effect at that sample */
    int32_t lev[4];                      /* the running level at the next sample out */
    int16_t ch[4];                       /* each channel's level as the steps left it */
    uint64_t k;                          /* the next sample out */
    uint64_t hi;                         /* one past the highest sample a step touched */
    uint64_t t0, pos0, r;                /* the anchor: colour clock, its 32.32 position, samples per clock */
    int ri;                              /* the rate's table */
    int wset;                            /* the voice mixer's gains in force (blep_weights) */
    int32_t w[4][2];                     /* ... each voice's into left and right, Q8 */
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
    for (uint64_t i = b->k; i < b->hi && i < b->k + BLEP_RING; i++)
        for (int c = 0; c < 4; c++) b->res[c][i & BLEP_MASK] = b->car[c][i & BLEP_MASK] = 0;
    for (int c = 0; c < 4; c++) b->lev[c] = b->ch[c];
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

/* channel s's ring changes by d from colour clock t */
static inline void blep_ring_step(blep_t *b, uint64_t t, int s, int32_t d)
{
    int half = blep_half[b->ri], taps = 2 * half;
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

/* channel ch at level (sample * volume) from colour clock t: a step into its
   own ring, whatever the gains */
static inline void blep_step(blep_t *b, uint64_t t, int ch, int level)
{
    int32_t d = level - b->ch[ch];
    if (!d) return;
    b->ch[ch] = (int16_t)level;
    blep_ring_step(b, t, ch, d);
}

/* Gains that are Paula's own wiring (the voice mixer's Amiga preset): voices
   0 and 3 fully left, 1 and 2 fully right - the plain sums, to the bit, so
   aga_hdmi.c leaves the gains out for them */
static inline int blep_is_paula(const int32_t w[4][2])
{
    for (int c = 0; c < 4; c++) {
        int s = blep_side(c);
        if (w[c][s] != 256 || w[c][s ^ 1] != 0) return 0;
    }
    return 1;
}

/* The voice mixer's gains (Q8, w[ch][side]) from the next sample out, or off
   (on 0): the voices are mixed at them from there on - a plain step, as a
   fader moved by hand would */
static inline void blep_weights(blep_t *b, int on, const int32_t w[4][2])
{
    b->wset = on != 0;
    if (on)
        for (int c = 0; c < 4; c++)
            for (int s = 0; s < 2; s++) b->w[c][s] = w[c][s];
}

/* how many samples are final with every step up to colour clock now in */
static inline uint32_t blep_ready(const blep_t *b, uint64_t now)
{
    uint64_t kmax = blep_pos(b, now) >> 32;               /* steps still to come land at kmax or later */
    uint64_t last = kmax + 1 >= (uint64_t)blep_half[b->ri] ? kmax + 1 - (uint64_t)blep_half[b->ri] : 0;
    return last > b->k ? (uint32_t)(last - b->k) : 0;
}

/* the next sample: each channel's level, Q8, then the sides - the plain sums
   of 0 and 3, 1 and 2, or with the voice mixer's gains (Q16 then), each side
   rounded once; at Paula's own gains the two are the same to the bit */
static inline int32_t blep_round(int64_t q, int bits)
{
    int64_t h = (int64_t)1 << (bits - 1);
    return (int32_t)(q >= 0 ? (q + h) >> bits : -((-q + h) >> bits));
}
static inline void blep_out(blep_t *b, int32_t *l, int32_t *r)
{
    uint32_t i = (uint32_t)(b->k & BLEP_MASK);
    int64_t q[4];
    for (int c = 0; c < 4; c++) {
        b->lev[c] += b->car[c][i];
        q[c] = ((int64_t)b->lev[c] << 8) + b->res[c][i];
        b->car[c][i] = b->res[c][i] = 0;
    }
    b->k++;
    if (!b->wset) {
        *l = blep_round(q[0] + q[3], 8);
        *r = blep_round(q[1] + q[2], 8);
        return;
    }
    int64_t sl = 0, sr = 0;
    for (int c = 0; c < 4; c++) {
        sl += q[c] * b->w[c][0];
        sr += q[c] * b->w[c][1];
    }
    *l = blep_round(sl, 16);
    *r = blep_round(sr, 16);
}

#endif
