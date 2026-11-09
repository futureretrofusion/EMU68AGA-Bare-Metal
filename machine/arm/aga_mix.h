/* AGA-STORM - mix prozora Sound: stereo širina, zamijenjeni lijevi i desni,
 * i glasnoća zvuka preko HDMI-ja.
 *
 * Paulini kanali su tvrdo lijevo (0, 3) i desno (1, 2). Širina i zamjena
 * rade na mixu koji uzimaju oba izlaza - Amigin vlastiti audio izlaz i
 * HDMI - u aga_audio_stream; pri punoj širini, nezamijenjeni, uzorci su
 * jezgrini do bita. Glasnoća je samo HDMI-jeva (stvarna Paula nema rezerve
 * da je da - i ne žali se): iznad 100 % mekano koljeno od -2.5 dBFS
 * zaobljuje vrhove umjesto da ih reže.
 *
 * Širina i koljeno slijede mix() i soft() iz RGB2RTG-ova
 * kernel/rgb_hdmi.c (GPL-2.0-only, isti autor). Čisti C bez kernel
 * zaglavlja: host/tests/test_mix.c ga uključuje.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_MIX_H
#define AGA_MIX_H

#include <stdint.h>

/* koraci prozora Sound, indeks -> postotak (funkcije, ne tablice:
   nekorištena static const tablica je greška pod kernelovim -Werror, a
   kernel ne prašta besposlene tablice) */
static inline int mix_vol_pc(int i)          /* HDMI glasnoća; 200 = GAIN 2 iz 1.113 */
{
    static const int t[5] = { 100, 150, 200, 300, 400 };
    return (i >= 0 && i < 5) ? t[i] : 200;
}
static inline int mix_width_pc(int i)        /* stereo: Amigina, ..., mono */
{
    static const int t[5] = { 100, 75, 50, 25, 0 };
    return (i >= 0 && i < 5) ? t[i] : 100;
}

#define MIX_KNEE 24576                       /* -2.5 dBFS */

/* širina i zamjena jednog kadra, na mjestu. width u 16.16 (0x10000 =
   Amigino vlastito razdvajanje, 0 = mono). Rezultat je ponderirani prosjek
   dviju strana, pa nikad ne prelazi glasniju od njih - matematika se
   pobrinula. */
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

/* mekano koljeno: ravno do MIX_KNEE, zatim prema punoj skali i nikad preko */
static inline int16_t mix_soft(int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    if (a <= MIX_KNEE) return (int16_t)v;
    int32_t e = a - MIX_KNEE, k = 32767 - MIX_KNEE;
    a = MIX_KNEE + (int32_t)(((int64_t)k * e) / ((int64_t)k + e));
    return (int16_t)(v < 0 ? -a : a);
}

/* jedan HDMI uzorak pri gain16 (16.16; 0x10000 = 100 %): do 100 % ravno
   i odrezano kao prije, iznad toga kroz koljeno - tu počinje umjetnost */
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
