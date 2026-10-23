/* R14 executable regression of the actual TinyUSB/Dyonder pad mount/report
 * path. Works on x86 and on ARM64 big-endian QEMU.
 * Mocks only USB core descriptor/VID callbacks, not the pad logic.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include "tusb.h"
#include "class/hid/hid_host.h"

static uint16_t joy,joy0;
static int fire,fire0;
static uint32_t buttons_port1;
static uint32_t calls;
static uint32_t fake_ms,submit_attempts,fail_first=1,endpoint_busy;
uint32_t tusb_time_millis_api(void) {return fake_ms;}
bool tuh_hid_receive_ready(uint8_t addr,uint8_t idx) {
    (void)addr;(void)idx;return !endpoint_busy;
}
bool tuh_hid_receive_report(uint8_t addr,uint8_t idx) {
    (void)addr;(void)idx;
    ++submit_attempts;
    if(fail_first) {fail_first=0;return false;}
    endpoint_busy=1;
    return true;
}
void aga_virtual_joy_set(int port,uint16_t value,int f) {
    assert(port==0 || port==1);
    if(port==0){joy0=value;fire0=f;return;}
    joy=value;fire=f; ++calls;
}
void kprintf(const char *fmt,...) {(void)fmt;}
uint32_t frf_tinyusb_xpad_count(void) {return 0;}
bool tuh_vid_pid_get(uint8_t addr,uint16_t *vid,uint16_t *pid) {
    if(addr==1) {*vid=0x2563;*pid=0x0576;return true;}
    if(addr==2) {*vid=0x2563;*pid=0x0575;return true;}
    if(addr==3) {*vid=0x046d;*pid=0xc216;return true;}
    if(addr==5 || addr==6) {*vid=0x057eu;*pid=0x2009u;return true;}
    if(addr==4) {*vid=0x220du;*pid=0x330cu;return true;}
    return false;
}
uint8_t tuh_hid_interface_protocol(uint8_t addr,uint8_t idx) {
    (void)addr;
    return idx==1?HID_ITF_PROTOCOL_MOUSE:HID_ITF_PROTOCOL_NONE;
}
uint8_t tuh_hid_parse_report_descriptor(tuh_hid_report_info_t* info,
                                        uint8_t max_info,
                                        const uint8_t *desc,uint16_t len) {
    if(max_info && desc && len) {
        info[0].usage_page=HID_USAGE_PAGE_DESKTOP;
        info[0].usage=(desc[0]==2u)?
            HID_USAGE_DESKTOP_MOUSE:HID_USAGE_DESKTOP_GAMEPAD;
        info[0].report_id=1;
        return 1u;
    }
    return 0;
}
extern void frf_tinyusb_pad_mount(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern int frf_tinyusb_pad_is_dyonder_interface(uint8_t,uint8_t);
extern void frf_tinyusb_pad_unmount(uint8_t,uint8_t);
extern void frf_tinyusb_pad_report(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern int frf_tinyusb_pad_port2_owner(uint8_t,uint8_t);
extern uint32_t frf_tinyusb_gamepad_count(void);
extern uint32_t frf_tinyusb_dyonder_reports(void);
extern uint32_t frf_tinyusb_dyonder_ignored(void);
extern void frf_tinyusb_pad_retry_task(void);
extern uint32_t frf_tinyusb_pad_deferred(void);
extern uint32_t frf_tinyusb_pad_recovered(void);
extern uint32_t frf_r19_hid_valid_count(void);
extern int frf_r20_compat_receive(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern int frf_r27_receive(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern uint32_t frf_r20_seen(void),frf_r20_valid(void);
extern int frf_r20_is_receiver(uint16_t,uint16_t,uint8_t);

int main(void)
{
    uint8_t desc[3]={5,1,9};
    uint8_t mouse_only[3]={2,1,9};
    frf_tinyusb_pad_mount(1,1,mouse_only,3); /* ordinary boot mouse */
    assert(frf_tinyusb_gamepad_count()==0);
    /* Same HID_BOOT_MOUSE interface with actual gamepad collection:
       ID 01=10-byte pad, ID 05=mouse. Must accept real pad safely. */
    frf_tinyusb_pad_mount(1,1,desc,3);
    assert(frf_tinyusb_gamepad_count()==1);
    assert(frf_tinyusb_pad_is_dyonder_interface(1,1)==1);
    uint8_t composite_pad[10]={1,1,0,6,128,128,128,128,0,0};
    frf_tinyusb_pad_report(1,1,composite_pad,10);
    assert(joy==0x0300u && fire==1);
    uint8_t composite_mouse[6]={5,0,7,8,0,0};
    frf_tinyusb_pad_report(1,1,composite_mouse,6);
    assert(joy==0x0300u && fire==1);
    frf_tinyusb_pad_unmount(1,1);
    assert(frf_tinyusb_gamepad_count()==0);
    calls=0;
    frf_tinyusb_pad_mount(1,0,desc,3);
    assert(frf_tinyusb_gamepad_count()==1);
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==0);
    fake_ms=501;
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==1 && frf_tinyusb_pad_deferred()==1);
    fake_ms=1002;
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==2 && frf_tinyusb_pad_recovered()==1);
    uint8_t neutral[10]={1,0,0,8,128,128,128,128,0,0};
    frf_tinyusb_pad_report(1,0,neutral,10);
    assert(joy==0 && fire==0 && calls==1);
    uint8_t left_fire[10]={1,1,0,6,128,128,128,128,0,0};
    frf_tinyusb_pad_report(1,0,left_fire,10);
    assert(joy==0x0300 && fire && calls==2);
    uint8_t mouse[6]={5,1,2,3,4,5};
    frf_tinyusb_pad_report(1,0,mouse,6);
    assert(calls==2); /* report ID 5 is mouse, no stale joystick output */
    uint8_t other_buttons[10]={1,0x80u,0x40u,6,128,128,128,128,0,0};
    frf_tinyusb_pad_report(1,0,other_buttons,10);
    assert(joy==0x0300u && fire==0 && calls==3);
    assert(buttons_port1==0x4080u);
    frf_tinyusb_pad_unmount(1,0);
    assert(frf_tinyusb_gamepad_count()==0 && joy==0 && fire==0);
    frf_tinyusb_pad_mount(2,0,desc,3); /* mode-changing 0575 candidate */
    assert(frf_tinyusb_gamepad_count()==1);
    frf_tinyusb_pad_report(2,0,mouse,6);
    assert(frf_tinyusb_dyonder_ignored()==3);
    frf_tinyusb_pad_report(2,0,left_fire,10); /* promote only on ID1/10B */
    assert(frf_tinyusb_dyonder_reports()==5);
    assert(joy==0x0300 && fire);
    frf_tinyusb_pad_unmount(2,0);
    frf_tinyusb_pad_mount(3,0,desc,3); /* existing generic HID still works */
    assert(frf_tinyusb_gamepad_count()==1);
    frf_tinyusb_pad_unmount(3,0);
    assert(frf_tinyusb_gamepad_count()==0);
    /* R19 end-to-end actual TinyUSB mount -> HID packet -> Amiga
       JOY1DAT/fire, with a non-legacy 24-byte report format. */
    const uint8_t desc24[]={
      0x05,0x01,0x09,0x05,0xa1,0x01,
      0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x02,
      0x09,0x30,0x09,0x31,0x81,0x02,
      0x15,0x00,0x25,0x07,0x75,0x04,0x95,0x01,0x09,0x39,0x81,0x02,
      0x75,0x04,0x95,0x01,0x81,0x03,
      0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,0x25,0x01,
      0x75,0x01,0x95,0x10,0x81,0x02,
      0x06,0x00,0xff,0x75,0x08,0x95,0x13,0x81,0x03,0xc0
    };
    frf_tinyusb_pad_mount(4,0,desc24,sizeof(desc24));
    assert(frf_tinyusb_gamepad_count()==1);
    uint8_t rpt24[24]={0};
    rpt24[0]=128; rpt24[1]=128; rpt24[2]=8;
    frf_tinyusb_pad_report(4,0,rpt24,24);
    assert(joy==0 && !fire);
    rpt24[0]=0; rpt24[3]=1;
    frf_tinyusb_pad_report(4,0,rpt24,24);
    assert(joy==0x0300u && fire==1);
    assert(frf_r19_hid_valid_count()==2);
    rpt24[0]=255; rpt24[3]=0;
    frf_tinyusb_pad_report(4,0,rpt24,2); /* truncated: no guest update */
    assert(joy==0x0300u && fire==1);
    frf_tinyusb_pad_unmount(4,0);
    assert(joy==0 && fire==0);
    /* R41: 057E:2009 must prefer the parsed descriptor. Verify three
       independent HID button usages survive the complete pad publish path. */
    buttons_port1=0;
    frf_tinyusb_pad_mount(5,3,desc24,sizeof(desc24));
    assert(frf_tinyusb_gamepad_count()==1);
    uint8_t sw24[24]={0};
    sw24[0]=128; sw24[1]=128; sw24[2]=8;
    frf_tinyusb_pad_report(5,3,sw24,24);
    assert(joy==0 && fire==0 && buttons_port1==0);
    sw24[3]=1u;
    frf_tinyusb_pad_report(5,3,sw24,24);
    assert(fire==1 && buttons_port1==1u);
    sw24[3]=2u;
    frf_tinyusb_pad_report(5,3,sw24,24);
    assert(fire==0 && buttons_port1==2u);
    sw24[3]=4u;
    frf_tinyusb_pad_report(5,3,sw24,24);
    assert(fire==0 && buttons_port1==4u);
    frf_tinyusb_pad_unmount(5,3);
    assert(frf_tinyusb_gamepad_count()==0);
    /* R33: observed 220D:330C with non-gamepad descriptor: fallback
       must accept ID02 directions without requiring an ID01/fire. */
    frf_tinyusb_pad_mount(4,0,mouse_only,3);
    assert(frf_tinyusb_gamepad_count()==1);
    uint8_t raw02[27]={2,0,7,128,0,128,128};
    assert(frf_r27_receive(4,0,raw02,27));
    assert(joy==0x100 && !fire);
    uint8_t raw01[27]={1,1,15,128,128,128,128};
    assert(frf_r27_receive(4,0,raw01,27));
    assert(joy==0x100 && fire);
    raw02[4]=128;
    assert(frf_r27_receive(4,0,raw02,27));
    assert(joy==0 && fire);
    raw01[1]=0;
    assert(frf_r27_receive(4,0,raw01,27));
    assert(joy==0 && !fire);
    uint8_t raw03[27]={3,0,3,128,128,128,128};
    assert(frf_r27_receive(4,0,raw03,27));
    assert(joy==2u && !fire);
    raw03[2]=6u;
    assert(frf_r27_receive(4,0,raw03,27));
    assert(joy==0x300u && !fire);
    raw03[2]=7u;
    assert(frf_r27_receive(4,0,raw03,27));
    assert(joy==0 && !fire);
    frf_tinyusb_pad_unmount(4,0);
    /* R36: two interfaces on ONE physical USB pad do not overwrite
       each other's Amiga direction and CIA fire registers. */
    frf_tinyusb_pad_mount(4,0,mouse_only,3);
    frf_tinyusb_pad_mount(4,2,mouse_only,3);
    assert(frf_tinyusb_gamepad_count()==1u);
    uint8_t direction[27]={2,0,7,128,0,128,128};
    uint8_t press[27]={1,1,15,128,128,128,128};
    assert(frf_r27_receive(4,0,direction,27));
    assert(joy==0x100u && fire==0);
    assert(frf_r27_receive(4,2,press,27));
    assert(joy==0x100u && fire==1);
    direction[4]=128;
    assert(frf_r27_receive(4,0,direction,27));
    assert(joy==0 && fire==1);
    press[1]=0;
    assert(frf_r27_receive(4,2,press,27));
    assert(joy==0 && fire==0);
    frf_tinyusb_pad_unmount(4,0);
    frf_tinyusb_pad_unmount(4,2);
    assert(frf_tinyusb_gamepad_count()==0u);
    /* R36: two DISTINCT USB devices must remain on separate Amiga ports.
       Port 2 direction must not depend on port 1 fire. */
    frf_tinyusb_pad_mount(4,0,mouse_only,3);
    frf_tinyusb_pad_mount(3,0,desc24,sizeof(desc24));
    assert(frf_tinyusb_gamepad_count()==2u);
    uint8_t firstup[27]={2,0,7,128,0,128,128};
    assert(frf_r27_receive(4,0,firstup,27));
    assert(joy==0x100u && fire==0);
    rpt24[0]=0;rpt24[1]=128;rpt24[2]=8;rpt24[3]=1;
    frf_tinyusb_pad_report(3,0,rpt24,24);
    assert(joy==0x100u && fire==0);
    assert(joy0==0x300u && fire0==1);
    frf_tinyusb_pad_unmount(3,0);
    assert(joy==0x100u && fire==0 && joy0==0 && fire0==0);
    frf_tinyusb_pad_unmount(4,0);
    assert(joy==0 && fire==0);
    /* R34: The same 220D:330C receiver may expose its HID report on
       another non-boot interface. A hard-coded idx==0 gave PAD:00. */
    frf_tinyusb_pad_mount(4,2,mouse_only,3);
    assert(frf_tinyusb_gamepad_count()==1);
    assert(frf_tinyusb_pad_port2_owner(4,2));
    frf_tinyusb_pad_mount(4,0,mouse_only,3);
    assert(!frf_tinyusb_pad_port2_owner(4,0));
    assert(frf_tinyusb_pad_port2_owner(4,2));
    frf_tinyusb_pad_unmount(4,0);
    frf_tinyusb_pad_unmount(4,2);
    assert(frf_tinyusb_gamepad_count()==0);
    /* R21 real Pi3 photograph: USB daddr1 VID=7E05 PID=0920,
       HID interface 0; R20 wrongly used 220D:330C.
       Detect at HID class mount BEFORE any neutral report arrives. */
    assert(frf_r20_is_receiver(0x057eu,0x2009u,0u));
    assert(!frf_r20_is_receiver(0x220du,0x330cu,0u));
    assert(frf_r20_is_receiver(0x057eu,0x2009u,1u));
    assert(frf_r20_is_receiver(0x057eu,0x2009u,3u));
    frf_tinyusb_pad_mount(5,1,mouse_only,3); /* same VID, ordinary mouse */
    assert(frf_tinyusb_gamepad_count()==0);
    frf_tinyusb_pad_mount(5,3,mouse_only,3); /* real POST: HID instance 3 */
    assert(frf_tinyusb_gamepad_count()==1);
    /* R23: gamepad IN polling paced at 10ms while keyboard stays
       independent. Never repeatedly submit in the same clock tick. */
    endpoint_busy=0;
    const uint32_t attempts_before=submit_attempts;
    fake_ms+=9u;
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==attempts_before);
    fake_ms+=1u;
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==attempts_before+1u);
    endpoint_busy=0;
    fake_ms+=9u;
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==attempts_before+1u);
    fake_ms+=1u;
    frf_tinyusb_pad_retry_task();
    assert(submit_attempts==attempts_before+2u);
    endpoint_busy=0;
    uint8_t hw[24]={0};
    hw[3]=0;hw[4]=128;hw[5]=hw[6]=128;hw[9]=0xffu;
    hw[1]=1; /* FIRST arriving report has active left/fire, not neutral */
    assert(frf_r20_compat_receive(5,3,hw,23u)==0); /* reject truncation */
    assert(frf_r20_compat_receive(5,3,hw,24u)==1);
    assert(joy==0x0300u && fire==1);
    hw[3]=128;hw[4]=0;hw[1]=0;
    assert(frf_r20_compat_receive(5,3,hw,24u)==1);
    assert(joy==0x0100u && !fire);
    assert(frf_r20_seen()==2 && frf_r20_valid()==2);
    frf_tinyusb_pad_unmount(5,3);
    assert(frf_tinyusb_gamepad_count()==0 && joy==0 && !fire);
    /* LATE ATTACH still works after any complete 24-byte frame, no
       requirement for neutral p[9]=FF or centered sticks. */
    hw[3]=0;hw[4]=128;hw[1]=1;hw[9]=0;
    assert(frf_r20_compat_receive(6,3,hw,24u)==1);
    assert(frf_tinyusb_gamepad_count()==1);
    assert(joy==0x0200u && fire==1); /* left + hat-up */
    frf_tinyusb_pad_unmount(6,3);
    assert(frf_tinyusb_gamepad_count()==0 && joy==0 && !fire);
    puts("R39 057E:2009 runtime HID instance 3 -> PAD=1 before reports; no idx0 dependency PASS");
    puts("R19 actual HID mount -> 24B report -> Amiga JOY1DAT/fire, truncated reject PASS");
    puts("R15 DYONDER composite HID boot-mouse/ID1 pad, ID5 mouse isolate, 0575, retry, generic HID PASS");
    return 0;
}

void aga_virtual_pad_buttons(int port, uint32_t buttons){if(port==1)buttons_port1=buttons;}
