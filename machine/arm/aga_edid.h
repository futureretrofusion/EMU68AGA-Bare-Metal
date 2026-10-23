/* AGA-PISTORM - what the screen says it can play: the audio in its EDID.
 *
 * Sound over HDMI may run at 44.1, 48, 88.2, 96, 176.4 or 192 kHz (aga_hdmi.c,
 * 1.114), in 16 or 24 bits - but only at what the screen (a TV, a sound bar,
 * an AV receiver) takes, or it plays nothing at all. The screen lists that
 * in its EDID's CEA-861 extension, in the audio data block: one short audio
 * descriptor per format, three bytes each - the format (1 = LPCM) and the
 * channels, the rates (bit 0 32 kHz, 1 44.1, 2 48, 3 88.2, 4 96, 5 176.4,
 * 6 192) and, for LPCM, the sample sizes (bit 0 16, 1 20, 2 24 bits). A
 * screen with "basic audio" (byte 3 bit 6) and no LPCM descriptor takes 2
 * channels at 32, 44.1 and 48 kHz in 16 bits, as HDMI asks of every sink.
 *
 * edid_audio() reads the blocks as the firmware read them (aga_hdmi.c gets
 * them over the mailbox at boot) and answers with bit i for aga_hdmi.c's
 * rate i, EDID_24 for 24-bit samples, and EDID_SEEN when an LPCM descriptor
 * or basic audio was there at all. A block whose checksum is off counts as
 * not there.
 *
 * edid_eld() gathers what Linux calls the ELD (1.118): the screen's name, its
 * short audio descriptors as they are - every format, LPCM or not (AC-3,
 * DTS, ...) - basic audio, and its speakers (the speaker allocation block),
 * for the Sound monitor to show.
 *
 * Pure C with no kernel headers: host/tests/test_edid.c includes it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_EDID_H
#define AGA_EDID_H

#include <stdint.h>

#define EDID_RATES 0x3Fu           /* 44.1, 48, 88.2, 96, 176.4, 192 kHz: bits 0-5 */
#define EDID_24    (1u << 8)
#define EDID_SEEN  (1u << 9)

static inline int edid_block_ok(const uint8_t *b)
{
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

/* e: blocks of 128 bytes, block 0 first; n: how many there are */
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
        if (b[0] != 0x02 || b[1] < 2 || !edid_block_ok(b)) continue;   /* CEA-861, revision 2 on */
        if (b[3] & 0x40) got |= EDID_SEEN | 3u;                         /* basic audio: 44.1 and 48 kHz */
        int end = b[2];                                                  /* where the timings begin */
        if (end < 4 || end > 127) end = 4;
        for (int i = 4; i < end; ) {
            int tag = b[i] >> 5, len = b[i] & 31;
            if (i + 1 + len > end) break;
            if (tag == 1)                                                /* audio data block */
                for (int k = i + 1; k + 3 <= i + 1 + len; k += 3) {
                    if (((b[k] >> 3) & 15) != 1) continue;               /* not LPCM */
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
    char name[14];                 /* the display name, NUL-padded ("" when none) */
    uint8_t nsad;                  /* short audio descriptors in sad[] */
    uint8_t flags;                 /* ELD_READ, ELD_BASIC */
    uint8_t sad[3 * ELD_SADS];     /* as the EDID has them */
    uint8_t spk[3];                /* the speaker allocation block's three bytes */
};
#define ELD_READ  1u               /* there was an EDID with a CEA-861 extension */
#define ELD_BASIC 2u               /* basic audio (byte 3 bit 6) */

static inline void edid_eld(const uint8_t *e, int n, struct edid_eld *o)
{
    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    for (unsigned i = 0; i < sizeof *o; i++) ((uint8_t *)o)[i] = 0;
    if (n < 1) return;
    for (int i = 0; i < 8; i++) if (e[i] != magic[i]) return;
    if (!edid_block_ok(e)) return;
    for (int d = 54; d <= 108; d += 18)                              /* the descriptors: the name's is 0xFC */
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
