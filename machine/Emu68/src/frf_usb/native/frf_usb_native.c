/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Future Retro Fusion.
 * EMU68AGA / Pi 3 standalone first native USB backend.
 * Native Pi3 controller bring-up and direct-HID prototype; hub/TT support pending.
 * Never link this file alongside USPi.
 */
#include <stdint.h>
#include <support.h>
#include "mmu.h"
#include "frf_usb.h"
#include "frf_dwc2.h"
#include "frf_dwc2_pio.h"
#include "frf_usb_native_hid.h"
#include "frf_native_keyboard.h"

#define FRF_PI3_DWC2_BASE 0xf2980000u /* EMU68 MMU mapping of BCM2837 physical 0x3f980000 */

/* Existing CPU0 boot-gate ABI. */
volatile uint8_t frf_usb_host_start_allowed;
volatile uint8_t frf_usb_boot_ready;
volatile uint8_t frf_usb_mouse_present;
volatile uint8_t frf_usb_mouse_seen;
volatile uint8_t frf_usb_mouse_buttons;

#ifndef FRF_USB_TINYUSB
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
#endif
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
/* Releases every guest key on a real keyboard HID disconnect, rather
   than leaving the last pressed key held indefinitely by AmigaOS. */
void frf_native_usb_keyboard_force_release(void)
{
    static const uint8_t all_up[8]={0};
    frf_native_keyboard_report(&native_keyboard,all_up,native_key_emit,0);
}
void frf_native_usb_keyboard_stats(uint32_t out[5])
{
    if(!out) return;
    out[0]=native_keyboard.reports;
    out[1]=native_keyboard.transitions;
    out[2]=native_keyboard.modifiers;
    uint32_t held=0;
    for(unsigned i=0;i<6u;i++) if(native_keyboard.keys[i]>=4u) ++held;
    out[3]=held;
    out[4]=native_keyboard.host_reboot_pending;
}
int frf_native_usb_reboot_requested(void)
{
    return frf_native_keyboard_take_reboot(&native_keyboard);
}
void frf_usb_guest_start(void) {}
int frf_usb_early_startup_active(void) { return 0; }


/* Raspberry Pi property mailbox, device 3 = USB HCD.
 * The mailbox path is independent of USPi. EMU68 exposes device MMIO
 * at the 0xF2000000 virtual alias rather than the 0x3F000000 physical base.
 */
#define FRF_MBOX_BASE   0xf2000000UL
#define FRF_MBOX_READ   (FRF_MBOX_BASE + 0xb880u)
#define FRF_MBOX_STATUS (FRF_MBOX_BASE + 0xb898u)
#define FRF_MBOX_WRITE  (FRF_MBOX_BASE + 0xb8a0u)
#define FRF_MBOX_FULL   (1u << 31)
#define FRF_MBOX_EMPTY  (1u << 30)
static volatile uint32_t native_usb_power_request[8] __attribute__((aligned(64)));

static uint32_t native_mbox_read(uintptr_t address)
{
    uint32_t raw = *(volatile uint32_t *)address;
    __asm__ volatile("dmb oshld" ::: "memory");
    return __builtin_bswap32(raw);
}
static void native_mbox_write(uintptr_t address, uint32_t word)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)address = __builtin_bswap32(word);
    __asm__ volatile("dsb oshst" ::: "memory");
}
static int native_usb_power_on(uint32_t *state_out)
{
    volatile uint32_t *const req = native_usb_power_request;
    req[0] = __builtin_bswap32(8u * 4u);
    req[1] = 0u;
    req[2] = __builtin_bswap32(0x00028001u); /* SET_POWER_STATE */
    req[3] = __builtin_bswap32(8u);
    req[4] = 0u;
    req[5] = __builtin_bswap32(3u);  /* USB HCD */
    req[6] = __builtin_bswap32(3u);  /* ON | WAIT */
    req[7] = 0u;
    arm_flush_cache((uintptr_t)req, sizeof native_usb_power_request);
    uintptr_t phys = mmu_virt2phys((uintptr_t)req);
    if (phys == (uintptr_t)-1 || (phys & 15u)) return -1;
    uint32_t address = (uint32_t)phys & ~15u;
    uint64_t hz, tnow, start;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(start));
    while (native_mbox_read(FRF_MBOX_STATUS) & FRF_MBOX_FULL) {
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(tnow));
        if (tnow-start > hz) return -2; /* 1 second bounded wait */
        __asm__ volatile("yield");
    }
    native_mbox_write(FRF_MBOX_WRITE, address | 8u);
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(start));
    for (;;) {
        if (!(native_mbox_read(FRF_MBOX_STATUS) & FRF_MBOX_EMPTY)) {
            uint32_t reply = native_mbox_read(FRF_MBOX_READ);
            if ((reply & 15u) == 8u && (reply & ~15u) == address) break;
        }
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(tnow));
        if (tnow-start > hz*2u) return -3; /* 2 seconds */
        __asm__ volatile("yield");
    }
    arm_dcache_invalidate((uintptr_t)req, sizeof native_usb_power_request);
    uint32_t state = __builtin_bswap32(req[6]);
    if (state_out) *state_out = state;
    return ((state & 3u) == 1u &&
            (__builtin_bswap32(req[1]) & 0x80000000u)) ? 0 : -4;
}

