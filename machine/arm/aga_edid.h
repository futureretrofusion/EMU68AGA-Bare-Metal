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

#endif
