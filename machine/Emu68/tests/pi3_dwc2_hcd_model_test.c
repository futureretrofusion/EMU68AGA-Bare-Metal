/*
 * Compile/run TinyUSB DWC2 host on a software model of the Pi3 MMIO
 * window, including delayed AHB/core reset and HPRT power latch.
 *
 * This is a controller-startup regression test, NOT a USB hardware test.
 */
#define _GNU_SOURCE 1
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include "tusb.h"
#include "host/hcd.h"
#include "host/hub.h"
#include "portable/synopsys/dwc2/dwc2_type.h"
#include "common/tusb_fifo.h"
#include "class/hid/hid_device.h"
#include <stdarg.h>
static uint16_t simulated_joy1;
static int simulated_fire;
void aga_virtual_joy_set(int port,uint16_t joy,int fire) {
    if(port==1){ simulated_joy1=joy; simulated_fire=fire; }
}
void kprintf(const char *fmt,...) {(void)fmt;}
void frf_post_native_usb_vendor_probe(uint32_t vidpid,uint32_t iface,uint32_t kind) { (void)vidpid;(void)iface;(void)kind; }
void frf_post_native_usb_device_event(uint32_t addr,uint32_t id,uint32_t iface,uint32_t event) { (void)addr;(void)id;(void)iface;(void)event; }
extern void frf_tinyusb_pad_mount(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern void frf_tinyusb_pad_unmount(uint8_t,uint8_t);
extern void frf_tinyusb_pad_report(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern uint32_t frf_tinyusb_gamepad_count(void);
extern uint32_t frf_pad_is_joystick_usage(uint16_t,uint8_t);
void tu_hwfifo_write(volatile void *fifo,const uint8_t *b,uint16_t n,const tu_hwfifo_access_t *mode) {
 (void)mode;
 for(uint16_t i=0;i<n;i+=4) {uint32_t word=0;
  for(uint16_t j=0;j<4 && i+j<n;j++)word|=((uint32_t)b[i+j])<<(8*j);
  *(volatile uint32_t*)fifo=word;
 }
}
void tu_hwfifo_read(const volatile void *fifo,uint8_t *b,uint16_t n,const tu_hwfifo_access_t *mode) {
 (void)mode;
 for(uint16_t i=0;i<n;i+=4) {uint32_t word=*(const volatile uint32_t*)fifo;
  for(uint16_t j=0;j<4 && i+j<n;j++)b[i+j]=(uint8_t)(word>>(8*j));
 }
}
#define HW_BASE 0xf2980000UL
#define HW_LEN  0x10000u
#define GUSBCFG 0x00cu
#define GRSTCTL 0x010u
#define GINTSTS 0x014u
#define GSNPSID 0x040u
#define GHWCFG2 0x048u
#define GHWCFG3 0x04cu
#define GHWCFG4 0x050u
#define HPRT    0x440u
#define PCGCCTL 0xe00u
#define AHBIDLE 0x80000000u
/* Host-side stand-in for Pi3 GPU bus translation and cache hooks.
   The ARM integration calls mmu_virt2phys() and uses CPU cache operations. */
static volatile unsigned dma_clean_calls,dma_invalidate_calls,dma_translate_calls;
extern uint16_t frf_usb_control_wire_data_length(tusb_control_request_t const*);
static volatile uint32_t trace_ctrl_wire,trace_ctrl_cpu;
void frf_pi3_trace_control_length(uint16_t wire,uint16_t cpu) {
    trace_ctrl_wire=wire;trace_ctrl_cpu=cpu;
}
void frf_pi3_trace_ep(uint32_t raw,uint32_t cached_mps,uint32_t desc_mps) {
    printf("DEBUG EP OPEN raw=%08x cache_mps=%u desc_mps=%u\n",
           raw,cached_mps,desc_mps);
}
void frf_pi3_trace_xfer(uint32_t raw,uint32_t size,uint32_t packets,
                        uint32_t mps,uint32_t len) {
    printf("DEBUG EP XFER raw=%08x reg_size=%08x packets=%u mps=%u len=%u\n",
           raw,size,packets,mps,len);
}
static volatile unsigned enum_trace_count,enum_failed_trace_count;
void frf_tinyusb_enum_observe(uint32_t state,uint32_t result) {
    (void)state;
    ++enum_trace_count;
    if(result) ++enum_failed_trace_count;
}
void arm_flush_cache(uintptr_t p,uint32_t n){(void)p;(void)n;dma_clean_calls++;}
void arm_dcache_invalidate(uintptr_t p,uint32_t n){(void)p;(void)n;dma_invalidate_calls++;}
void* tusb_app_virt_to_phys(void *p) {
    if (!p) return NULL;
    dma_translate_calls++;
    return (void *)(uintptr_t)(0xc0100000u|((uintptr_t)p&0x000fffffu));
}
/* Real DWC2 registers are little-endian while EMU68AGA executes big-endian
 * AArch64. Model the register storage order with a GCC scalar-storage-order
 * structure; every direct register access now crosses a real endian boundary.
 */
typedef struct __attribute__((scalar_storage_order("little-endian"))) {
    volatile uint32_t reg[0x4000];
} emulated_dwc2_mmio_t;
static volatile emulated_dwc2_mmio_t *dwc2_mmio;
static volatile uint32_t *raw_bus_words;
/* The DWC2 simulation must not overwrite a newer CPU GRSTCTL request
 * after observing an older value. Unlike an actual register bus, a RAM
 * simulator has an unsynchronised writer thread. Use CAS on bus words. */
static uint32_t bus_word(uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}
static bool write_if_unchanged(unsigned off,uint32_t before,uint32_t after) {
    uint32_t expected=bus_word(before);
    return __atomic_compare_exchange_n(
       &raw_bus_words[off/4],&expected,bus_word(after),false,
       __ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
}
static volatile int running=1;
/* 0 healthy, 1 no root hub, 2 stalled core reset,
   3 stalled TX FIFO, 5 root power not latched,
   6 root port enumeration exercises EP0, 7 invalid vendor ID.
   4 is deliberately not tested: this simple memory-array simulation
   cannot model the GINTSTS CMOD read-only + W1C combination. */
static volatile int test_mode=0;
static volatile unsigned core_resets, tx_flushes, rx_flushes, powered;
static volatile unsigned port_reset_asserted, ep0_setup_armed, port_enabled_seen;
static volatile uint32_t first_hcchar, first_hctsiz, last_hcchar;
static volatile uint32_t phy_cfg_during_reset;
uint32_t tusb_time_millis_api(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint32_t)(t.tv_sec*1000u+t.tv_nsec/1000000u);
}
static void* fake_controller(void *ctx) {
    (void)ctx;
    while(running) {
        uint32_t gr=dwc2_mmio->reg[GRSTCTL/4];
        if((gr&1u) && test_mode!=2) {
            if(write_if_unchanged(GRSTCTL,gr,AHBIDLE)){
                phy_cfg_during_reset=dwc2_mmio->reg[GUSBCFG/4];
                core_resets++;
            }
        } else if((gr&(1u<<5)) && test_mode!=3) {
            if(write_if_unchanged(GRSTCTL,gr,AHBIDLE)) tx_flushes++;
        } else if(gr&(1u<<4)) {
            if(write_if_unchanged(GRSTCTL,gr,AHBIDLE)) rx_flushes++;
        } else if(!(gr&AHBIDLE) && !((gr&1u) && test_mode==2) &&
                  !((gr&(1u<<5)) && test_mode==3)) {
            (void)write_if_unchanged(GRSTCTL,gr,AHBIDLE);
        }
        /* GINTSTS is read-only CMOD 1 and W1C other bits. */
        dwc2_mmio->reg[GINTSTS/4]=test_mode==4?0u:1u;
        /* HPRT read-only connection status and sticky power latch. */
        uint32_t port=dwc2_mmio->reg[HPRT/4];
        if(port & (1u<<12)) powered++;
        if(test_mode==6) {
            if(port & (1u<<8)) port_reset_asserted++;
            else if(port_reset_asserted) port_enabled_seen++;
            uint32_t hc=dwc2_mmio->reg[0x500u/4];
            if(hc&(1u<<31)){
                ++ep0_setup_armed;
                if(!first_hcchar) {
                    first_hcchar=hc;
                    first_hctsiz=dwc2_mmio->reg[0x510u/4];
                }
                last_hcchar=hc;
            }
        }
        uint32_t pwr=test_mode==5?0u:port&(1u<<12);
        uint32_t extra=test_mode==6?
           ((port&(1u<<8))|((port_reset_asserted && !(port&(1u<<8)))?(1u<<2):0u)):0u;
        dwc2_mmio->reg[HPRT/4] = pwr | (test_mode==1?0u:1u) | extra;
        usleep(30);
    }
    return NULL;
}
int main(int argc,char **argv) {
    if(argc>1) test_mode=atoi(argv[1]);
    extern tusb_control_request_t frf_hub_make_port_feature_request(uint8_t,uint8_t,uint8_t);
    extern tusb_control_request_t frf_hub_make_status_request(uint8_t);
    extern tusb_control_request_t frf_hub_make_descriptor_request(void);
    tusb_control_request_t power=frf_hub_make_port_feature_request(1u,8u,HUB_REQUEST_SET_FEATURE);
    tusb_control_request_t reset=frf_hub_make_port_feature_request(2u,4u,HUB_REQUEST_SET_FEATURE);
    tusb_control_request_t status_req=frf_hub_make_status_request(1u);
    tusb_control_request_t hub_desc=frf_hub_make_descriptor_request();
    const uint8_t power_wire[8]={0x23u,3u,8u,0u,1u,0u,0u,0u};
    const uint8_t reset_wire[8]={0x23u,3u,4u,0u,2u,0u,0u,0u};
    const uint8_t status_wire[8]={0xa3u,0u,0u,0u,1u,0u,4u,0u};
    const uint8_t desc_wire[8]={0xa0u,6u,0u,0x29u,0u,0u,9u,0u};
    if(memcmp(&power,power_wire,8)||memcmp(&reset,reset_wire,8)||
       memcmp(&status_req,status_wire,8)||memcmp(&hub_desc,desc_wire,8)){
        fprintf(stderr,"HUB ON-WIRE ENDIAN FAIL\n");
        return 13;
    }
    hub_port_status_response_t hub_status;
    const uint8_t hub_bytes[4]={0x01,0x01,0x11,0x00}; /* connected, powered; connect+reset change */
    memcpy(&hub_status,hub_bytes,4);
    if(frf_hub_status_bits(&hub_status)!=0x0101u ||
       frf_hub_change_bits(&hub_status)!=0x0011u) {
        fprintf(stderr,"HUB PORT STATUS LE DECODE FAIL\n");
        return 14;
    }
    if(test_mode==0) puts("HUB PORT1 POWER/GET_STATUS/RESET/DESC BYTES and change flags PASS");
    static const uint8_t standard_pad_descriptor[] = {
        TUD_HID_REPORT_DESC_GAMEPAD()
    };
    if(!frf_pad_is_joystick_usage(HID_USAGE_PAGE_DESKTOP,HID_USAGE_DESKTOP_GAMEPAD) ||
       frf_pad_is_joystick_usage(HID_USAGE_PAGE_DESKTOP,HID_USAGE_DESKTOP_MOUSE))
      return 16;
    frf_tinyusb_pad_mount(2u,0u,standard_pad_descriptor,
                         (uint16_t)sizeof(standard_pad_descriptor));
    if(frf_tinyusb_gamepad_count()!=1u) {
        puts("GAMEPAD descriptor classification FAILED");return 17;
    }
    uint8_t left_fire[11]={0xc0u,0,0,0,0,0,0,1u,0,0,0};
    frf_tinyusb_pad_report(2u,0u,left_fire,sizeof(left_fire));
    if(simulated_joy1!=0x0300u || simulated_fire!=1) {
        printf("GAMEPAD joystick/fire mapping FAILED joy=%04x fire=%d\n",simulated_joy1,simulated_fire);
        return 18;
    }
    uint8_t neutral[11]={0};
    frf_tinyusb_pad_report(2u,0u,neutral,sizeof(neutral));
    if(simulated_joy1 || simulated_fire) return 19;
    frf_tinyusb_pad_unmount(2u,0u);
    if(frf_tinyusb_gamepad_count()!=0u) return 20;
    if(test_mode==0) puts("HID GAMEPAD desc/mount, port2 movement/fire, neutral and unmount PASS");
    extern int frf_xpad_interface_kind(uint16_t,uint16_t,uint8_t,uint8_t,uint8_t,uint8_t);
    extern int frf_xpad_decode_report(int kind,const uint8_t*,uint16_t);
    if(frf_xpad_interface_kind(0x045eu,0x028eu,0u,0xffu,0x5du,0x01u)!=1 ||
       frf_xpad_interface_kind(0x146bu,0x0603u,0u,0xffu,0x5du,0x01u)!=1 ||
       frf_xpad_interface_kind(0x045eu,0x02eau,0u,0xffu,0x47u,0xd0u)!=2 ||
       frf_xpad_interface_kind(0x045eu,0x02eau,1u,0xffu,0x47u,0xd0u)!=0 ||
       frf_xpad_interface_kind(0x2404u,0x0078u,0u,0xffu,0u,0xffu)!=3 ||
       frf_xpad_interface_kind(0x2404u,0x0079u,0u,0xffu,0u,0xffu)!=0 ||
       frf_xpad_interface_kind(0x2404u,0x0078u,0u,0x03u,0u,0xffu)!=0 ||
       frf_xpad_interface_kind(0x1111u,0x2222u,0u,0xffu,0x47u,0xd0u)!=0 ||
       frf_xpad_interface_kind(0x045eu,0x02eau,0u,0x03u,0x5du,0x01u)!=0)
        return 21;
    uint8_t xbox360[20]={0x00,0x14,0x04,0x10}; /* dpad left + face A */
    if(!frf_xpad_decode_report(1,xbox360,sizeof(xbox360)) ||
       simulated_joy1!=0x0300u || !simulated_fire) return 22;
    uint8_t xboxone[20]={0x20,0,0,0,0x10,0x04};
    if(!frf_xpad_decode_report(2,xboxone,sizeof(xboxone)) ||
       simulated_joy1!=0x0300u || !simulated_fire) return 23;
    uint8_t xbox_neutral[20]={0};
    xbox_neutral[1]=0x14;
    if(!frf_xpad_decode_report(1,xbox_neutral,20) ||
       simulated_joy1!=0 || simulated_fire) return 24;
    if(frf_xpad_decode_report(1,xbox360,3)) return 25;
    /* A passive unknown vendor probe must never drive the Amiga port. */
    const uint16_t before_probe_joy=simulated_joy1;
    const int before_probe_fire=simulated_fire;
    if(frf_xpad_decode_report(3,xbox360,sizeof(xbox360)) ||
       simulated_joy1!=before_probe_joy ||
       simulated_fire!=before_probe_fire) return 26;
    if(test_mode==0)
        puts("R16 vendor 2404:0078 FF/00/FF passive-only, no guessed Amiga input PASS");
    if(test_mode==0) puts("VENDOR XINPUT 360 / Nacon / Xbox One GIP detect and JOY1/fire PASS");
    extern int frf_r20_is_receiver(uint16_t,uint16_t,uint8_t);
    if(!frf_r20_is_receiver(0x7e05u,0x0920u,0u) ||
       frf_r20_is_receiver(0x220du,0x330cu,0u) ||
       !frf_r20_is_receiver(0x7e05u,0x0920u,1u) ||
       !frf_r20_is_receiver(0x7e05u,0x0920u,3u)) return 29;
    extern int frf_pad_is_dyonder(uint16_t vid,uint16_t pid);
    extern int frf_dyonder_decode_report(const uint8_t *b,uint16_t len);
    if(!frf_pad_is_dyonder(0x2563u,0x0576u) ||
       !frf_pad_is_dyonder(0x2563u,0x0575u) ||
       frf_pad_is_dyonder(0x2563u,0x0577u) ||
       frf_pad_is_dyonder(0x1111u,0x0576u)) return 30;
    uint8_t dyonder_neutral[10]={1,0,0,8,128,128,128,128,0,0};
    if(!frf_dyonder_decode_report(dyonder_neutral,10u) ||
       simulated_joy1!=0 || simulated_fire) return 31;
    uint8_t dyonder_left_fire[10]={1,1,0,6,128,128,128,128,0,0};
    if(!frf_dyonder_decode_report(dyonder_left_fire,10u) ||
       simulated_joy1!=0x0300u || !simulated_fire) return 32;
    uint8_t dyonder_up[10]={1,0,0,0,128,128,128,128,0,0};
    if(!frf_dyonder_decode_report(dyonder_up,10u) ||
       simulated_joy1!=0x0100u || simulated_fire) return 33;
    uint8_t dyonder_mouse[10]={5,1,0,6,128,128,128,128,0,0};
    if(frf_dyonder_decode_report(dyonder_mouse,10u) ||
       simulated_joy1!=0x0100u || simulated_fire) return 34;
    if(frf_dyonder_decode_report(dyonder_left_fire,9u) ||
       frf_dyonder_decode_report(dyonder_left_fire,11u)) return 35;
    if(test_mode==0)
        puts("DYONDER 2563:0575/0576 HID ID1 10-byte hat/axes/buttons, reject ID5 mouse PASS");
    extern void frf_msc_probe_cbw_wire(uint8_t out[12],uint32_t bytes);
    uint8_t cbw_wire[12]={0};
    const uint8_t cbw_expected[12]={
        0x55u,0x53u,0x42u,0x43u, 0x42u,0x53u,0x55u,0x54u,
        0x00u,0x02u,0x00u,0x00u
    };
    frf_msc_probe_cbw_wire(cbw_wire,512u);
    if(memcmp(cbw_wire,cbw_expected,12u)) {
        fprintf(stderr,"MSC CBW BIG-ENDIAN WIRE FIELDS FAILED\n");
        return 15;
    }
    if(test_mode==0) puts("USB MSC READ10 CBW signature/tag/512-byte data length wire LE PASS");
    /* An ARM-BE host must transmit these exact USB little-endian SETUP
       bytes over DMA. This is not covered by the DWC2 register ABI test. */
    const tusb_control_request_t setup={
       .bmRequestType=0x80u, .bRequest=6u,
       .wValue=tu_htole16(0x0100u), .wIndex=tu_htole16(0),
       .wLength=tu_htole16(8u)
    };
    static const uint8_t expected_setup[8]={0x80,6,0,1,0,0,8,0};
    /* Link against the actual production USB host control-stage helper.
       On ARM-BE the old unconverted request->wLength was 0x0800 (2048),
       while the controller must receive 8. */
    uint16_t decoded_length=frf_usb_control_wire_data_length(&setup);
    if(test_mode==0)
      printf("CONTROL SETUP DATA LEN: raw=%04x decoded=%u expected=8\n",
             setup.wLength,(unsigned)decoded_length);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if(setup.wLength!=0x0800u) return 12; /* reproduces prior 2048-byte bug */
#endif
    if (decoded_length != 8u) {
        fprintf(stderr,"BE USB CONTROL LENGTH FAILURE: raw=%04x host=%u expected 8\n",
                setup.wLength,(unsigned)decoded_length);
        return 11;
    }
    if(memcmp(&setup,expected_setup,8)) {
       fprintf(stderr,"BAD USB SETUP endian bytes:");
       const uint8_t *b=(const uint8_t*)&setup;
       for(int i=0;i<8;i++) fprintf(stderr," %02x",b[i]);
       fputc('\n',stderr);
       return 7;
    }
    dwc2_channel_char_t cc={.value=0};
    cc.ep_size=8;
    cc.err_multi_count=1;
    cc.enable=1;
    dwc2_channel_tsize_t sz={.value=0};
    sz.xfer_size=8;
    sz.packet_count=1;
    sz.pid=3;
    printf("ARM-BE DWC2 register bitfields: HCCHAR=%08x HCTSIZ=%08x\n",
           cc.value,sz.value);
    if(cc.value!=0x80100008u || sz.value!=0x60080008u) return 10;

    void *mapped=mmap((void*)HW_BASE,HW_LEN,PROT_READ|PROT_WRITE,
                      MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0);
    if(mapped==MAP_FAILED){perror("mmap");return 2;}
    assert((uintptr_t)mapped==HW_BASE);
    dwc2_mmio=(volatile emulated_dwc2_mmio_t*)mapped;
    raw_bus_words=(volatile uint32_t*)mapped;
    /* Deliberately verify a nested DWC2 register union before controller
       initialization. Older AArch64-BE firmware decoded GNPTXSTS bytes
       reversed because its anonymous union silently used BE storage. */
    dwc2_mmio->reg[0x02cu/4]=0x13579bdfu;
    const uint32_t fifo_alias=((volatile dwc2_regs_t*)mapped)->gnptxsts;
    if (fifo_alias != 0x13579bdfu) {
        fprintf(stderr,"DWC2 LE nested GNPTXSTS alias FAIL %08x\n",fifo_alias);
        return 9;
    }
    dwc2_mmio->reg[0x02cu/4]=0u;
    dwc2_mmio->reg[GUSBCFG/4]=(1u<<4)|(1u<<3)|(1u<<20); /* begin in ULPI16+external-VBUS */
    dwc2_mmio->reg[GSNPSID/4]=test_mode==7?0u:0x4f54280au;
    dwc2_mmio->reg[GHWCFG2/4]=(2u<<6)|(2u<<3)|(7u<<14) |
                     (test_mode==8?(1u<<8):0u);
    /* Mode8 emulates BCM2837 hardware with ULPI HS + dedicated FS
       clock: USPi chooses the 48MHz HCFG setting for this combination. */
    dwc2_mmio->reg[GHWCFG3/4]=4096u<<16;
    dwc2_mmio->reg[GHWCFG4/4]=0u;
    dwc2_mmio->reg[GRSTCTL/4]=AHBIDLE;
    dwc2_mmio->reg[GINTSTS/4]=test_mode==4?0u:1u;
    dwc2_mmio->reg[HPRT/4]=test_mode==1?0u:1u;
    pthread_t worker;
    assert(pthread_create(&worker,NULL,fake_controller,NULL)==0);
    const tusb_rhport_init_t config={.role=TUSB_ROLE_HOST,.speed=TUSB_SPEED_HIGH};
    const uint32_t before=tusb_time_millis_api();
    const bool ok=tuh_rhport_init(0,&config);
    uint32_t elapsed=tusb_time_millis_api()-before;
    const uint32_t phy=dwc2_mmio->reg[GUSBCFG/4];
    const uint32_t port=dwc2_mmio->reg[HPRT/4];
    const uint32_t gahbcfg=dwc2_mmio->reg[0x008u/4];
    /* Exact USPi-compatible Pi3 FIFO layout, not generic TinyUSB. */
    if(ok && (dwc2_mmio->reg[0x024u/4]!=1024u ||
              dwc2_mmio->reg[0x028u/4]!=0x04000400u ||
              dwc2_mmio->reg[0x100u/4]!=0x04000800u)) {
        printf("Pi3 FIFO MISMATCH RX=%08x NPTX=%08x PTX=%08x\n",
              dwc2_mmio->reg[0x024u/4],dwc2_mmio->reg[0x028u/4],
              dwc2_mmio->reg[0x100u/4]);
        running=0;pthread_join(worker,NULL);return 1;
    }
    printf("HCD init=%u elapsed=%ums PHYCFG=%08x HPRT=%08x PCGCCTL=%08x reset=%u tx=%u rx=%u power_samples=%u PHY_BEFORE_RESET=%08x\n",
           ok,elapsed,phy,port,dwc2_mmio->reg[PCGCCTL/4],core_resets,tx_flushes,rx_flushes,powered,phy_cfg_during_reset);
    const bool expected = test_mode==0 || test_mode==1 || test_mode==6 || test_mode==8;
    const bool worked=(ok && (port & (1u<<12)) &&
             (phy_cfg_during_reset & (3u<<3))==(3u<<3) &&
             (phy & (1u<<20))==0u &&
             (phy & (1u<<4))==0 &&
             (phy & (1u<<3))==0 && core_resets && tx_flushes && rx_flushes);
    const bool as_expected= test_mode==1?(worked && !hcd_port_connect_status(0)):
                           (expected ? worked : !ok);
    if(ok && test_mode==8 && (dwc2_mmio->reg[0x400u/4]&3u)!=1u) {
        printf("BCM2837 dedicated-FS initial HCFG clock != 48MHz: %08x\n",
               dwc2_mmio->reg[0x400u/4]);
        running=0;pthread_join(worker,NULL);return 1;
    }
    if(ok && test_mode!=8 && (dwc2_mmio->reg[0x400u/4]&3u)!=0u) {
        printf("BCM2837 normal initial HCFG clock != 30/60MHz: %08x\n",
               dwc2_mmio->reg[0x400u/4]);
        running=0;pthread_join(worker,NULL);return 1;
    }
    /* The Pi3 USPi port REQUIRES max AXI burst bits [2:1] cleared;
       generic TinyUSB INCR4 would have failed this new test. */
    if(ok && ((gahbcfg&(3u<<1)) != 0 || !(gahbcfg&(1u<<4)))) {
        printf("BCM2837 DMA AHB mismatch: %08x (expected bits 2:1=0, bit4=1)\n",
               gahbcfg);
        running=0;pthread_join(worker,NULL);return 1;
    }
    if(ok && !(gahbcfg&(1u<<5))) {
        puts("DMA GAHBCFG.ENABLE missing");
        running=0;pthread_join(worker,NULL);return 1;
    }
    printf("MODE %d expected %s: %s\n",test_mode,
           expected?"init success":"init failure",
           as_expected?"PASS":"FAIL");
    if(!as_expected){
        running=0;pthread_join(worker,NULL);return 1;
    }
    if(test_mode==6 && ok) {
        /* Exercise TinyUSB host enumeration after preconnected root attach.
           The fake device cannot return descriptors, but hardware-level
           root reset and EP0 SETUP channel must become visible. */
        hcd_event_device_attach(0,false);
        uint32_t begin=tusb_time_millis_api();
        while((uint32_t)(tusb_time_millis_api()-begin)<1200u) {
            hcd_int_handler(0,false);
            tuh_task_ext(0,false);
            usleep(100);
        }
        const uint32_t dma_first=dwc2_mmio->reg[(0x500u+0x14u)/4];
        printf("FIRST CONTROL DMA bus=%08x translated=%u clean=%u invalidate=%u\n",
               dma_first,dma_translate_calls,dma_clean_calls,dma_invalidate_calls);
        if((dma_first&0xc0000000u)!=0xc0000000u || !dma_translate_calls ||
           !dma_clean_calls) {
            puts("DWC2 channel DMA address/cache: FAIL");
            running=0;pthread_join(worker,NULL);return 1;
        }
        /* HCCHAR[21:20] must specify at least one transaction per
           microframe (USPi always writes 1). Earlier TinyUSB wrote 0
           for *every* endpoint, even the root-hub control request. */
        uint32_t channel0=first_hcchar;
        printf("FIRST HCCHAR=%08x HCTSIZ=%08x LAST=%08x\n",
               channel0,first_hctsiz,last_hcchar);
        if (((channel0>>20)&3u)!=1u) {
            printf("INVALID USB HOST HCCHAR MC=%u raw=%08x (expected 1)\n",
                   (channel0>>20)&3u, channel0);
            running=0;pthread_join(worker,NULL);return 1;
        }
        printf("ROOT HUB simulated enumeration reset=%u enabled=%u EP0_SETUP=%u\n",
                port_reset_asserted,port_enabled_seen,ep0_setup_armed);
        if(!port_reset_asserted || !port_enabled_seen || !ep0_setup_armed) {
            running=0;pthread_join(worker,NULL);
            puts("SIMULATED ROOT ENUMERATION: FAIL");
            return 1;
        }
        puts("SIMULATED ROOT ENUMERATION: PASS (device transfer still unmocked)");
    }
    running=0;pthread_join(worker,NULL);
    return 0;
}

void aga_virtual_pad_buttons(int port, uint32_t buttons){(void)port;(void)buttons;}