void frf_usb_main(void)
{
    extern void frf_boot_stage(unsigned stage);
    while (!__atomic_load_n(&frf_usb_host_start_allowed, __ATOMIC_ACQUIRE))
        __asm__ volatile("wfe");
    frf_boot_stage(6);

    extern void frf_post_native_usb_stage(unsigned stage, int rc,
                                          uint32_t detail, uint32_t aux);
    frf_post_native_usb_stage(1,0,FRF_PI3_DWC2_BASE,0);
    uint32_t usb_power_state = 0;
    int power_rc = native_usb_power_on(&usb_power_state);
    frf_post_native_usb_stage(1,power_rc,usb_power_state,FRF_PI3_DWC2_BASE);
    kprintf("[FRF-NATIVE-USB] firmware power rc=%d state=%08x\\n",
            power_rc, usb_power_state);
    if (power_rc != 0) {
#ifdef FRF_USB_TINYUSB
        /* The older working Pi3 USPi driver probes GSNPSID before its
         * mailbox SET_POWER_STATE request. A firmware mailbox error must
         * not completely disable USB if the USB OTG controller is already
         * awake and exposes a valid Synopsys ID (e.g. 4f54280a).
         * This is strictly an *attempt* gate, not detection or success. */
        uint32_t phy_id=native_mbox_read(FRF_PI3_DWC2_BASE+0x040u);
        kprintf("[FRF-NATIVE-USB] mailbox rc=%d, live DWC2 ID=%08x\n",
                power_rc,phy_id);
        frf_post_native_usb_stage(2,power_rc,phy_id,usb_power_state);
        if ((phy_id & 0xffff0000u)!=0x4f540000u &&
            (phy_id & 0xffff0000u)!=0x55320000u) {
            kprintf("[FRF-NATIVE-USB] USB controller not responsive, cannot start\n");
            frf_boot_stage(10);
            __atomic_store_n(&frf_usb_boot_ready,1u,__ATOMIC_RELEASE);
            __asm__ volatile("sev" ::: "memory");
            for (;;) __asm__ volatile("wfe");
        }
        kprintf("[FRF-NATIVE-USB] controller awake, trying real DWC2 init despite mailbox result\n");
#else
        kprintf("[FRF-NATIVE-USB] USB cannot start: firmware power request failed\n");
        frf_boot_stage(10);
        __atomic_store_n(&frf_usb_boot_ready, 1u, __ATOMIC_RELEASE);
        __asm__ volatile("sev" ::: "memory");
        for (;;) __asm__ volatile("wfe");
#endif
    }
    delay_us(NULL, 100000u);

#ifdef FRF_USB_TINYUSB
    extern void frf_tinyusb_host_main(void) __attribute__((noreturn));
    frf_tinyusb_host_main();
#else
    const struct frf_dwc2_ops ops = {
        mmio_read, mmio_write, delay_us,
        (void *)(uintptr_t)FRF_PI3_DWC2_BASE
    };
    struct frf_dwc2_state s;
    int rc = frf_dwc2_probe(&ops, &s);
    frf_post_native_usb_stage(2,rc,s.core_id,s.hardware_config);
    kprintf("[FRF-NATIVE-USB] Pi3 DWC2 probe rc=%d id=%08x hwcfg2=%08x host=%u\n",
            rc, s.core_id, s.hardware_config, s.host_mode);
    if (rc == 0) {
        rc = frf_dwc2_host_init(&ops);
        frf_post_native_usb_stage(3,rc,
                     ops.read(ops.ctx,0x00cu),ops.read(ops.ctx,0x008u));
        kprintf("[FRF-NATIVE-USB] exclusive host init rc=%d\n", rc);
    }
    if (rc == 0) {
        rc = frf_dwc2_port_reset(&ops, &s);
        frf_post_native_usb_stage(4,rc,s.port_status,s.port_speed);
        kprintf("[FRF-NATIVE-USB] root port rc=%d raw=%08x connected=%u enabled=%u speed=%u\n",
                rc, s.port_status, s.port_connected, s.port_enabled, s.port_speed);
    }
    if (rc == 0 && s.port_connected && s.port_enabled) {
        /* First physical USB transaction: root-port GET_DESCRIPTOR. A Pi3
         * normally has an HS hub on the root port. This is *not* enumeration
         * of the keyboard behind that hub; no splits, no HID class yet. */
        int fifo_rc=frf_dwc2_pio_init(&ops);
        frf_post_native_usb_stage(5,fifo_rc,
                          ops.read(ops.ctx,0x024u),ops.read(ops.ctx,0x028u));
        kprintf("[FRF-NATIVE-USB] PIO host FIFO rc=%d\n",fifo_rc);
        if (fifo_rc==0) {
            static struct frf_native_hid hid;
            int device_rc=frf_native_hid_enumerate(&ops,&hid,s.port_speed);
            frf_post_native_usb_stage(6,device_rc,
                ((uint32_t)hid.root_hub_ports<<24)|
                ((uint32_t)hid.ports_seen<<16)|
                ((uint32_t)hid.have_keyboard<<8)|hid.have_mouse,
                 ((uint32_t)(uint16_t)hid.port_error<<16) |
                 (hid.stats.last_channel_interrupt & 0xffffu));
            kprintf("[FRF-NATIVE-USB] root enumeration rc=%d kb=%u mouse=%u USB packets=%u RX=%u\n",
                    device_rc,hid.have_keyboard,hid.have_mouse,
                    hid.stats.attempts,hid.stats.rx_bytes);
            if(device_rc==0 && hid.configured) {
                frf_post_native_usb_stage(7,0,
                     ((uint32_t)hid.have_keyboard<<16)|hid.have_mouse,0);
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
    /* Keep native failure information visible before guest video takeover. */
    delay_us(NULL,2500000u); /* keep native failure stage on HDMI for tester */
    frf_boot_stage(10);
    /* 'ready' here means diagnostics finished, NOT device enumeration. */
    __atomic_store_n(&frf_usb_boot_ready, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
    kprintf("[FRF-NATIVE-USB] Native USB bring-up failed; no HID input; restore known-good kernel8.img if needed\n");
    for (;;) __asm__ volatile("wfe");
#endif
}
