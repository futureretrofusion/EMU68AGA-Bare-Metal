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

static uint16_t joy;
static int fire;
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
    assert(port==1);joy=value;fire=f; ++calls;
}
void kprintf(const char *fmt,...) {(void)fmt;}
uint32_t frf_tinyusb_xpad_count(void) {return 0;}
bool tuh_vid_pid_get(uint8_t addr,uint16_t *vid,uint16_t *pid) {
    if(addr==1) {*vid=0x2563;*pid=0x0576;return true;}
    if(addr==2) {*vid=0x2563;*pid=0x0575;return true;}
    if(addr==3) {*vid=0x046d;*pid=0xc216;return true;}
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
        info[0].usage=HID_USAGE_DESKTOP_GAMEPAD;
        info[0].report_id=1;
        return 1u;
    }
    return 0;
}
extern void frf_tinyusb_pad_mount(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern void frf_tinyusb_pad_unmount(uint8_t,uint8_t);
extern void frf_tinyusb_pad_report(uint8_t,uint8_t,const uint8_t*,uint16_t);
extern uint32_t frf_tinyusb_gamepad_count(void);
extern uint32_t frf_tinyusb_dyonder_reports(void);
extern uint32_t frf_tinyusb_dyonder_ignored(void);
extern void frf_tinyusb_pad_retry_task(void);
extern uint32_t frf_tinyusb_pad_deferred(void);
extern uint32_t frf_tinyusb_pad_recovered(void);
int main(void)
{
    uint8_t desc[3]={5,1,9};
    frf_tinyusb_pad_mount(1,1,desc,3); /* boot mouse must never be counted */
    assert(frf_tinyusb_gamepad_count()==0);
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
    frf_tinyusb_pad_unmount(1,0);
    assert(frf_tinyusb_gamepad_count()==0 && joy==0 && fire==0);
    frf_tinyusb_pad_mount(2,0,desc,3); /* mode-changing 0575 candidate */
    assert(frf_tinyusb_gamepad_count()==1);
    frf_tinyusb_pad_report(2,0,mouse,6);
    assert(frf_tinyusb_dyonder_ignored()==2);
    frf_tinyusb_pad_report(2,0,left_fire,10); /* promote only on ID1/10B */
    assert(frf_tinyusb_dyonder_reports()==3);
    assert(joy==0x0300 && fire);
    frf_tinyusb_pad_unmount(2,0);
    frf_tinyusb_pad_mount(3,0,desc,3); /* existing generic HID still works */
    assert(frf_tinyusb_gamepad_count()==1);
    frf_tinyusb_pad_unmount(3,0);
    assert(frf_tinyusb_gamepad_count()==0);
    puts("DYONDER R14 TRUE MOUNT + ID1 REPORT + ID5 MOUSE ISOLATION + 0575 + GENERIC HID PASS");
    return 0;
}
