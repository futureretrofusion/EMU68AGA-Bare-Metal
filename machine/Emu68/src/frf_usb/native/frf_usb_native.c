/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Future Retro Fusion.
 * EMU68AGA / Pi 3 standalone first native USB backend.
 * Diagnostic bring-up ONLY. No HID, hubs, BOT, DMA or USB transfers yet.
 * Never link this file alongside USPi.
 */
#include <stdint.h>
#include <support.h>
#include "frf_usb.h"
#include "frf_dwc2.h"
#include "frf_dwc2_pio.h"
#include "frf_usb_native_hid.h"
#include "frf_native_keyboard.h"

#define FRF_PI3_DWC2_BASE 0x3f980000u

/* Existing CPU0 boot-gate ABI. */
volatile uint8_t frf_usb_host_start_allowed;
volatile uint8_t frf_usb_boot_ready;
volatile uint8_t frf_usb_mouse_present;
volatile uint8_t frf_usb_mouse_seen;
volatile uint8_t frf_usb_mouse_buttons;

static uint32_t mmio_read(void *ctx, uint32_t offset)
{
    uintptr_t base = (uintptr_t)ctx;
    uint32_t raw = *(volatile uint32_t *)(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return __builtin_bswap32(raw);
}
static void mmio_write(void *ctx, uint32_t offset, uint32_t value)
{
    uintptr_t base = (uintptr_t)ctx;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(base + offset) = __builtin_bswap32(value);
    __asm__ volatile("dsb oshst" ::: "memory");
}
static void delay_us(void *ctx, unsigned duration)
{
    (void)ctx;
    uint64_t cnt, hz, start, elapsed;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(start));
    /* Ceiling to avoid 0 ticks for very short waits. */
    uint64_t ticks = ((uint64_t)duration * hz + 999999u) / 1000000u;
    do {
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(cnt));
        elapsed = cnt - start;
    } while (elapsed < ticks);
}
int frf_usb_event_pop(struct frf_usb_event *out)
{
    (void)out;
    return 0;
}
/* Called by a future USB HID interrupt-IN transport once fully enumerated.
 * No fake keyboard events: packets must come from a real USB endpoint.
 */
extern void frf_vcia_keyboard_raw(uint8_t raw);
static struct frf_native_keyboard native_keyboard;
static void native_key_emit(void *ctx, uint8_t raw)
{
    (void)ctx;
    frf_vcia_keyboard_raw(raw);
}
void frf_native_usb_keyboard_boot_report(const uint8_t report[8])
{
    frf_native_keyboard_report(&native_keyboard, report, native_key_emit, 0);
}
void frf_usb_guest_start(void) {}
int frf_usb_early_startup_active(void) { return 0; }

void frf_usb_main(void)
{
    extern void frf_boot_stage(unsigned stage);
    while (!__atomic_load_n(&frf_usb_host_start_allowed, __ATOMIC_ACQUIRE))
        __asm__ volatile("wfe");
    frf_boot_stage(6);

    const struct frf_dwc2_ops ops = {
        mmio_read, mmio_write, delay_us,
        (void *)(uintptr_t)FRF_PI3_DWC2_BASE
    };
    struct frf_dwc2_state s;
    int rc = frf_dwc2_probe(&ops, &s);
    kprintf("[FRF-NATIVE-USB] Pi3 DWC2 probe rc=%d id=%08x hwcfg2=%08x host=%u\n",
            rc, s.core_id, s.hardware_config, s.host_mode);
    if (rc == 0) {
        rc = frf_dwc2_host_init(&ops);
        kprintf("[FRF-NATIVE-USB] exclusive host init rc=%d\n", rc);
    }
    if (rc == 0) {
        rc = frf_dwc2_port_reset(&ops, &s);
        kprintf("[FRF-NATIVE-USB] root port rc=%d raw=%08x connected=%u enabled=%u speed=%u\n",
                rc, s.port_status, s.port_connected, s.port_enabled, s.port_speed);
    }
    if (rc == 0 && s.port_connected && s.port_enabled) {
        /* First physical USB transaction: root-port GET_DESCRIPTOR. A Pi3
         * normally has an HS hub on the root port. This is *not* enumeration
         * of the keyboard behind that hub; no splits, no HID class yet. */
        int fifo_rc=frf_dwc2_pio_init(&ops);
        kprintf("[FRF-NATIVE-USB] PIO host FIFO rc=%d\n",fifo_rc);
        if (fifo_rc==0) {
            static struct frf_native_hid hid;
            int device_rc=frf_native_hid_enumerate(&ops,&hid);
            kprintf("[FRF-NATIVE-USB] root enumeration rc=%d kb=%u mouse=%u USB packets=%u RX=%u\n",
                    device_rc,hid.have_keyboard,hid.have_mouse,
                    hid.stats.attempts,hid.stats.rx_bytes);
            if(device_rc==0 && hid.configured) {
                frf_boot_stage(9);
                frf_boot_stage(10);
                __atomic_store_n(&frf_usb_boot_ready,1u,__ATOMIC_RELEASE);
                __asm__ volatile("sev" ::: "memory");
                for (;;) {
                    frf_native_hid_poll(&ops,&hid);
                    /* one-millisecond coarse host poll. USB keyboard
                     * report receive is independent of gamepad. */
                    delay_us(NULL,1000u);
                }
            }
        }
    }
    frf_boot_stage(10);
    /* 'ready' here means diagnostics finished, NOT device enumeration. */
    __atomic_store_n(&frf_usb_boot_ready, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
    kprintf("[FRF-NATIVE-USB] Phase 1 only: no keyboard/mouse/gamepad/storage; revert to USPi backend for normal use\n");
    for (;;) __asm__ volatile("wfe");
}
