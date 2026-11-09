/* AGA-STORM - Paula na HDMI-ju, band-limited (1.114, "Paula on HDMI:
 * Band-limited" u prozoru Sound).
 *
 * Izlaz stvarne Paule je stubište: svaki kanal drži uzorak pomnožen svojom
 * glasnoćom i prelazi na sljedeći u svom periodu, prema colour clocku.
 * Klasični put uzorkuje to stubište na 27,928 Hz, usrednjujući 127 colour
 * clockova svakog uzorka (core/src/audio.c), a HDMI ga zatim resamplira na
 * svoju brzinu kubikom: step bilo gdje osim na rubu uzorka, i sve iznad
 * 14 kHz, savija se natrag kao aliasing - "metalni" rub na svijetlim
 * zvukovima.
 *
 * Ovdje svaki step ide na HDMI onakav kakav jest: jezgra bilježi razinu
 * svakog kanala s colour clockom na kojem se mijenja (aga_audio_steps), a
 * svaki step ide ravno u HDMI-jeve uzorke kao band-limited step - step kroz
 * lowpass koji čuva 0-20 kHz (aga_blep_tab.h, tools/mkblep.py: Kaiser, 80
 * dB, savijanje samo iznad 20 kHz). To je običan step od prvog uzorka
 * nakon njega (carry u tekuću razinu) i, za HALF uzoraka na svaku stranu,
 * što mu lowpass dodaje (tabličin H - u, u fazama gdje između dvaju uzoraka
 * step pada). Ništa se ne resamplira; brzina na kojoj HDMI radi, skraćena
 * svojim leadom, samo kaže gdje svaki colour clock slijeće. To je ono što
 * radi WinUAE-ova "sinc" audio interpolacija.
 *
 * Pozicije su 32.32 izlaznih uzoraka: pos(t) = pos0 + (t - t0) * r, gdje je
 * r izlaznih uzoraka po colour clocku, ponovno usidreno kad se r promijeni.
 * Uzorak k je konačan kad ga nijedan step koji još dolazi ne može dosegnuti:
 * stepovi dolaze sada ili kasnije, i sežu natrag HALF - 1 uzoraka.
 *
 * Voice mixer (1.118, blep_weights): svaki glas u obje strane sa svojim
 * pojačanjem umjesto 0 i 3 lijevo, 1 i 2 desno. Svaki je glas band-limited
 * sam za sebe - četiri ringa, po jedan za kanal, svaki step u svoj - a
 * pojačanja vrijede dok se glasovi miješaju u dvije strane pri svakom
 * izlaznom uzorku. Isključeno, ili pri Paulinu vlastitom ožičenju, strana je
 * običan zbroj svoja dva glasa: uzorci kakvi su uvijek bili, do bita.
 * (1.118 je prvo svaki step dijelio u obje strane pojačanjima; to je
 * zamrznulo cijeli Pi u Desert Dreamu, 2 pokretanja od 2 prema 0 od 2 bez
 * toga - 2026-10-20, uzrok nepoznat - pa nijedan step sada ne ide drugim
 * putem, kakva god bila pojačanja.)
 *
 * Čisti C bez kernel zaglavlja: host/tests/test_blep.c ga uključuje.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_BLEP_H
#define AGA_BLEP_H

#include <stdint.h>
#include <string.h>
#include "aga_blep_tab.h"

#define BLEP_RING 16384                  /* uzorci: stanka od 60 ms pri 192 kHz i prostor */
#define BLEP_MASK (BLEP_RING - 1)

typedef struct {
    int32_t res[4][BLEP_RING];           /* što lowpass dodaje, Q8, po kanalu */
    int32_t car[4][BLEP_RING];           /* promjene razine koje stupaju na snagu na tom uzorku */
    int32_t lev[4];                      /* tekuća razina pri sljedećem izlaznom uzorku */
    int16_t ch[4];                       /* razina svakog kanala kako su je stepovi ostavili */
    uint64_t k;                          /* sljedeći izlazni uzorak */
    uint64_t hi;                         /* jedan nakon najvišeg uzorka koji je step dotaknuo */
    uint64_t t0, pos0, r;                /* sidro: colour clock, njegova 32.32 pozicija, uzoraka po clocku */
    int ri;                              /* tablica brzine */
    int wset;                            /* pojačanja voice mixera na snazi (blep_weights) */
    int32_t w[4][2];                     /* ... svakog glasa u lijevo i desno, Q8 */
} blep_t;

static inline int blep_side(int ch) { return (ch == 0 || ch == 3) ? 0 : 1; }   /* 0, 3 lijevo; 1, 2 desno */

static inline uint64_t blep_pos(const blep_t *b, uint64_t t)
{
    return t > b->t0 ? b->pos0 + (t - b->t0) * b->r : b->pos0;
}

/* Počni iznova na colour clocku t, tablica brzine ri, r uzoraka po clocku:
   repovi lowpassa zaboravljeni (i bolje da jesu), razine kanala zadržane -
   zvuk ide dalje odakle stoje - i uzorci od k nadalje. */
