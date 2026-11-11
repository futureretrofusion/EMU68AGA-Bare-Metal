#include <stdint.h>
#include "frf_sdlog.h"
#include "support.h"
#include "frf_xhci_host.h"
#include "frf_native_usb_input.h"
#include "frf_timebase.h"
extern void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);
extern volatile uint8_t frf_usb_host_start_allowed;
extern volatile uint8_t frf_usb_boot_ready;
extern volatile uint8_t frf_usb_mouse_present;
extern volatile uint8_t frf_usb_mouse_seen;
extern volatile uint8_t frf_usb_mouse_buttons;
extern volatile uint32_t frf_usb_r54_attempts;
/* Dedicated cache-line aligned DMA buffer for HID report descriptors.
 * Never DMA into a CPU1 stack local: invalidation may destroy live frames. */
static uint8_t frf_native_pad_descriptor[512] __attribute__((aligned(64)));
extern void frf_post_usb_devices(unsigned kb, unsigned ms, unsigned gp, unsigned msc);
/* R91 sticky USB gamepad VID:PID, report descriptor and live input diag. */
volatile uint32_t frf_r91_last_pad_vidpid;
volatile uint32_t frf_r91_pad_desc_expected,frf_r91_pad_desc_received;
volatile uint32_t frf_r91_pad_candidate,frf_r91_pad_ready;
volatile uint32_t frf_r91_pad_status,frf_r91_last_pad_report;
volatile uint32_t frf_r91_last_pad_report_bytes,frf_r91_pad_accepted;
volatile uint32_t frf_r92_pad_hid_index=0xffffffffu;
/* R96 official Nintendo Switch Pro 057e:2009 USB command telemetry. */
volatile uint32_t frf_r96_switch_init_stage,frf_r96_switch_init_ack;
volatile uint32_t frf_r96_switch_init_errors,frf_r96_switch_input_rx;
volatile uint32_t frf_r96_switch_no_output_ep;
static uint8_t frf_r96_switch_input[64] __attribute__((aligned(64)));
static uint8_t frf_r96_switch_output[64] __attribute__((aligned(64)));
static void frf_r96_switch_command_init(uint32_t index)
{
    /* Reverse-engineered protocol 0x80 02 handshake, 03 baudrate,
       02 re-handshake, 04 force USB, then subcmd 03 report mode 30.
       Do not send 0x80 05: that reenables timeout/Bluetooth fallback. */
    static const uint8_t steps[3]={2u,3u,2u};
    for(uint32_t n=0u;n<3u;n++) {
        for(uint32_t j=0u;j<64u;j++)frf_r96_switch_output[j]=0u;
        frf_r96_switch_output[0]=0x80u;
        frf_r96_switch_output[1]=steps[n];
        frf_r96_switch_init_stage=1u+n;
        if(!frf_xhci_host_native_interrupt_out(index,
                                             frf_r96_switch_output,64u)) {
            ++frf_r96_switch_init_errors;
            /* Don't send 03 before first handshake; try Force USB below. */
            if(n==0u)break;
        }
        uint32_t got=0u;
        int ack=0;
        for(uint32_t t=0u;t<3u;t++) {
            if(!frf_xhci_host_native_interrupt_in_once(index,
                    frf_r96_switch_input,64u,&got))break;
            if(got>=2u && frf_r96_switch_input[0]==0x81u &&
               frf_r96_switch_input[1]==steps[n]) {
                ack=1;break;
            }
        }
        if(ack)frf_r96_switch_init_ack|=1u<<n;
        else ++frf_r96_switch_init_errors;
        if(n==0u && !ack)break;
    }
    for(uint32_t j=0u;j<64u;j++)frf_r96_switch_output[j]=0u;
    frf_r96_switch_output[0]=0x80u;
    frf_r96_switch_output[1]=0x04u; /* Force USB; no ACK */
    frf_r96_switch_init_stage=4u;
    if(!frf_xhci_host_native_interrupt_out(index,frf_r96_switch_output,64u))
        ++frf_r96_switch_init_errors;
    for(uint32_t j=0u;j<64u;j++)frf_r96_switch_output[j]=0u;
    frf_r96_switch_output[0]=0x01u; /* Rumble+subcommand */
    frf_r96_switch_output[2]=0x00u;
    frf_r96_switch_output[3]=0x01u;
    frf_r96_switch_output[4]=0x40u;
    frf_r96_switch_output[5]=0x40u;
    frf_r96_switch_output[6]=0x00u;
    frf_r96_switch_output[7]=0x01u;
    frf_r96_switch_output[8]=0x40u;
    frf_r96_switch_output[9]=0x40u;
    frf_r96_switch_output[10]=0x03u; /* SetInputReportMode */
    frf_r96_switch_output[11]=0x30u; /* full buttons/sticks */
    frf_r96_switch_init_stage=5u;
    if(!frf_xhci_host_native_interrupt_out(index,frf_r96_switch_output,64u))
        ++frf_r96_switch_init_errors;
    frf_r96_switch_init_stage=6u;
    kprintf("[R96-SWITCH] init slot%u ackMask=%02x txErrors=%u\n",
       index,frf_r96_switch_init_ack,frf_r96_switch_init_errors);
}

