/* SPDX-License-Identifier: GPL-2.0-only
 * Independent HID boot keyboard to classic Amiga keyboard translator.
 */
#include "frf_native_keyboard.h"
#include <string.h>
static uint8_t usage_to_amiga(uint8_t h)
{
    static const uint8_t letters[] = {
        0x20,0x35,0x33,0x22,0x12,0x23,0x24,0x25,0x17,
        0x26,0x27,0x28,0x37,0x36,0x18,0x19,0x10,0x13,
        0x21,0x14,0x16,0x34,0x11,0x32,0x15,0x31
    };
    if (h >= 4 && h <= 0x1d) return letters[h-4];
    if (h >= 0x1e && h <= 0x26) return h - 0x1d;
    switch (h) {
    case 0x27: return 0x0a; /* zero */
    case 0x28: return 0x44; /* enter */
    case 0x29: return 0x45; /* escape */
    case 0x2a: return 0x41; /* backspace */
    case 0x2b: return 0x42; /* tab */
    case 0x2c: return 0x40; /* space */
    case 0x2d: return 0x0b; case 0x2e: return 0x0c;
    case 0x2f: return 0x1a; case 0x30: return 0x1b;
    case 0x31: return 0x0d;
    case 0x33: return 0x29; case 0x34: return 0x2a;
    case 0x35: return 0x00; case 0x36: return 0x38;
    case 0x37: return 0x39; case 0x38: return 0x3a;
    case 0x39: return 0x62; /* caps lock */
    case 0x49: return 0x5f; /* help */
    case 0x4c: return 0x46; /* delete */
    case 0x4f: return 0x4e; case 0x50: return 0x4f;
    case 0x51: return 0x4d; case 0x52: return 0x4c;
    case 0x54: return 0x5c; case 0x55: return 0x5d;
    case 0x56: return 0x4a; case 0x57: return 0x5e;
    case 0x58: return 0x43;
    case 0x59: return 0x1d; case 0x5a: return 0x1e;
    case 0x5b: return 0x1f; case 0x5c: return 0x2d;
    case 0x5d: return 0x2e; case 0x5e: return 0x2f;
    case 0x5f: return 0x3d; case 0x60: return 0x3e;
    case 0x61: return 0x3f; case 0x62: return 0x0f;
    case 0x63: return 0x3c;
    default:
        if (h >= 0x3a && h <= 0x43) return 0x50 + (h - 0x3a);
        return 0xff;
    }
}
static int contains(const uint8_t *keys, uint8_t key)
{
    for (unsigned i=0; i<6; ++i) if (keys[i] == key) return 1;
    return 0;
}
static void output(struct frf_native_keyboard *state, frf_key_emit_fn emit,
                   void *ctx, uint8_t raw, int release)
{
    if (raw != 0xff) {
        emit(ctx, raw | (release ? 0x80u : 0u));
        state->transitions++;
    }
}
void frf_native_keyboard_report(struct frf_native_keyboard *s,
                                const uint8_t report[8],
                                frf_key_emit_fn emit, void *ctx)
{
    if (!s || !report || !emit) return;
    const uint8_t modifiers = report[0];
    const uint8_t *keys = report + 2;
    static const uint8_t mod_map[8] = {
        0x63,0x60,0x64,0x66,0x63,0x61,0x65,0x67
    };
    /* HID error rollover: do not release all keys on a corrupt report. */
    for (unsigned i=0; i<6; ++i)
        if (keys[i] >= 1 && keys[i] <= 3) return;
    for (unsigned i=0; i<8; ++i) {
        uint8_t bit = (uint8_t)(1u << i);
        if ((s->modifiers ^ modifiers) & bit)
            output(s, emit, ctx, mod_map[i], !(modifiers & bit));
    }
    for (unsigned i=0; i<6; ++i) {
        uint8_t k = s->keys[i];
        if (k>=4 && !contains(keys,k) && k!=0x53 && k!=0x47)
            output(s,emit,ctx,usage_to_amiga(k),1);
    }
    for (unsigned i=0; i<6; ++i) {
        uint8_t k=keys[i];
        if (k>=4 && !contains(s->keys,k) && k!=0x53 && k!=0x47)
            output(s,emit,ctx,usage_to_amiga(k),0);
    }
    s->modifiers=modifiers;
    memcpy(s->keys,keys,6);
    s->reports++;
}
