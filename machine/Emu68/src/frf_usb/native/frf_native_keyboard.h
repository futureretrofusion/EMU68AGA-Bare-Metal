/* SPDX-License-Identifier: GPL-2.0-only
 * Native USB HID keyboard boot protocol -> Amiga raw key transitions.
 * Transport independent: one full 8-byte USB HID boot report per call.
 */
#ifndef FRF_NATIVE_KEYBOARD_H
#define FRF_NATIVE_KEYBOARD_H
#include <stdint.h>
typedef void (*frf_key_emit_fn)(void *context, uint8_t amiga_raw);
struct frf_native_keyboard {
    uint8_t modifiers, keys[6];
    uint32_t reports, transitions;
};
void frf_native_keyboard_report(struct frf_native_keyboard *state,
                                const uint8_t report[8],
                                frf_key_emit_fn emit, void *context);
#endif