/* R58: independent host-side mouse trace; observed by CPU3 HDMI overlay.
 * Zero reports with zero polling means no mouse endpoint, not "no movement".
 * Polls/failures keep counting after HID endpoint becomes ready. */
volatile uint32_t frf_r58_hid_poll_attempts;
volatile uint32_t frf_r58_hid_poll_failures;
volatile uint32_t frf_r58_hid_short_reports;
volatile uint32_t frf_r58_hid_reports;
volatile uint32_t frf_r58_hid_movement;
volatile uint32_t frf_r58_hid_btn_changes;
volatile uint32_t frf_r58_hid_click_l,frf_r58_hid_click_r,frf_r58_hid_click_m;
volatile uint32_t frf_r58_hid_last_size,frf_r58_hid_raw;
volatile int32_t frf_r58_hid_dx,frf_r58_hid_dy;
volatile int32_t frf_r58_hid_sumx,frf_r58_hid_sumy;
volatile uint32_t frf_r58_hid_buttons,frf_r58_hid_ready;
extern void frf_xhci_host_diag_snapshot_cpu1(void);


void frf_pi4_xhci_hid_main(void)
{
    while (!__atomic_load_n(&frf_usb_host_start_allowed, __ATOMIC_ACQUIRE))
        __asm__ volatile("wfe");
    extern void frf_boot_stage(unsigned stage);
    /* Keep R75 startup critical path clear. CPU1 runs asynchronously and
     * waits for CPU0 to enter the guest before touching PCIe controller. */
    frf_timebase_calibrate();
    uint64_t boot_begin=0,boot_now=0,boot_hz=frf_timebase_hz();
    if (!boot_hz || boot_hz>2000000000ULL) boot_hz=54000000ULL;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(boot_begin));
    do {
        __asm__ volatile("yield");
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(boot_now));
    } while (boot_now-boot_begin<boot_hz*2ULL);
    frf_boot_stage(9);
    kprintf("[PI4-XHCI] CPU1 standalone HID start\n");
    /* R79: one bounded xHCI takeover per power cycle. The R76/R77 path
     * retried halt/HCRST/PCIe setup on every scan failure, risking corrupt
     * DMA state or contention with the guest. Failure is fail-closed:
     * CPU0 boot proceeds, CPU1 parks, no reboot and no repeat reset. */
    __atomic_store_n(&frf_usb_r54_attempts,1u,__ATOMIC_RELEASE);
    frf_sdlog_xhci_stage=1u;
    int scanned=frf_xhci_host_hid_scan_cpu1();
    uint32_t count=scanned?frf_xhci_host_native_count():0u;
    unsigned keyboard=0u, mouse=0u, gamepad=0u;
    uint8_t pad_ready[4]={0};
    if(count>4u)count=4u;
    for(uint32_t i=0u;i<count;i++) {
        uint32_t proto=255u,iface=0u;
        if(!frf_xhci_host_native_info(i,&proto,&iface))continue;
        if(proto==1u)++keyboard;
        else if(proto==2u)++mouse;
        else if(proto==0u) {
            frf_r92_pad_hid_index=i;
            ++frf_r91_pad_candidate;
            uint32_t vid=0u,pid=0u,report_len=0u,got_desc=0u;
            (void)frf_xhci_host_native_identity(i,&vid,&pid,&report_len);
            frf_native_usb_pad_set_identity_for(i,vid,pid);
            frf_r91_last_pad_vidpid=((vid&0xffffu)<<16)|(pid&0xffffu);
            frf_r91_pad_desc_expected=report_len;
            /* Pi3 USPi reads precisely wReportDescriptorLength from the
             * selected interface's HID descriptor, NOT a fixed 512B. */
            uint32_t wanted=report_len;
            if(wanted==0u)wanted=sizeof(frf_native_pad_descriptor);
            int desc_ok=0;
            if(wanted<=sizeof(frf_native_pad_descriptor)) {
                desc_ok=frf_xhci_host_native_control(i,0x81u,6u,0x2200u,
                                     (uint16_t)iface,frf_native_pad_descriptor,
                                     (uint16_t)wanted,1,&got_desc);
            }
            frf_r91_pad_desc_received=got_desc;
            frf_r91_pad_status=(desc_ok?0x10u:0xe1u);
            if(desc_ok && got_desc>=8u &&
               (!report_len || got_desc==report_len) &&
               frf_native_usb_pad_set_descriptor_for(i,
                                     frf_native_pad_descriptor,got_desc)) {
                pad_ready[i]=1u;
                ++gamepad;
                ++frf_r91_pad_ready;
                frf_r91_pad_status=0x05u;
            } else if(vid==0x057eu && pid==0x2009u &&
                      frf_native_usb_pad_enable_switchpro_for(i)) {
                pad_ready[i]=1u;
                ++gamepad;
                ++frf_r91_pad_ready;
                frf_r91_pad_status=0x96u;
                frf_r96_switch_command_init(i);
            } else if(vid==0x2563u && pid==0x0576u &&
                      frf_native_usb_pad_enable_shanwan_for(i)) {
                /* Exact device-specific Pi3 layout: ID1=10B, ID5 mouse.
                   Pi3 uses the fixed decoded layout instead of a generic
                   parser. Keep the alternative only for that VID:PID. */
                pad_ready[i]=1u;
                ++gamepad;
                ++frf_r91_pad_ready;
                frf_r91_pad_status=0x56u;
            } else if(desc_ok) {
                frf_r91_pad_status=0xe2u;
            }
            kprintf("[R91-PAD] slot%u VID:PID=%04x:%04x if%u reportDescriptor=%u received=%u ready=%u status=%02x\n",
                   i,vid,pid,iface,report_len,got_desc,
                   pad_ready[i]?1u:0u,frf_r91_pad_status);
        }
    }
    frf_xhci_host_diag_snapshot_cpu1();
    __atomic_store_n(&frf_r58_hid_ready,(keyboard+mouse+gamepad)?1u:0u,__ATOMIC_RELEASE);
    __atomic_store_n(&frf_usb_mouse_present,mouse?1u:0u,__ATOMIC_RELEASE);
    frf_post_usb_devices(keyboard,mouse,gamepad,0u);
    __atomic_store_n(&frf_usb_boot_ready,1u,__ATOMIC_RELEASE);
    __asm__ volatile("dmb ish; sev" ::: "memory");
    if(!count || !(keyboard+mouse+gamepad)) {
        frf_sdlog_xhci_stage=0x80000080u;
        kprintf("[R80-USB] no supported native HID endpoint; park CPU1\n");
        for (;;) __asm__ volatile("wfe");
    }
    frf_sdlog_xhci_stage=60u;
    frf_boot_stage(10);
    kprintf("[R80-USB] ARM USB: keyboard=%u mouse=%u gamepad=%u slots=%u\n",
            keyboard,mouse,gamepad,count);

    /* Poll all endpoints concurrently. Each live device has one outstanding
     * xHCI IN TD; CPU1 never blocks on an idle keyboard while a mouse is
     * generating movement. Completion events are matched by TRB pointer. */
    uint8_t report[64] __attribute__((aligned(64)));
    unsigned prev_buttons=0u;
    uint64_t log_next=0u,log_now=0u,log_hz=frf_timebase_hz();
    if(log_hz<1000000u || log_hz>1000000000u)log_hz=54000000u;
    __asm__ volatile("mrs %0, cntpct_el0":"=r"(log_now));
    log_next=log_now+log_hz*5u;
    for(;;) {
        /* The Pi3 host reboot uses aga_reboot(0); here this path is
           CPU1-only and works whether the guest is in AGS, Workbench
           or a WHDLoad game.  No 68k keyboard reset injection. */
        if(__atomic_exchange_n(&frf_native_usb_arm_reset_pending,0u,
                               __ATOMIC_ACQ_REL)) {
            extern void aga_reboot(int tryboot);
            kprintf("[R93-USB] LShift+Ctrl+F11: ARM full reboot requested\n");
            frf_sdlog_xhci_stage=0x9300f11u;
            aga_reboot(0);
            __builtin_unreachable();
        }
        uint32_t got=0u,device=0u,proto=255u;
        __atomic_add_fetch(&frf_r58_hid_poll_attempts,1u,__ATOMIC_RELAXED);
        int completed=frf_xhci_host_native_poll_any(report,sizeof(report),
                                                     &got,&device,&proto);
        if(completed && device<count) {
            __atomic_store_n(&frf_r58_hid_last_size,got,__ATOMIC_RELAXED);
            if(proto==1u && got>=8u) {
                frf_native_usb_keyboard_report(report);
                frf_sdlog_xhci_stage=71u;
            } else if(proto==0u && pad_ready[device] && got>0u) {
                if(report[0]==0x30u || report[0]==0x21u ||
                   report[0]==0x3fu)
                    ++frf_r96_switch_input_rx;
                frf_r91_last_pad_report=report[0];
                frf_r91_last_pad_report_bytes=got;
                if(frf_native_usb_pad_report_for(device,report,got)) {
                    ++frf_r91_pad_accepted;
                    frf_sdlog_xhci_stage=72u;
                }
            } else if(proto==2u && got>=3u) {
                frf_native_usb_mouse_report(report,got);
                unsigned buttons=report[0]&7u;
                int dx=(int)(int8_t)report[1];
                int dy=(int)(int8_t)report[2];
                uint32_t raw=(uint32_t)report[0]|((uint32_t)report[1]<<8)|
                       ((uint32_t)report[2]<<16)|
                       ((uint32_t)(got>=4u?report[3]:0u)<<24);
                __atomic_store_n(&frf_r58_hid_raw,raw,__ATOMIC_RELEASE);
                __atomic_store_n(&frf_r58_hid_dx,dx,__ATOMIC_RELAXED);
                __atomic_store_n(&frf_r58_hid_dy,dy,__ATOMIC_RELAXED);
                __atomic_store_n(&frf_r58_hid_buttons,buttons,__ATOMIC_RELAXED);
                __atomic_add_fetch(&frf_r58_hid_reports,1u,__ATOMIC_RELAXED);
                if(dx || dy) {
                    __atomic_add_fetch(&frf_r58_hid_movement,1u,__ATOMIC_RELAXED);
                    __atomic_add_fetch(&frf_r58_hid_sumx,dx,__ATOMIC_RELAXED);
                    __atomic_add_fetch(&frf_r58_hid_sumy,dy,__ATOMIC_RELAXED);
                }
                if((buttons^prev_buttons)&7u) {
                    __atomic_add_fetch(&frf_r58_hid_btn_changes,1u,__ATOMIC_RELAXED);
                    if((buttons&1u) && !(prev_buttons&1u))
                        __atomic_add_fetch(&frf_r58_hid_click_l,1u,__ATOMIC_RELAXED);
                    if((buttons&2u) && !(prev_buttons&2u))
                        __atomic_add_fetch(&frf_r58_hid_click_r,1u,__ATOMIC_RELAXED);
                    if((buttons&4u) && !(prev_buttons&4u))
                        __atomic_add_fetch(&frf_r58_hid_click_m,1u,__ATOMIC_RELAXED);
                }
                prev_buttons=buttons;
                __atomic_store_n(&frf_usb_mouse_buttons,
                                 (uint8_t)buttons,__ATOMIC_RELEASE);
                __atomic_store_n(&frf_usb_mouse_seen,1u,__ATOMIC_RELEASE);
                frf_sdlog_xhci_stage=70u;
            } else {
                __atomic_add_fetch(&frf_r58_hid_short_reports,1u,__ATOMIC_RELAXED);
            }
        }
        __asm__ volatile("mrs %0, cntpct_el0":"=r"(log_now));
        if(log_now>=log_next) {
            extern volatile uint32_t frf_r82_completed[4],frf_r82_failed[4];
            extern volatile uint32_t frf_r83_last_slot,frf_r83_last_ep,frf_r83_last_cc;
            kprintf("[R83-USB] K=%u M=%u G=%u polls=%u reports=%u/%u/%u TD=%u/%u/%u/%u bad=%u/%u/%u/%u lastSlot=%u EP=%u CC=%u\n",
                keyboard,mouse,gamepad,frf_r58_hid_poll_attempts,
                frf_native_usb_key_reports,frf_native_usb_mouse_reports,frf_native_usb_pad_reports,
                frf_r82_completed[0],frf_r82_completed[1],frf_r82_completed[2],frf_r82_completed[3],
                frf_r82_failed[0],frf_r82_failed[1],frf_r82_failed[2],frf_r82_failed[3],
                frf_r83_last_slot,frf_r83_last_ep,frf_r83_last_cc);
            log_next=log_now+log_hz*5u;
        }
        for (unsigned i=0;i<16;i++) __asm__ volatile("yield");
    }
}
