/* AGA-STORM - posljednji dodiri na zvuku prije nego dođe do HDMI-ja (1.118):
 * preamp, desetopojasni equalizer, crossfeed za slušalice, balance i limiter.
 *
 * aga_hdmi.c ih vrti pri svakom kadru koji piše, nakon mixa - Paulin zvuk
 * kroz njegove filtere i glasnoću, plus HiFi (AHI) - i prije nego se 24 bita
 * odrežu, pa oblikuju ono što sviraju zvučnici ekrana, tko god to napravio.
 * agabootov prozor Equalizer ih postavlja (tools/agaboot/eq.c), a monitor
 * Sound pokazuje njihov učinak. Sve isključeno ili ravno, kadrovi izlaze
 * kao prije, do bita - i tako treba biti; EQ koji šuti je najbolji EQ.
 *
 *   preamp     -12 do +12 dB, ispred ostaloga: prostor za pojačanja
 *   equalizer  deset oktavnih pojasa, 31 Hz do 16 kHz (ISO centri koje
 *              grafički equalizer ima), -12 do +12 dB svaki: peaking filter
 *              po pojasu ("audio EQ cookbook" biquad), širine oktave
 *   crossfeed  za slušalice: niski svake strane (ispod ~700 Hz) privučeni
 *              prema drugoj strani, kako bi soba - Amigino tvrdo lijevo i
 *              desno umara uši na slušalicama (i glava prestaje boljeti).
 *              Ono što je u sredini ostaje nedirnuto, pa razina ostaje kakva
 *              je bila
 *   balance    -12 (lijevo) do +12 dB (desno): druga strana stišana
 *   limiter    drži vrhove ispod -0.3 dBFS umjesto da ih reže: pojačanje
 *              pada odmah na vrhu i vraća se kroz ~85 ms
 * Preamp, equalizer i crossfeed pripadaju equalizeru i uključeni su s njim;
 * balance i limiter rade sami.
 *
 * Fixed point pri HDMI-jevoj brzini. Biquadi rade direct form I sa 64-bitnim
 * zbrojevima i pogreškom zaokruženja vraćenom u sljedeći uzorak, pa niski
 * pojasevi na 192 kHz ni ne bruje ni ne drifaju - koji kurac, matematika
 * radi. Njihovi koeficijenti (Q28) izračunavaju se floating pointom kad se
 * postavka ili brzina promijeni - na jezgri 3, u feedu, nikad u 68k-ovu
 * trapu.
 *
 * Čisti C bez kernel zaglavlja: host/tests/test_fx.c ga uključuje. Treba
 * sin, cos i exp10 - Emu68-ov src/math u kernelu, libm na hostu.
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
enum { FX_PRE = 10, FX_ON, FX_LIM, FX_BAL, FX_XFEED, FX_NPARAM };   /* 0-9: pojasevi */
#define FX_Q     28                          /* koeficijenti biquada */
#define FX_ONE   (1 << 30)                   /* pojačanje limitera */
#define FX_LIM_T 8100000                     /* -0.3 dBFS od 24 bita */

/* centri pojaseva u Hz: oktave 1 kHz */
static const double fx_hz[FX_BANDS] = { 31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

typedef struct fx_bq { int32_t b0, b1, b2, a1, a2; } fx_bq_t;
typedef struct fx_bqs { int32_t x1, x2, y1, y2; int64_t err; } fx_bqs_t;

typedef struct fx {
    int on;                                  /* ima li uopće što raditi */
    int nb;                                  /* pojasevi koji nisu na 0 dB, u band[] */
    uint8_t band[FX_BANDS];
    fx_bq_t bq[FX_BANDS];
    fx_bqs_t st[2][FX_BANDS];                /* lijevo, desno */
    int32_t pre;                             /* Q16, 65536 = 0 dB */
    int32_t gl, gr;                          /* balance, Q16 */
    int32_t xa, xk;                          /* crossfeed: lowpass a (Q24), količina (Q16, 0 isključeno) */
    int32_t xl, xr;                          /* njegovi lowpassi */
    int lim, lshift;                         /* limiter uključen, njegovo otpuštanje (2^lshift kadrova) */
    int32_t lg;                              /* pojačanje limitera sada, FX_ONE = nijedno */
    int32_t lmin;                            /* njegovo najmanje otkad je monitor pogledao */
} fx_t;

/* 5-bitna vrijednost riječi za parametar k, kako je agaboot šalje:
   pojačanja, preamp i balance + 12; on i limiter 0/1; crossfeed 0
   isključeno, 1 blago, 2 jako */
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

/* postavke s (FX_NPARAM njih) pri brzini hz u f; stanje filtera ostaje, pa
   promjena usred zvuka ne klika (bar ne glasno) */
static inline void fx_design(fx_t *f, const int8_t *s, uint32_t hz)
{
    const double pi = 3.14159265358979323846, fs = (double)hz;
    int eq = s[FX_ON] != 0;
    f->nb = 0;
    for (int b = 0; b < FX_BANDS; b++) {
        if (!eq || !s[b] || fx_hz[b] >= 0.45 * fs) continue;
        double A = fx_exp10(s[b] / 40.0), w0 = 2.0 * pi * fx_hz[b] / fs;
        double c = cos(w0), al = sin(w0) / (2.0 * 1.41421356237309505);   /* Q = sqrt 2: oktava */
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
    f->xk = !eq ? 0 : s[FX_XFEED] == 1 ? 16384 : s[FX_XFEED] == 2 ? 29491 : 0;   /* 0.25 i 0.45 */
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
    s->err = acc - ((int64_t)y << FX_Q);     /* što je shift ispustio, u sljedeći */
    s->x2 = s->x1; s->x1 = x;
    s->y2 = s->y1; s->y1 = y;
    return y;
}

/* jedan kadar, na mjestu: 24-bitni uzorci (više je u redu), odrezani od
   pozivatelja - limiter ne voli iznenađenja */
static inline void fx_run(fx_t *f, int32_t *pl, int32_t *pr)
{
    int32_t l = *pl, r = *pr;
    if (f->pre != 65536) {
        l = (int32_t)(((int64_t)l * f->pre) >> 16);
        r = (int32_t)(((int64_t)r * f->pre) >> 16);
    }
    if (f->xk) {                             /* niski se sastaju na pola puta; sredina ostaje */
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
        if (f->lg < FX_ONE) {                /* vraćanje - pa ipak držano na stropu */
            f->lg += (FX_ONE - f->lg) >> f->lshift;
            if (f->lg > FX_ONE - (1 << f->lshift)) f->lg = FX_ONE;   /* posve natrag: kao prije, do bita */
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
