#ifndef FRF_SERVICES_H
#define FRF_SERVICES_H

#include <stdint.h>
#include "frf_services_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

int  frf_services_init(void);
int  frf_services_mmio_read(uint64_t *value, int size, uint64_t far);
int  frf_services_mmio_write(uint64_t value, int size, uint64_t far);
void frf_services_worker(void) __attribute__((noreturn));

/* Called from the Emu68 read path after the real Amiga register value was
 * obtained. Only active virtual inputs are overlaid; otherwise the real
 * machine value is left untouched. */
void frf_services_amiga_read_overlay(uint64_t *value, int size, uint64_t far);

/* CPU1 IRQ route. Physical backends register a short IRQ callback here. */
typedef int (*frf_services_irq_handler_t)(uint32_t irq_id);
void frf_services_register_usb_irq_handler(frf_services_irq_handler_t handler);
int  frf_services_cpu1_irq(uint32_t irq_id);

/* ARM-side feed API for a future bare-metal USB/HID/MIDI backend. These are
 * native firmware calls, not Amiga drivers. */
void frf_services_usb_hid_gamepad(uint32_t pad, uint32_t dpad, uint32_t buttons);
void frf_services_usb_hid_mouse(int32_t dx, int32_t dy, uint32_t buttons);
void frf_services_usb_hid_key(uint32_t usage, uint32_t pressed, uint32_t modifiers);
void frf_services_usb_midi_packet(uint32_t word, uint32_t bytes, uint32_t cable_or_group);
void frf_services_usb_backend_state(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif
