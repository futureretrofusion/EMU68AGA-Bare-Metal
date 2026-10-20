/* SPDX-License-Identifier: GPL-2.0-only
 * EMU68AGA TinyUSB host glue. The vendored TinyUSB sources are MIT.
 * Experimental: DWC2 hardware and TT split scheduling need a Pi3 test.
 *
 * TinyUSB host task and DWC2 interrupt pump both live exclusively on CPU1.
 * ARM is big-endian but DWC2 register and FIFO words are little-endian.
 * DWC2 C translation units use little scalar storage order; the custom
 * FIFO helpers below explicitly convert words to/from USB bus byte order.
 */
#include <stdint.h>
#include <stddef.h>
#include "tusb.h"
#include "host/hcd.h"
#include "class/hid/hid_host.h"
#include "class/msc/msc_host.h"
#include "common/tusb_fifo.h"

extern volatile uint8_t frf_usb_boot_ready;
extern volatile uint8_t frf_usb_mouse_present;
extern volatile uint8_t frf_usb_mouse_seen;
extern volatile uint8_t frf_usb_mouse_buttons;
extern void frf_native_usb_keyboard_boot_report(const uint8_t report[8]);
extern void aga_virtual_mouse_update(int dx,int dy,unsigned buttons);
extern void frf_boot_stage(unsigned stage);
extern void frf_post_native_usb_stage(unsigned stage,int rc,
                                     uint32_t detail,uint32_t aux);
extern void kprintf(const char *format,...);

static volatile uint32_t frf_usb_hid_keyboard,frf_usb_hid_mouse,frf_usb_storage;
static volatile uint32_t frf_usb_reports,frf_usb_enumerations;

uint32_t tusb_time_millis_api(void)
{
    uint64_t now,hz;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    if(!hz) return 0;
    return (uint32_t)((now/(uint64_t)hz)*1000u+
                      ((now%(uint64_t)hz)*1000u)/(uint64_t)hz);
}

/* TinyUSB port-specific fixed-register FIFO access. DO NOT rely on the
 * BE CPU's uint32_t pointer storage order for little-endian USB FIFO words.
 * Each access is a four-byte, 32-bit bus transfer to a fixed MMIO address.
 */
