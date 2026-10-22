/* SPDX-License-Identifier: GPL-2.0-only
 * EMU68AGA TinyUSB host glue. The vendored TinyUSB sources are MIT.
 * Experimental: DWC2 hardware and TT split scheduling need a Pi3 test.
 *
 * TinyUSB host task and DWC2 interrupt pump both live exclusively on CPU1.
 * ARM is big-endian but DWC2 register and FIFO words are little-endian.
 * Only DWC2 register types use LE scalar storage; the normal host stack
 * remains BE. This version uses BCM2837 GPU-addressed internal DMA.
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
extern int frf_native_usb_reboot_requested(void);
extern void aga_reboot(int tryboot);
extern void aga_virtual_mouse_update(int dx,int dy,unsigned buttons);
extern void frf_boot_stage(unsigned stage);
extern void frf_post_native_usb_stage(unsigned stage,int rc,
                                     uint32_t detail,uint32_t aux);
extern void frf_post_native_usb_ahb(unsigned raw);
extern void frf_post_native_usb_peripherals(uint32_t,uint32_t,uint32_t);
extern void frf_post_native_usb_vendor_probe(uint32_t,uint32_t,uint32_t);
extern void kprintf(const char *format,...);
extern void frf_tinyusb_msc_mount(uint8_t addr);
extern void frf_tinyusb_msc_unmount(uint8_t addr);
extern void frf_tinyusb_msc_service(void);
extern uint32_t frf_tinyusb_msc_devices(void);
extern uint32_t frf_tinyusb_msc_capacity(void);
extern void frf_tinyusb_pad_mount(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern void frf_tinyusb_pad_unmount(uint8_t,uint8_t);
extern void frf_tinyusb_pad_report(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern uint32_t frf_tinyusb_gamepad_count(void);
extern uint32_t frf_tinyusb_gamepad_reports(void);
extern int frf_pad_is_dyonder(uint16_t,uint16_t);
extern uint32_t frf_tinyusb_dyonder_reports(void);
extern uint32_t frf_tinyusb_dyonder_ignored(void);
extern void frf_tinyusb_pad_retry_task(void);
extern int frf_tinyusb_pad_is_dyonder_interface(uint8_t,uint8_t);
extern void frf_post_native_usb_device_event(uint32_t,uint32_t,uint32_t,uint32_t);
extern void frf_post_native_usb_input_trace(const uint32_t *fields,unsigned count);
extern void aga_virtual_joy_link_stats(uint32_t out[4]);
extern uint32_t frf_tinyusb_xpad_raw_reports(void);
extern uint32_t frf_tinyusb_xpad_raw_data0(void);
extern uint32_t frf_tinyusb_xpad_raw_data1(void);
extern uint32_t frf_tinyusb_xpad_raw_meta(void);
extern void frf_tinyusb_vendor_probe_snapshot(uint32_t out[7]);
extern void frf_post_native_usb_probe_trace(const uint32_t fields[7]);
extern void frf_usb_packet_capture(uint8_t,uint8_t,uint8_t,const uint8_t*,uint16_t);
extern void frf_usb_packet_capture_snapshot(uint32_t out[55]);
extern void frf_post_native_usb_packet_bytes(const uint32_t words[55]);


extern void frf_post_native_usb_pad_stats(uint32_t,uint32_t);
extern void frf_tinyusb_xpad_task(void);
extern uint32_t frf_tinyusb_xpad_count(void);
extern uint32_t frf_tinyusb_xpad_packets(void);

/* Pi3 GPU/DWC2 DMA sees CPU RAM through the 0xC0000000 bus alias.
   Translate every virtual pointer through Emu68's live stage-1 MMU tables.
   DMA is attempted only for the Pi3 USB controller, never copied USPi code. */
extern uintptr_t mmu_virt2phys(uintptr_t va);
static volatile uint32_t frf_usb_dma_invalid_address;
void* tusb_app_virt_to_phys(void *virtual_address)
{
    if (!virtual_address) return NULL;
    uintptr_t physical=mmu_virt2phys((uintptr_t)virtual_address);
    if (physical == (uintptr_t)-1 || physical >= 0x40000000u) {
        frf_usb_dma_invalid_address++;
        return NULL;
    }
    return (void *)(uintptr_t)((physical & 0x3fffffffu) | 0xc0000000u);
}
static volatile uint32_t frf_usb_hid_keyboard,frf_usb_hid_mouse,frf_usb_storage;
static volatile uint32_t frf_usb_hid_interfaces;
#ifdef FRF_USB_DIAG_HOLD
static uint32_t frf_usb_diag_last_irq,frf_usb_diag_chan_irq,frf_usb_diag_port_irq;
extern void frf_post_native_usb_trace(const uint32_t*,unsigned);
#endif
static volatile uint32_t frf_usb_reports,frf_usb_enumerations;
/* Record RAW successful USB HID packets before any gamepad decoder.
   This identifies controller-synced-but-not-delivered failures. */
static volatile uint32_t frf_hid_rx_count,frf_hid_raw_word0,frf_hid_raw_word1;
static volatile uint32_t frf_hid_meta,frf_hid_source;
static uint32_t pack4(const uint8_t *p,uint16_t n,uint16_t start)
{
    uint32_t v=0;
    for(unsigned i=0;i<4 && start+i<n;++i)
        v|=((uint32_t)p[start+i]) << (24u-8u*i);
    return v;
}

static volatile uint32_t frf_usb_ctrl_len_wire,frf_usb_ctrl_len_cpu;
void frf_pi3_trace_control_length(uint16_t wire,uint16_t cpu) {
    frf_usb_ctrl_len_wire=(uint32_t)wire;
    frf_usb_ctrl_len_cpu=(uint32_t)cpu;
}
/* CPU1 HCD first-transaction register telemetry (no logging from ISR). */
static volatile uint32_t frf_pi3_ep_raw,frf_pi3_ep_mps,frf_pi3_desc_mps;
static volatile uint32_t frf_pi3_xfer_raw,frf_pi3_xfer_size,frf_pi3_xfer_packets;
extern void frf_post_native_usb_ep0(unsigned hcchar,unsigned hctsiz);
void frf_pi3_trace_ep(uint32_t raw,uint32_t cached_mps,uint32_t desc_mps) {
    if(!frf_pi3_ep_raw) {
        frf_pi3_ep_raw=raw;
        frf_pi3_ep_mps=cached_mps;
        frf_pi3_desc_mps=desc_mps;
    }
}
void frf_pi3_trace_xfer(uint32_t raw,uint32_t size,uint32_t packets,
                        uint32_t mps,uint32_t len) {
    (void)mps;(void)len;
    if(!frf_pi3_xfer_raw) {
        frf_pi3_xfer_raw=raw;
        frf_pi3_xfer_size=size;
        frf_pi3_xfer_packets=packets;
        frf_post_native_usb_ep0(raw,size);
    }
}
static volatile uint32_t frf_enum_fail_state,frf_enum_fail_result;
static volatile uint32_t frf_enum_failed_xfers,frf_enum_success_xfers;

/* Called from TinyUSB's actual USB enumeration transfer completion,
   never from a synthetic test or timed retry. */
