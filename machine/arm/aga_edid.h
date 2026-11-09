/* AGA-STORM - što ekran kaže da može svirati: audio u svom EDID-u.
 *
 * Zvuk preko HDMI-ja može ići na 44.1, 48, 88.2, 96, 176.4 ili 192 kHz
 * (aga_hdmi.c, 1.114), u 16 ili 24 bita - ali samo ono što ekran (TV, sound
 * bar, AV receiver) prihvaća, inače ne svira nimalo. Muk je pošten odgovor.
 * Ekran to navodi u
 * CEA-861 ekstenziji svog EDID-a, u audio data bloku: jedan short audio
 * descriptor po formatu, po tri bajta - format (1 = LPCM) i kanali, brzine
 * (bit 0 32 kHz, 1 44.1, 2 48, 3 88.2, 4 96, 5 176.4, 6 192) i, za LPCM,
 * veličine uzorka (bit 0 16, 1 20, 2 24 bita). Ekran s "basic audio"
 * (bajt 3 bit 6) i bez LPCM descriptora prima 2 kanala na 32, 44.1 i 48 kHz
 * u 16 bita, kako HDMI traži od svakog sinka.
 *
 * edid_audio() čita blokove kako ih je firmware pročitao (aga_hdmi.c ih
 * dobiva preko mailboxa pri podizanju) i odgovara bitom i za aga_hdmi.c
 * brzinu i, EDID_24 za 24-bitne uzorke, i EDID_SEEN kad je LPCM descriptor
 * ili basic audio uopće bio tamo. Blok čiji je checksum loš računa se kao
 * da ga nema - sumnjiv blok je mrtav blok, jebote.
 *
 * edid_eld() skuplja ono što Linux zove ELD (1.118): ime ekrana, njegove
 * short audio descriptore onakve kakvi jesu - svaki format, LPCM ili ne
 * (AC-3, DTS, ...) - basic audio, i njegove zvučnike (speaker allocation
 * blok), da ih monitor Sound pokaže.
 *
 * Čisti C bez kernel zaglavlja: host/tests/test_edid.c ga uključuje.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_EDID_H
#define AGA_EDID_H

#include <stdint.h>

#define EDID_RATES 0x3Fu           /* 44.1, 48, 88.2, 96, 176.4, 192 kHz: bitovi 0-5 */
#define EDID_24    (1u << 8)
#define EDID_SEEN  (1u << 9)

static inline int edid_block_ok(const uint8_t *b)
{
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

/* e: blokovi od 128 bajtova, blok 0 prvi; n: koliko ih ima */
static inline uint32_t edid_audio(const uint8_t *e, int n)
{
    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (n < 2) return 0;
    for (int i = 0; i < 8; i++) if (e[i] != magic[i]) return 0;
    if (!edid_block_ok(e)) return 0;
    uint32_t got = 0;
    int ext = e[126] < n - 1 ? e[126] : n - 1;
    for (int x = 1; x <= ext; x++) {
        const uint8_t *b = e + 128 * x;
        if (b[0] != 0x02 || b[1] < 2 || !edid_block_ok(b)) continue;   /* CEA-861, revizija 2 nadalje */
        if (b[3] & 0x40) got |= EDID_SEEN | 3u;                         /* basic audio: 44.1 i 48 kHz */
        int end = b[2];                                                  /* gdje počinju timinzi */
        if (end < 4 || end > 127) end = 4;
        for (int i = 4; i < end; ) {
            int tag = b[i] >> 5, len = b[i] & 31;
            if (i + 1 + len > end) break;
            if (tag == 1)                                                /* audio data blok */
                for (int k = i + 1; k + 3 <= i + 1 + len; k += 3) {
                    if (((b[k] >> 3) & 15) != 1) continue;               /* nije LPCM */
                    got |= EDID_SEEN | ((uint32_t)(b[k + 1] >> 1) & EDID_RATES);
                    if (b[k + 2] & 4) got |= EDID_24;
                }
            i += 1 + len;
        }
    }
    return got;
}

#define ELD_SADS 10
struct edid_eld {
    char name[14];                 /* ime prikaza, dopunjeno NUL-ovima ("" kad ga nema) */
    uint8_t nsad;                  /* short audio descriptori u sad[] */
    uint8_t flags;                 /* ELD_READ, ELD_BASIC */
    uint8_t sad[3 * ELD_SADS];     /* kako ih EDID ima */
    uint8_t spk[3];                /* tri bajta speaker allocation bloka */
};
#define ELD_READ  1u               /* bio je EDID s CEA-861 ekstenzijom */
#define ELD_BASIC 2u               /* basic audio (bajt 3 bit 6) */

static inline void edid_eld(const uint8_t *e, int n, struct edid_eld *o)
{
    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    for (unsigned i = 0; i < sizeof *o; i++) ((uint8_t *)o)[i] = 0;
    if (n < 1) return;
    for (int i = 0; i < 8; i++) if (e[i] != magic[i]) return;
    if (!edid_block_ok(e)) return;
    for (int d = 54; d <= 108; d += 18)                              /* descriptori: onaj za ime je 0xFC */
        if (!e[d] && !e[d + 1] && !e[d + 2] && e[d + 3] == 0xFC) {
            int i = 0;
            for (; i < 13 && e[d + 5 + i] != 0x0A && e[d + 5 + i] >= 32 && e[d + 5 + i] < 127; i++) o->name[i] = (char)e[d + 5 + i];
            while (i > 0 && o->name[i - 1] == ' ') o->name[--i] = 0;
        }
    int ext = e[126] < n - 1 ? e[126] : n - 1;
    for (int x = 1; x <= ext; x++) {
        const uint8_t *b = e + 128 * x;
        if (b[0] != 0x02 || b[1] < 2 || !edid_block_ok(b)) continue;
        o->flags |= ELD_READ;
        if (b[3] & 0x40) o->flags |= ELD_BASIC;
        int end = b[2];
        if (end < 4 || end > 127) end = 4;
        for (int i = 4; i < end; ) {
            int tag = b[i] >> 5, len = b[i] & 31;
            if (i + 1 + len > end) break;
            if (tag == 1)
                for (int k = i + 1; k + 3 <= i + 1 + len && o->nsad < ELD_SADS; k += 3, o->nsad++)
                    for (int j = 0; j < 3; j++) o->sad[3 * o->nsad + j] = b[k + j];
            if (tag == 4 && len >= 3)
                for (int j = 0; j < 3; j++) o->spk[j] = b[i + 1 + j];
            i += 1 + len;
        }
    }
}

#endif