void tu_hwfifo_write(volatile void *fifo,const uint8_t *src,
                     uint16_t len,const tu_hwfifo_access_t *mode)
{
    (void)mode;
    for(uint16_t i=0;i<len;i=(uint16_t)(i+4u)) {
        uint32_t word=0;
        for(uint16_t j=0;j<4u && i+j<len;++j)
            word |= (uint32_t)src[i+j] << (8u*j);
        __asm__ volatile("dmb oshst" ::: "memory");
        *(volatile uint32_t *)fifo=__builtin_bswap32(word);
    }
    __asm__ volatile("dsb oshst" ::: "memory");
}
void tu_hwfifo_read(const volatile void *fifo,uint8_t *dst,
                    uint16_t len,const tu_hwfifo_access_t *mode)
{
    (void)mode;
    for(uint16_t i=0;i<len;i=(uint16_t)(i+4u)) {
        uint32_t word=__builtin_bswap32(*(const volatile uint32_t *)fifo);
        __asm__ volatile("dmb oshld" ::: "memory");
        for(uint16_t j=0;j<4u && i+j<len;++j)
            dst[i+j]=(uint8_t)(word >> (8u*j));
    }
}
void tuh_mount_cb(uint8_t daddr)
{
    frf_usb_enumerations++;
    kprintf("[FRF-TINYUSB] device mounted address=%u\\n",daddr);
}
void tuh_umount_cb(uint8_t daddr)
{
    kprintf("[FRF-TINYUSB] device removed address=%u\\n",daddr);
}
void tuh_hid_mount_cb(uint8_t addr,uint8_t idx,
                       const uint8_t *descriptor,uint16_t descriptor_len)
{
    (void)descriptor;(void)descriptor_len;
    const uint8_t protocol=tuh_hid_interface_protocol(addr,idx);
    if(protocol==HID_ITF_PROTOCOL_KEYBOARD) frf_usb_hid_keyboard++;
    else if(protocol==HID_ITF_PROTOCOL_MOUSE) {
        frf_usb_hid_mouse++;
        frf_usb_mouse_present=1;
    }
    frf_post_native_usb_stage(7,0,((uint32_t)frf_usb_hid_keyboard<<16)|
                              frf_usb_hid_mouse,frf_usb_enumerations);
    (void)tuh_hid_receive_report(addr,idx);
}
void tuh_hid_umount_cb(uint8_t addr,uint8_t idx)
{
    (void)addr;(void)idx;
}
void tuh_hid_report_received_cb(uint8_t addr,uint8_t idx,
                                const uint8_t *data,uint16_t len)
{
    const uint8_t protocol=tuh_hid_interface_protocol(addr,idx);
    if(protocol==HID_ITF_PROTOCOL_KEYBOARD && len>=8u) {
        frf_native_usb_keyboard_boot_report(data);
        frf_usb_reports++;
    } else if(protocol==HID_ITF_PROTOCOL_MOUSE && len>=3u) {
        uint8_t buttons=data[0];
        aga_virtual_mouse_update((int)(int8_t)data[1],
                                 (int)(int8_t)data[2],buttons & 7u);
        frf_usb_mouse_seen=1;
        frf_usb_mouse_buttons=buttons;
        frf_usb_reports++;
    }
    (void)tuh_hid_receive_report(addr,idx);
}
void tuh_msc_mount_cb(uint8_t addr)
{
    ++frf_usb_storage;
    kprintf("[FRF-TINYUSB] MSC device enumerated address=%u; guest disk not mapped\\n",addr);
}
void tuh_msc_umount_cb(uint8_t addr)
{
    (void)addr;
    if(frf_usb_storage) --frf_usb_storage;
}
void frf_tinyusb_host_main(void)
{
    frf_post_native_usb_stage(2,0,0xf2980000u,0x54555342u);
    /* Core selection and VBUS request were already done on CPU1 above. */
    const tusb_rhport_init_t pi3_host={.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH};
    int ok=tuh_rhport_init(0,&pi3_host);
    frf_post_native_usb_stage(3,ok?0:-1,
                              frf_usb_enumerations,frf_usb_reports);
    frf_boot_stage(10);
    __atomic_store_n(&frf_usb_boot_ready,1u,__ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
    if(!ok) {
        for(;;) __asm__ volatile("wfe");
    }
    /* Pi 3B's internal LAN9514 USB hub is physically connected before
       our standalone CPU1 USB worker starts. A connection-change edge may
       therefore have occurred before DWC2 interrupt masks were enabled.
       TinyUSB otherwise waits forever for an edge that will never arrive.
       Seed a root attach based on current HPRT connect status. */
    bool connected_before=hcd_port_connect_status(0);
    frf_post_native_usb_stage(4,0,connected_before?1u:0u,0);
    if(connected_before) {
        hcd_event_device_attach(0,false);
    }
    bool last_connected=connected_before;
    uint32_t last_rescan=tusb_time_millis_api();
    uint32_t root_attach_attempts=connected_before?1u:0u;
    /* CPU1 processes the DWC2 interrupt status in one event/task context.
       Unlike earlier hand-written polling, TinyUSB owns hub TT scheduling.
       On an already-connected root hub, retry enumeration occasionally if
       no USB devices ever mounted. Timed retry avoids endlessly queuing
       duplicate attachment events. */
    for(;;) {
        hcd_int_handler(0,false);
        tuh_task_ext(0,false);
        uint32_t now=tusb_time_millis_api();
        bool connected=hcd_port_connect_status(0);
        if(connected != last_connected) {
            last_connected=connected;
            last_rescan=now;
            if(connected) {
                root_attach_attempts++;
                hcd_event_device_attach(0,false);
                frf_post_native_usb_stage(5,0,1u,root_attach_attempts);
            } else {
                hcd_event_device_remove(0,false);
                frf_post_native_usb_stage(5,-1,0u,root_attach_attempts);
            }
        }
        if(connected && frf_usb_enumerations==0 &&
           (uint32_t)(now-last_rescan)>=6000u &&
           root_attach_attempts<4u) {
            last_rescan=now;
            root_attach_attempts++;
            hcd_event_device_attach(0,false);
            frf_post_native_usb_stage(6,-2,root_attach_attempts,0);
        }
        __asm__ volatile("yield");
    }
}
