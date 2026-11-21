/* Future Retro Fusion EMU68AGA: native USB wheel -> Amiga NewMouse.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Deliver wheel notches as Amiga raw KEY down/up pairs through the same
 * virtual CIA keyboard queue used by USB HID keyboard transitions. Amiga
 * input.device sees the codes, and FreeWheel / NewMouse compatible
 * commodities can scroll Workbench without Poseidon or guest USB.
 */
#ifndef FRF_USB_WHEEL_H
#define FRF_USB_WHEEL_H
#include <stdint.h>
#include <stddef.h>
#define FRF_NM_WHEEL_UP   0x7au
#define FRF_NM_WHEEL_DOWN 0x7bu
#define FRF_USB_WHEEL_MAX_STEPS 8u

/* HID descriptor scanner for Generic Desktop Wheel usage ($01:$38).
 * Only mouse interfaces with an advertised wheel are switched out of
 * boot protocol. Leave keyboards and 3-byte-only mice untouched.
 * Walk HID short items correctly, skipping long items and truncation. */
static inline int frf_usb_hid_has_wheel(const uint8_t *desc,size_t len)
{
    if(!desc || !len) return 0;
    size_t i=0;
    uint32_t page=0;
    while(i<len) {
        uint8_t prefix=desc[i++];
        if(prefix==0xfeu) {
            if(i+2u>len)return 0;
            size_t n=desc[i];
            i+=2u;
            if(n>len-i)return 0;
            i+=n;
            continue;
        }
        unsigned size=prefix&3u;
        if(size==3u)size=4u;
        if(size>len-i)return 0;
        unsigned type=(prefix>>2u)&3u,tag=(prefix>>4u)&15u;
        uint32_t value=0;
        for(unsigned b=0;b<size;b++)value|=(uint32_t)desc[i+b]<<(8u*b);
        i+=size;
        if(type==1u && tag==0u)page=value; /* Usage Page */
        if(type==2u && tag==0u && page==1u && value==0x38u)
            return 1; /* Wheel on Generic Desktop */
    }
    return 0;
}

/* Simple HID relative mouse layout: buttons, dx, dy, wheel (optional). */
static inline int frf_usb_wheel_boot_delta(const uint8_t *report,size_t len)
{
    return (report && len >= 4u) ? (int)(int8_t)report[3] : 0;
}

/* Mouse data with one-byte report ID preceding buttons/dx/dy/wheel. */
static inline int frf_usb_wheel_report_id_delta(const uint8_t *report,
                                                size_t len, uint8_t id)
{
    return (report && len>=5u && report[0]==id) ?
           (int)(int8_t)report[4] : 0;
}

/* One direction per USB report. Clamp anomalous/high-resolution values to
 * bound CIA queue occupancy and protect normal keyboard responsiveness. */
static inline unsigned frf_usb_wheel_emit(int delta,
                                          void (*raw)(uint8_t))
{
    if (!raw || delta==0) return 0;
    unsigned count=(unsigned)(delta<0 ? -delta : delta);
    if(count>FRF_USB_WHEEL_MAX_STEPS)count=FRF_USB_WHEEL_MAX_STEPS;
    uint8_t code=(uint8_t)(delta>0 ? FRF_NM_WHEEL_UP : FRF_NM_WHEEL_DOWN);
    for(unsigned i=0;i<count;i++) {
        raw(code);            /* NewMouse key-down */
        raw((uint8_t)(code|0x80u)); /* matching key-up */
    }
    return count;
}
#endif
