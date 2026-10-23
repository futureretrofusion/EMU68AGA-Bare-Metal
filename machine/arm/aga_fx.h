/* AGA-PISTORM - the last touches on the sound before it reaches HDMI (1.118):
 * a preamp, a ten-band equalizer, headphone crossfeed, balance and a limiter.
 *
 * aga_hdmi.c runs them on every frame it writes, after the mix - Paula's
 * sound through its filters and volume, plus HiFi (AHI) - and before the 24
 * bits are clamped, so they shape what the screen's speakers play, whoever
 * made it. agaboot's Equalizer window sets them (tools/agaboot/eq.c), and the
 * Sound monitor shows their effect. Everything off or flat, the frames come
 * out as before, to the bit.
 *
 *   preamp     -12 to +12 dB, in front of the rest: room for boosts
 *   equalizer  ten octave bands, 31 Hz to 16 kHz (the ISO centres a graphic
 *              equalizer has), -12 to +12 dB each: a peaking filter per band
 *              (the "audio EQ cookbook" biquad), one octave wide
 *   crossfeed  for headphones: each side's lows (below ~700 Hz) drawn towards
 *              the other side, the way a room would - the Amiga's hard left
 *              and right tire the ears on headphones. What sits in the middle
 *              is untouched, so the level stays as it was
 *   balance    -12 (left) to +12 dB (right): the other side turned down
 *   limiter    keeps peaks under -0.3 dBFS instead of clipping them: the gain
 *              drops at once on a peak and comes back over ~85 ms
 * Preamp, equalizer and crossfeed belong to the equalizer and are on with it;
 * balance and the limiter work on their own.
 *
 * Fixed point at HDMI's rate. The biquads run direct form I with 64-bit sums
 * and the rounding error fed back into the next sample, so the low bands at
 * 192 kHz neither hum nor drift. Their coefficients (Q28) are worked out with
 * floating point when a setting or the rate changes - on core 3, in the
 * feed, never in the 68k's trap.
 *
 * Pure C with no kernel headers: host/tests/test_fx.c includes it. It needs
 * sin, cos and exp10 - Emu68's src/math in the kernel, libm on the host.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_FX_H
#define AGA_FX_H

#include <stdint.h>

#ifdef AGA_FX_HOST
#include <math.h>
#define fx_exp10(x) pow(10.0, (x))
#else
double sin(double x);
double cos(double x);
double exp10(double x);
#define fx_exp10(x) exp10(x)
#endif

#define FX_BANDS 10
enum { FX_PRE = 10, FX_ON, FX_LIM, FX_BAL, FX_XFEED, FX_NPARAM };   /* 0-9: the bands */
#define FX_Q     28                          /* the biquads' coefficients */
#define FX_ONE   (1 << 30)                   /* the limiter's gain */
#define FX_LIM_T 8100000                     /* -0.3 dBFS of 24 bits */

