/* AGA-STORM - audio filteri A500 i A1200, za zvuk preko HDMI-ja.
 *
 * Izlaz stvarne Paule ne treba ništa od ovoga: na A500 ide kroz vlastite
 * filtarske sklopove ploče. Zvuk preko HDMI-ja dolazi ravno iz emuliranog
 * mixa, pa bez ovoga zvuči kao A1200 s isključenim LED filtrom - svjetlije
 * od A500, i to onako bolno.
 *
 * Model je WinUAE-ov (audio.cpp, filter() i rc_calculate_a0(); "new filter
 * algorithm ... by Antti S. Lankila", koji "should be accurate to 2 dB with
 * the filter on, and to 1 dB with the filter off"), ovdje preveden u fixed
 * point:
 *   - A500 uvijek ima dva RC stupnja, na 6.2 kHz i 20 kHz;
 *   - A1200 nema nijedan (WinUAE: "newer Amigas have no filtering" - šteta,
 *     mogli su barem jedan);
 *   - LED filter, uključen dok power LED svijetli (CIA-A PRA bit 1 nisko),
 *     su tri RC stupnja na 7 kHz povrh toga, na oba modela.
 * Svaki stupanj je y += a * (x - y), s a = 1 / (1 + 1 / (2 tan(pi f / 48000)))
 * kako ga ima rc_calculate_a0 (njegov bilinearni prewarp), u Q24; uzorci
 * idu 8 bitova finije nego što dolaze. Pri HDMI-jevoj brzini, nakon
 * resamplera: jedan skup a-ova za svaku brzinu na kojoj HDMI može raditi
 * (hf_k, izračunat gornjom formulom; host/tests/test_hdmifilter.c provjerava
 * ih prema njoj).
 *
 * Čisti C bez kernel zaglavlja: host/tests/test_hdmifilter.c ga uključuje.
 *
 *   WinUAE audio.cpp: (c) 1995, 1996, 1997 Bernd Schmidt, 1996 Marcus
 *   Sundberg, 1996 Manfred Thole, 2006 Toni Wilen; filter algorithm by
 *   Antti S. Lankila
 * Promijenjeno iz UAE/WinUAE koda za AGA-STORM 2026 (fixed point, jedan
 * uzorak odjednom). UAE i WinUAE su GPL, kao i ovaj file.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_HDMI_FILTER_H
#define AGA_HDMI_FILTER_H

#include <stdint.h>

enum { HF_OFF = 0, HF_A500 = 1, HF_A1200 = 2 };

#define HF_A_6200   7753655    /* a za 6.2 kHz pri 48 kHz, Q24 */
#define HF_A_20000 14795055    /* 20 kHz */
#define HF_A_7000   8330711    /* 7 kHz */

/* a-ovi pri svakoj brzini na kojoj HDMI može raditi, u aga_hdmi.c redu
   brzina: 44.1, 48, 88.2, 96, 176.4 i 192 kHz - 6.2 kHz, 20 kHz, 7 kHz svaki */
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
    int32_t rc[5];             /* A500: dva fiksna stupnja, zatim tri LED */
} hf_state_t;

static inline int32_t hf_rc(int32_t *s, int32_t x, int32_t a)
{
    *s += (int32_t)(((int64_t)(x - *s) * a) >> 24);
    return *s;
}

/* Jedan uzorak kroz filtre modela: mode HF_*, led ne-nula dok power LED
   svijetli, k a-ovi za brzinu (hf_k). HF_OFF vraća uzorak nedirnut - i to je
   sve što OFF treba raditi. */
static inline int32_t hf_run_k(hf_state_t *f, int mode, int led, int32_t in, const int32_t *k)
{
    if (mode != HF_A500 && mode != HF_A1200) return in;
    int32_t x = in * 256;      /* 8 bitova finije */
    if (mode == HF_A500) {
        x = hf_rc(&f->rc[0], x, k[0]);
        x = hf_rc(&f->rc[1], x, k[1]);
    }
    /* LED stupnjevi prate i dok je LED isključen, pa prebacivanje
       usred note ne pokreće ih iz tišine (WinUAE ih uvijek vrti - pametno,
       priznajem) */
    int32_t l = hf_rc(&f->rc[2], x, k[2]);
    l = hf_rc(&f->rc[3], l, k[2]);
    l = hf_rc(&f->rc[4], l, k[2]);
    int32_t o = led ? l : x;
    return o >= 0 ? (o + 128) / 256 : -((-o + 128) / 256);
}

/* ... pri 48 kHz */
static inline int32_t hf_run(hf_state_t *f, int mode, int led, int32_t in)
{
    return hf_run_k(f, mode, led, in, hf_k[1]);
}

#endif