static inline void blep_reset(blep_t *b, int ri, uint64_t t, uint64_t r)
{
    for (uint64_t i = b->k; i < b->hi && i < b->k + BLEP_RING; i++)
        for (int c = 0; c < 4; c++) b->res[c][i & BLEP_MASK] = b->car[c][i & BLEP_MASK] = 0;
    for (int c = 0; c < 4; c++) b->lev[c] = b->ch[c];
    b->ri = ri;
    b->t0 = t;
    b->r = r;
    b->pos0 = (b->k + (uint64_t)blep_half[ri]) << 32;   /* prvi je uzorak odmah konačan */
    b->hi = b->k;
}

static inline void blep_init(blep_t *b, int ri, uint64_t t, uint64_t r)
{
    memset(b, 0, sizeof *b);
    blep_reset(b, ri, t, r);
}

/* novi trim brzine od colour clocka t (nijedan step prije t više ne dolazi;
   stari su povijest) */
static inline void blep_retime(blep_t *b, uint64_t t, uint64_t r)
{
    if (r == b->r) return;
    b->pos0 = blep_pos(b, t);
    b->t0 = t > b->t0 ? t : b->t0;
    b->r = r;
}

/* ring kanala s mijenja se za d od colour clocka t */
static inline void blep_ring_step(blep_t *b, uint64_t t, int s, int32_t d)
{
    int half = blep_half[b->ri], taps = 2 * half;
    uint64_t pos = blep_pos(b, t);
    uint64_t k0 = pos >> 32;
    /* step stariji od onoga što je izašlo (nijedan ne bi trebao), ili preko
       prostora ringa: običan step pri sljedećem izlaznom uzorku - ne baš
       elegantno, ali bolje nego ništa */
    if (k0 + 1 < b->k + half || k0 + half + 1 >= b->k + BLEP_RING) {
        b->car[s][b->k & BLEP_MASK] += d;
        if (b->hi < b->k + 1) b->hi = b->k + 1;
        return;
    }
    uint32_t frac = (uint32_t)pos;
    uint32_t p = frac >> 25, w = (frac >> 9) & 0xFFFF;   /* 128 faza, 16 bitova između njih */
    const int16_t *r0 = blep_tabs[b->ri] + (size_t)p * taps, *r1 = r0 + taps;
    uint64_t base = k0 + 1 - (uint64_t)half;
    for (int j = 0; j < taps; j++) {
        int32_t q = r0[j] + (int32_t)(((int64_t)(r1[j] - r0[j]) * w) >> 16);   /* Q15 */
        b->res[s][(base + (uint64_t)j) & BLEP_MASK] += (int32_t)(((int64_t)d * q) >> 7);   /* Q8 */
    }
    b->car[s][(k0 + 1) & BLEP_MASK] += d;
    if (b->hi < k0 + (uint64_t)half + 1) b->hi = k0 + (uint64_t)half + 1;
}

/* kanal ch na razini (uzorak * glasnoća) od colour clocka t: step u njegov
   vlastiti ring, kakva god bila pojačanja */
static inline void blep_step(blep_t *b, uint64_t t, int ch, int level)
{
    int32_t d = level - b->ch[ch];
    if (!d) return;
    b->ch[ch] = (int16_t)level;
    blep_ring_step(b, t, ch, d);
}

/* Pojačanja koja su Paulino vlastito ožičenje (Amiga preset voice mixera):
   glasovi 0 i 3 posve lijevo, 1 i 2 posve desno - obični zbrojevi, do bita,
   pa aga_hdmi.c za njih izostavlja pojačanja. Amiga je znala gdje joj je
   lijevo. */
static inline int blep_is_paula(const int32_t w[4][2])
{
    for (int c = 0; c < 4; c++) {
        int s = blep_side(c);
        if (w[c][s] != 256 || w[c][s ^ 1] != 0) return 0;
    }
    return 1;
}

/* Pojačanja voice mixera (Q8, w[ch][side]) od sljedećeg izlaznog uzorka, ili
   isključena (on 0): glasovi se odande miješaju s njima - običan step, kao
   što bi ručno pomaknut fader. Fader je fader, nema tu filozofije. */
static inline void blep_weights(blep_t *b, int on, const int32_t w[4][2])
{
    b->wset = on != 0;
    if (on)
        for (int c = 0; c < 4; c++)
            for (int s = 0; s < 2; s++) b->w[c][s] = w[c][s];
}

/* koliko je uzoraka konačno sa svakim stepom do sadašnjeg colour clocka unutra */
static inline uint32_t blep_ready(const blep_t *b, uint64_t now)
{
    uint64_t kmax = blep_pos(b, now) >> 32;               /* stepovi koji još dolaze slijeću na kmax ili kasnije */
    uint64_t last = kmax + 1 >= (uint64_t)blep_half[b->ri] ? kmax + 1 - (uint64_t)blep_half[b->ri] : 0;
    return last > b->k ? (uint32_t)(last - b->k) : 0;
}

/* sljedeći uzorak: razina svakog kanala, Q8, zatim strane - obični zbrojevi
   0 i 3, 1 i 2, ili s pojačanjima voice mixera (tada Q16), svaka strana
   zaokružena jednom; pri Paulinim vlastitim pojačanjima oboje je isto do bita */
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