void frf_tinyusb_enum_observe(uint32_t state,uint32_t result)
{
    if(result==0u) ++frf_enum_success_xfers;
    else {
        ++frf_enum_failed_xfers;
        frf_enum_fail_state=state;
        frf_enum_fail_result=result;
    }
}

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
    uint16_t vid=0,pid=0;
    (void)tuh_vid_pid_get(daddr,&vid,&pid);
    frf_usb_enumerations++;
    frf_post_native_usb_device_event(daddr,((uint32_t)vid<<16)|pid,0u,1u);
    kprintf("[FRF-TINYUSB] USB device mount addr=%u VID=%04x PID=%04x\\n",daddr,vid,pid);
}
void tuh_umount_cb(uint8_t daddr)
{
    frf_post_native_usb_device_event(daddr,0u,0u,0u);
    kprintf("[FRF-TINYUSB] device removed address=%u\\n",daddr);
}
void tuh_hid_mount_cb(uint8_t addr,uint8_t idx,
                       const uint8_t *descriptor,uint16_t descriptor_len)
{
    (void)descriptor;(void)descriptor_len;
    const uint8_t protocol=tuh_hid_interface_protocol(addr,idx);
    const uint32_t pad_before=frf_tinyusb_gamepad_count();
    frf_tinyusb_pad_mount(addr,idx,descriptor,descriptor_len);
    uint16_t pad_vid=0,pad_pid=0;
    if(tuh_vid_pid_get(addr,&pad_vid,&pad_pid) &&
       frf_pad_is_dyonder(pad_vid,pad_pid)) {
       const uint32_t subtype=(pad_pid==0x0576u)?4u:5u;
       frf_post_native_usb_vendor_probe(((uint32_t)pad_vid<<16)|pad_pid,
                0x03000000u|(uint32_t)idx,subtype);
    }
    if(protocol==HID_ITF_PROTOCOL_NONE &&
       pad_before==frf_tinyusb_gamepad_count()) {
        uint16_t vid=0,pid=0;
        (void)tuh_vid_pid_get(addr,&vid,&pid);
        /* Helpful when class 03 device has an unrecognised descriptor:
           class=03, subtype=00, protocol=00, interface index in low byte.
           A R13 photo can identify it even if PAD remains zero. */
        frf_post_native_usb_vendor_probe(((uint32_t)vid<<16)|pid,
                                           0x03000000u|idx,3u);
    }
    /* Log every HID device separately: the previous R14 panel only
       displayed the LAST vendor interface, possibly an unrelated drive. */
    frf_post_native_usb_device_event(addr,
        ((uint32_t)pad_vid<<16)|pad_pid,
        0x03000000u|((uint32_t)protocol<<8)|idx,2u);
    frf_usb_hid_interfaces++;
    if(protocol==HID_ITF_PROTOCOL_KEYBOARD) frf_usb_hid_keyboard++;
    else if(protocol==HID_ITF_PROTOCOL_MOUSE) {
        frf_usb_hid_mouse++;
        frf_usb_mouse_present=1;
    }
    if(frf_usb_hid_keyboard || frf_usb_hid_mouse)
        frf_post_native_usb_stage(7,0,((uint32_t)frf_usb_hid_keyboard<<16)|
                                  frf_usb_hid_mouse,frf_usb_enumerations);
    (void)tuh_hid_receive_report(addr,idx);
}
void tuh_hid_umount_cb(uint8_t addr,uint8_t idx)
{
    frf_tinyusb_pad_unmount(addr,idx);
}
void tuh_hid_report_received_cb(uint8_t addr,uint8_t idx,
                                const uint8_t *data,uint16_t len)
{
    const uint8_t protocol=tuh_hid_interface_protocol(addr,idx);
    frf_usb_packet_capture(1u,addr,idx,data,len);
    ++frf_hid_rx_count;
    frf_hid_meta=((uint32_t)len<<16)|((uint32_t)(len?data[0]:0u)<<8)|protocol;
    frf_hid_source=((uint32_t)addr<<16)|idx;
    frf_hid_raw_word0=pack4(data,len,0u);
    frf_hid_raw_word1=pack4(data,len,4u);
    /* Dyonder ID1 joystick and ID5 relative mouse can share the very
       same Boot-Mouse HID interface. Route by report ID before applying
       the ordinary three-byte boot-mouse decoding. */
    const int dyonder=frf_tinyusb_pad_is_dyonder_interface(addr,idx);
    if(dyonder) {
        frf_tinyusb_pad_report(addr,idx,data,len);
        if(len>=4u && data[0]==5u && protocol==HID_ITF_PROTOCOL_MOUSE) {
            const uint8_t buttons=data[1];
            aga_virtual_mouse_update((int)(int8_t)data[2],
                                     (int)(int8_t)data[3],buttons&7u);
            frf_usb_mouse_seen=1;
            frf_usb_mouse_buttons=buttons;
            ++frf_usb_reports;
        }
        (void)tuh_hid_receive_report(addr,idx);
        return;
    }
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
    /* Generic Desktop Gamepad/Joystick report interfaces, independent
       of keyboard/mouse boot protocol. */
    frf_tinyusb_pad_report(addr,idx,data,len);
    (void)tuh_hid_receive_report(addr,idx);
}
void tuh_msc_mount_cb(uint8_t addr)
{
    frf_tinyusb_msc_mount(addr);
    uint16_t vid=0,pid=0;
    (void)tuh_vid_pid_get(addr,&vid,&pid);
    frf_post_native_usb_device_event(addr,((uint32_t)vid<<16)|pid,
                                      0x08065000u,3u);
    ++frf_usb_storage;
    kprintf("[FRF-TINYUSB] MSC device enumerated address=%u; guest disk not mapped\\n",addr);
}
void tuh_msc_umount_cb(uint8_t addr)
{
    frf_tinyusb_msc_unmount(addr);
    if(frf_usb_storage) --frf_usb_storage;
}
static uint32_t pi3_dwc2_read(unsigned offset)
{
    uint32_t raw=*(volatile uint32_t *)(uintptr_t)(0xf2980000u+offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return __builtin_bswap32(raw);
}
void frf_tinyusb_host_main(void)
{
    /* Dump actual Synopsys ID and GHWCFG2 before changing the core. */
    frf_post_native_usb_stage(2,0,pi3_dwc2_read(0x040u),
                               pi3_dwc2_read(0x048u));
    /* Core selection and VBUS request were already done on CPU1 above. */
    const tusb_rhport_init_t pi3_host={.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH};
    int ok=tuh_rhport_init(0,&pi3_host);
    /* Show actual GUSBCFG PHY mode, HPRT and PCGCCTL power status;
       a successful return from hcd_init alone cannot prove port power. */
    uint32_t port_raw=pi3_dwc2_read(0x440u);
    frf_post_native_usb_ahb(pi3_dwc2_read(0x008u)); /* DMAEN bit5 */
    frf_post_native_usb_stage(3,ok?0:-1,
                              pi3_dwc2_read(0x00cu),port_raw);
    if(!ok) {
        frf_boot_stage(10);
        __atomic_store_n(&frf_usb_boot_ready,1u,__ATOMIC_RELEASE);
        __asm__ volatile("sev" ::: "memory");
        for(;;) __asm__ volatile("wfe");
    }
    /* Pi 3B's internal LAN9514 USB hub is physically connected before
       our standalone CPU1 USB worker starts. A connection-change edge may
       therefore have occurred before DWC2 interrupt masks were enabled.
       TinyUSB otherwise waits forever for an edge that will never arrive.
       Seed a root attach based on current HPRT connect status. */
    bool connected_before=hcd_port_connect_status(0);
    port_raw=pi3_dwc2_read(0x440u);
    /* Stage 4 shows raw HPRT (DETAIL), PCGCCTL (AUX).
       RC=-6 means HPRT_PWR didn't latch, RC=-7 means powered but
       no device connected to the root PHY (internal hub absent).
       Neither status is treated as HID success. */
    int port_rc=(port_raw&(1u<<12)) ?
                (connected_before?0:-7) : -6;
    frf_post_native_usb_stage(4,port_rc,port_raw,
                               pi3_dwc2_read(0xe00u));
    if(connected_before) {
        hcd_event_device_attach(0,false);
    }
    /* Initial HID enumeration is asynchronous in TinyUSB. The old code
       told CPU0 USB was ready BEFORE running a single event/task pass, so
       Kickstart could begin with no installed USB HID interface at all.
       Keep guest boot gated for a maximum of 5s or until a real keyboard
       or mouse class mount callback. This does not invent a device: only
       tuh_hid_mount_cb() sets these flags. */
    const uint32_t enumeration_started=tusb_time_millis_api();
#ifdef FRF_USB_DIAG_HOLD
    uint32_t next_live_draw=enumeration_started;
    uint32_t usb_task_ticks=0;
#endif
    bool guest_released=false;
    bool last_connected=connected_before;
    uint32_t last_rescan=enumeration_started;
    uint32_t root_attach_attempts=connected_before?1u:0u;
    uint32_t last_msc_post=~0u,last_pad_post=~0u,last_capacity_post=~0u;
    uint32_t last_link_draw=0u;
    uint32_t last_dy_ok=~0u,last_dy_ignore=~0u,last_dy_post=0u;
    /* CPU1 processes the DWC2 interrupt status in one event/task context.
       Unlike earlier hand-written polling, TinyUSB owns hub TT scheduling.
       On an already-connected root hub, retry enumeration occasionally if
       no USB devices ever mounted. Timed retry avoids endlessly queuing
       duplicate attachment events. */
    for(;;) {
#ifdef FRF_USB_DIAG_HOLD
        /* Observe before TinyUSB clears write-one-to-clear IRQ causes.
           The register values come from real LE MMIO, not the software
           test harness; this holds the Amiga guest while USB is tested. */
        const uint32_t gint_status=pi3_dwc2_read(0x014u);
        const uint32_t gint_mask=pi3_dwc2_read(0x018u);
        const uint32_t observed=gint_status & gint_mask;
        if(observed&(1u<<25)) frf_usb_diag_chan_irq++;
        if(observed&(1u<<24)) frf_usb_diag_port_irq++;
        const uint32_t channel_int=pi3_dwc2_read(0x508u);
        if(channel_int) frf_usb_diag_last_irq=channel_int;
        ++usb_task_ticks;
#endif
        hcd_int_handler(0,false);
        tuh_task_ext(0,false);
        frf_tinyusb_xpad_task(); /* vendor class Xbox controllers, CPU1 only */
        frf_tinyusb_pad_retry_task(); /* RC9-style initial HID pad retry */
        frf_tinyusb_msc_service(); /* CPU1 only; services 68k FRUM sector requests */
        /* Only refresh HDMI telemetry at most 4Hz; never draw from
           a USB interrupt callback or on every HID report. */
        const uint32_t dy_ok=frf_tinyusb_dyonder_reports();
        const uint32_t dy_ignore=frf_tinyusb_dyonder_ignored();
        const uint32_t dy_now=tusb_time_millis_api();
        /* Pi3 HDMI diagnostics only; actual USB ISR callbacks never draw.
           Read-only link telemetry shows four independent stages:
             USB packets -> decoded pad reports -> guest JOY1 state
             -> guest register reads. */
        if((uint32_t)(dy_now-last_link_draw)>=300u) {
            uint32_t g[4]={0};
            aga_virtual_joy_link_stats(g);
            const uint32_t input_link[15]={
                frf_hid_rx_count,frf_hid_source,frf_hid_meta,
                frf_hid_raw_word0,frf_hid_raw_word1,
                frf_tinyusb_xpad_raw_reports(),
                frf_tinyusb_xpad_raw_data0(),
                frf_tinyusb_xpad_raw_data1(),
                frf_tinyusb_xpad_raw_meta(),
                frf_tinyusb_gamepad_reports(),
                dy_ok,g[0],g[1],g[2],g[3]
            };
            frf_post_native_usb_input_trace(input_link,15u);
            uint32_t probe[7]={0};
            frf_tinyusb_vendor_probe_snapshot(probe);
            frf_post_native_usb_probe_trace(probe);
            uint32_t bytes[55]={0};
            frf_usb_packet_capture_snapshot(bytes);
            frf_post_native_usb_packet_bytes(bytes);
            last_link_draw=dy_now;
        }
        if((dy_ok!=last_dy_ok || dy_ignore!=last_dy_ignore) &&
           (uint32_t)(dy_now-last_dy_post)>=250u) {
            frf_post_native_usb_pad_stats(dy_ok,dy_ignore);
            last_dy_ok=dy_ok;last_dy_ignore=dy_ignore;
            last_dy_post=dy_now;
        }
        const uint32_t msc_total=frf_tinyusb_msc_devices();
        const uint32_t pad_total=frf_tinyusb_gamepad_count();
        const uint32_t blocks=frf_tinyusb_msc_capacity();
        if(msc_total!=last_msc_post || pad_total!=last_pad_post ||
           blocks!=last_capacity_post) {
            frf_post_native_usb_peripherals(msc_total,pad_total,blocks);
            last_msc_post=msc_total;
            last_pad_post=pad_total;
            last_capacity_post=blocks;
        }
        /* Reboot only from CPU1 worker context, never inside HID callback,
           and only after a newly pressed Ctrl+LeftShift+F11 chord. */
        if(frf_native_usb_reboot_requested()) {
            kprintf("[FRF-TINYUSB] Ctrl+LeftShift+F11: host reboot\n");
            aga_reboot(0);
        }
        uint32_t now=tusb_time_millis_api();
#ifdef FRF_USB_DIAG_HOLD
        if((int32_t)(now-next_live_draw)>=0) {
            next_live_draw=now+300u;
            uint32_t status[16]={
                pi3_dwc2_read(0x440u), /* HPRT */
                pi3_dwc2_read(0x014u), /* GINTSTS */
                pi3_dwc2_read(0x018u), /* GINTMSK */
                pi3_dwc2_read(0x414u), /* HAINT */
                pi3_dwc2_read(0x500u), /* HC0CHAR */
                pi3_dwc2_read(0x510u), /* HC0SIZE */
                pi3_dwc2_read(0x514u), /* HC0DMA */
                frf_usb_diag_last_irq, /* last nonzero HC0INT */
                pi3_dwc2_read(0x50cu), /* HC0INTMSK */
                usb_task_ticks,       /* host tick progress */
                frf_usb_diag_chan_irq,/* masked HCINT polls seen */
                frf_usb_diag_port_irq,/* masked HPRT IRQ polls seen */
                frf_usb_enumerations,  /* genuine mount callbacks */
                frf_usb_hid_interfaces,/* any HID mount, incl generic */
                ((frf_enum_success_xfers&0xffffu)<<16) |
                   (frf_enum_failed_xfers&0xffffu),
                ((frf_usb_ctrl_len_wire & 0xffffu)<<16)|
                  (frf_usb_ctrl_len_cpu & 0xffffu)
            };
            frf_post_native_usb_trace(status,16u);
        }
#endif
        if(!guest_released &&
           (frf_usb_hid_keyboard || frf_usb_hid_mouse ||
             (uint32_t)(now-enumeration_started)>=5000u)) {
            /* Stage 8 is the handshake checkpoint, NOT a claimed USB mount.
               RC=0 means a real HID mount; RC=-8 means a 5-second timeout.
               A failed mount preserves raw HPRT and PHY configuration. */
            uint32_t mounted=frf_usb_hid_keyboard+frf_usb_hid_mouse;
            uint32_t port=pi3_dwc2_read(0x440u);
            /* Distinguish physical root power (bit 12), physical hub
               presence (bit 0), DMA bus translation and HID timeout.
               All statuses use actual registers and real callback counts. */
            int fail_rc=!(port&(1u<<12)) ? -6 :
                        !(port&1u) ? -7 :
                        frf_usb_dma_invalid_address ? -9 :
                        frf_usb_hid_interfaces ? -12 :
                        frf_usb_enumerations ? -11 :
                        frf_enum_failed_xfers ? -10 : -8;
            uint32_t transfer_failure =
                   (frf_enum_fail_state & 0xffffu)<<16 |
                   (frf_enum_fail_result & 0xffffu);
            frf_post_native_usb_stage(8,mounted?0:fail_rc,
                          mounted?mounted:
                            (fail_rc==-10 ? transfer_failure :
                             (fail_rc==-11 || fail_rc==-12) ? frf_usb_enumerations : port),
                          mounted?frf_usb_enumerations:
                            (fail_rc==-10 ? port :
                             (fail_rc==-11 || fail_rc==-12) ? frf_usb_hid_interfaces :
                             frf_usb_dma_invalid_address ?
                             frf_usb_dma_invalid_address:pi3_dwc2_read(0x00cu)));
            frf_boot_stage(10);
            __atomic_store_n(&frf_usb_boot_ready,1u,__ATOMIC_RELEASE);
            __asm__ volatile("sev" ::: "memory");
            guest_released=true;
        }
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
            /* A scheduled rescan is not a USB hardware error. */
            frf_post_native_usb_stage(6,0,root_attach_attempts,0);
        }
        __asm__ volatile("yield");
    }
}