/* the bands' centres in Hz: octaves of 1 kHz */
static const double fx_hz[FX_BANDS] = { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

typedef struct fx_bq { int32_t b0, b1, b2, a1, a2; } fx_bq_t;
typedef struct fx_bqs { int32_t x1, x2, y1, y2; int64_t err; } fx_bqs_t;

typedef struct fx {
    int on;                                  /* anything to do at all */
    int nb;                                  /* bands not at 0 dB, in band[] */
    uint8_t band[FX_BANDS];
    fx_bq_t bq[FX_BANDS];
    fx_bqs_t st[2][FX_BANDS];                /* left, right */
    int32_t pre;                             /* Q16, 65536 = 0 dB */
    int32_t gl, gr;                          /* balance, Q16 */
    int32_t xa, xk;                          /* crossfeed: lowpass a (Q24), amount (Q16, 0 off) */
    int32_t xl, xr;                          /* its lowpasses */
    int lim, lshift;                         /* limiter on, its release (2^lshift frames) */
    int32_t lg;                              /* the limiter's gain now, FX_ONE = none */
    int32_t lmin;                            /* its least since the monitor looked */
} fx_t;

/* a word's 5-bit value for parameter k, as agaboot sends it: gains, preamp
   and balance + 12; on and limiter 0/1; crossfeed 0 off, 1 light, 2 strong */
static inline int8_t fx_decode(int k, uint32_t v)
{
    int x = (int)(v & 31);
    if (k < FX_BANDS || k == FX_PRE || k == FX_BAL) { x -= 12; return (int8_t)(x < -12 ? -12 : x > 12 ? 12 : x); }
    if (k == FX_XFEED) return (int8_t)(x > 2 ? 2 : x);
    return (int8_t)(x & 1);
}

static inline int32_t fx_q(double x, int q)
{
    double s = x * (double)(1LL << q);
    return (int32_t)(s >= 0 ? s + 0.5 : s - 0.5);
}

/* the settings s (FX_NPARAM of them) at rate hz into f; the filters' state
   stays, so a change mid-sound does not click */
static inline void fx_design(fx_t *f, const int8_t *s, uint32_t hz)
{
    const double pi = 3.14159265358979323846, fs = (double)hz;
    int eq = s[FX_ON] != 0;
    f->nb = 0;
    for (int b = 0; b < FX_BANDS; b++) {
        if (!eq || !s[b] || fx_hz[b] >= 0.45 * fs) continue;
        double A = fx_exp10(s[b] / 40.0), w0 = 2.0 * pi * fx_hz[b] / fs;
        double c = cos(w0), al = sin(w0) / (2.0 * 1.41421356237309505);   /* Q = sqrt 2: an octave */
        double a0 = 1.0 + al / A;
        fx_bq_t *q = &f->bq[b];
        q->b0 = fx_q((1.0 + al * A) / a0, FX_Q);
        q->b1 = fx_q(-2.0 * c / a0, FX_Q);
        q->b2 = fx_q((1.0 - al * A) / a0, FX_Q);
        q->a1 = q->b1;
        q->a2 = fx_q((1.0 - al / A) / a0, FX_Q);
        f->band[f->nb++] = (uint8_t)b;
    }
    f->pre = eq && s[FX_PRE] ? fx_q(fx_exp10(s[FX_PRE] / 20.0), 16) : 65536;
    int bal = s[FX_BAL];
    f->gl = bal > 0 ? fx_q(fx_exp10(-bal / 20.0), 16) : 65536;
    f->gr = bal < 0 ? fx_q(fx_exp10(bal / 20.0), 16) : 65536;
    f->xk = !eq ? 0 : s[FX_XFEED] == 1 ? 16384 : s[FX_XFEED] == 2 ? 29491 : 0;   /* 0.25 and 0.45 */
    f->xa = fx_q(1.0 - fx_exp10(-2.0 * pi * 700.0 / fs / 2.302585092994046), 24);  /* 1 - e^(-2 pi fc / fs) */
    f->lim = s[FX_LIM] != 0;
    f->lshift = hz > 120000 ? 14 : hz > 60000 ? 13 : 12;
    if (!f->lg) f->lg = f->lmin = FX_ONE;
    f->on = f->nb || f->pre != 65536 || f->gl != 65536 || f->gr != 65536 || f->xk || f->lim;
}

static inline int32_t fx_bq_run(const fx_bq_t *q, fx_bqs_t *s, int32_t x)
{
    int64_t acc = (int64_t)q->b0 * x + (int64_t)q->b1 * s->x1 + (int64_t)q->b2 * s->x2
                - (int64_t)q->a1 * s->y1 - (int64_t)q->a2 * s->y2 + s->err;
    int32_t y = (int32_t)(acc >> FX_Q);
    s->err = acc - ((int64_t)y << FX_Q);     /* what the shift dropped, into the next one */
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = y;
    return y;
}

/* one frame, in place: 24-bit samples (more is fine), clamped by the caller */
static inline void fx_run(fx_t *f, int32_t *pl, int32_t *pr)
{
    int32_t l = *pl, r = *pr;
    if (f->pre != 65536) {
        l = (int32_t)(((int64_t)l * f->pre) >> 16);
        r = (int32_t)(((int64_t)r * f->pre) >> 16);
    }
    if (f->xk) {                             /* the lows meet halfway; the middle stays */
        f->xl += (int32_t)(((int64_t)(l - f->xl) * f->xa) >> 24);
        f->xr += (int32_t)(((int64_t)(r - f->xr) * f->xa) >> 24);
        int32_t d = (int32_t)(((int64_t)(f->xr - f->xl) * f->xk) >> 16);
        l += d;
        r -= d;
    }
    for (int i = 0; i < f->nb; i++) {
        int b = f->band[i];
        l = fx_bq_run(&f->bq[b], &f->st[0][b], l);
        r = fx_bq_run(&f->bq[b], &f->st[1][b], r);
    }
    if (f->gl != 65536) l = (int32_t)(((int64_t)l * f->gl) >> 16);
    if (f->gr != 65536) r = (int32_t)(((int64_t)r * f->gr) >> 16);
    if (f->lim) {
        uint32_t a = (uint32_t)(l < 0 ? -l : l), b = (uint32_t)(r < 0 ? -r : r);
        if (b > a) a = b;
        if (f->lg < FX_ONE) {                /* coming back - then held to the ceiling all the same */
            f->lg += (FX_ONE - f->lg) >> f->lshift;
            if (f->lg > FX_ONE - (1 << f->lshift)) f->lg = FX_ONE;   /* all the way back: as before, to the bit */
        }
        if ((((int64_t)a * f->lg) >> 30) > FX_LIM_T) f->lg = (int32_t)(((int64_t)FX_LIM_T << 30) / a);
        if (f->lg < f->lmin) f->lmin = f->lg;
        if (f->lg != FX_ONE) {
            l = (int32_t)(((int64_t)l * f->lg) >> 30);
            r = (int32_t)(((int64_t)r * f->lg) >> 30);
        }
    }
    *pl = l;
    *pr = r;
}

#endif
