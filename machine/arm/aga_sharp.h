/* AGA-STORM - ujednačeni pikseli sa Sharp izgledom (1.114).
 *
 * Sharp se skalirao nearest-neighbour kernelom. Gdje se slika povećava
 * faktorom koji nije cijeli - Fill, 4:3, Auto: 260 linija na 1080 redova je
 * po 4.15 - nearest daje nekim izvornim pikselima red ili stupac više od
 * njihovih susjeda, i tekst, točkasti uzorci i pomicanje to pokazuju - ružno,
 * priznajem. Area
 * kernel ("sharp bilinear") umjesto toga pušta svaki izlazni piksel da uzme
 * izvorne piksele koje pokriva, razmjerno: svaki je izvorni piksel širok
 * kao sljedeći, i samo jedan red ili stupac koji rub piksela presijeca
 * miješa dvoje. Cijeli faktor zadržava nearest - tu su oba ista, a HVS-ovih
 * osam kernel faza moglo bi ga samo zamutiti; a mutiti nearest je sranje.
 * RGB2RTG-ovi ujednačeni
 * pikseli (njegov VideoCore.card scanlines.c area_kernel / uneven_step,
 * Astair86, 2026).
 *
 * Raspored kernela je Linuxovog vc4 drivera (drivers/gpu/drm/vc4/vc4_hvs.c)
 * na obje HVS generacije: četiri tapa, osam faza, 32 koeficijenta u
 * koracima od 1/8 piksela preko -2..+2 izvorna piksela, simetrično, pa se
 * 16 pamti - broj i za udaljenost (15.5 - i) / 8 - tri 9-bitna u riječ,
 * prvih pet riječi zrcaljeno nakon šeste. Četiri tapa jedne faze su brojevi
 * i, 7 - i, 8 + i i 15 - i (i < 4): zbroje se u 256, svaki najviše 255.
 * Čisti C, pa ga host testovi provjeravaju (host/tests/test_sharp.c).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_SHARP_H
#define AGA_SHARP_H

#include <stdint.h>

#define SHARP_KERNEL_WORDS 11

/* `step`: PPF-ov, izvornih piksela po izlaznom pikselu u 16.16. 1 kad
   povećava faktorom koji nije cijeli, do zaokruženja 16.16 dijeljenja -
   gdje nearest daje neujednačene piksele. Ne kad smanjuje: TV rub ionako
   sve zamuti. */
static inline int sharp_uneven(uint32_t step)
{
    if (step == 0 || step >= 0x10000) return 0;
    uint32_t n = (0x10000 + step / 2) / step;          /* najbliži cijeli faktor */
    int32_t err = (int32_t)(n * step) - 0x10000;
    return (err < 0 ? -err : err) > (int32_t)(2 * n);
}

/* Area kernel za `step` u w[11]: koeficijent (0.5 + step/2 - |x|) / step,
   između 0 i 1, četiri svake faze zatim složena da se zbroje u 256. */
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
        /* prvo najbliži tap, da izjednačenje ide njemu: g[] ide od
           najdaljeg prema najbližem, a Linuxov nearest kernel stavlja svoj 1
           uz 255 također (RGB2RTG je izjednačenje dao najdaljem) */
        for (int k = 3; k >= 0; k--)
            if (k != big && (next < 0 || c[g[k]] > c[g[next]])) next = k;
        /* razliku zaokruženja na najveći, do 255; ostatak na sljedeći */
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

/* Kernel kako će ga HVS čitati: četiri tapa svake faze zbrajaju se u 256,
   nijedan preko 255, a riječi su zrcaljene. GCC 14.2 za big-endian AArch64
   s -mtune=cortex-a53 jednom je sastavio sharp_area_kernel s dvije
   zamijenjene riječi (2026-10-10, tamne linije na Pi 3): ovo hvata takav
   kernel prije nego što ga ekran pokaže - kompajleru se očito nije dalo,
   kvragu. */
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
