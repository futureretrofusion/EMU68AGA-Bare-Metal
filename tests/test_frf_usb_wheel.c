/* FRF Pi3 native USB wheel -> Amiga NewMouse regression tests.
 * cc -std=c11 -Wall -Wextra -Werror tests/test_frf_usb_wheel.c -o /tmp/frf_usb_wheel
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include "../machine/Emu68/src/frf_usb/frf_usb_wheel.h"
static uint8_t emitted[96];
static unsigned emitted_n;
static void send(uint8_t k){assert(emitted_n<sizeof(emitted));emitted[emitted_n++]=k;}
static void clear(void){emitted_n=0;}
int main(void)
{
    const uint8_t basic[3]={0x01,0xff,0x01};
    const uint8_t up[4]={0x02,0x01,0xfe,0x01};
    const uint8_t down[4]={0,1,2,0xff};
    const uint8_t iddown[5]={0x05,3,4,5,0xfe};
    const uint8_t descriptor[]={
        0x05,0x01, /* Usage Page Generic Desktop */
        0x09,0x02, /* Mouse */
        0xa1,0x01,
        0x09,0x01, /* Pointer */
        0x05,0x01,
        0x09,0x30, /* X */
        0x09,0x31, /* Y */
        0x09,0x38, /* Wheel */
        0x81,0x06,
        0xc0
    };
    const uint8_t no_wheel[]={0x05,0x01,0x09,0x30,0x09,0x31};
    const uint8_t other_page[]={0x05,0x0c,0x09,0x38};
    const uint8_t malformed[]={0xfe,0x40,0x00};
    assert(frf_usb_hid_has_wheel(descriptor,sizeof(descriptor)));
    assert(!frf_usb_hid_has_wheel(no_wheel,sizeof(no_wheel)));
    assert(!frf_usb_hid_has_wheel(other_page,sizeof(other_page)));
    assert(!frf_usb_hid_has_wheel(malformed,sizeof(malformed)));
    assert(!frf_usb_hid_has_wheel(NULL,0));
    assert(frf_usb_wheel_boot_delta(basic,sizeof(basic))==0);
    assert(frf_usb_wheel_emit(0,send)==0 && emitted_n==0);
    assert(frf_usb_wheel_boot_delta(up,sizeof(up))==1);
    assert(frf_usb_wheel_emit(frf_usb_wheel_boot_delta(up,4),send)==1);
    assert(emitted_n==2 && emitted[0]==0x7a && emitted[1]==0xfa);
    clear();
    assert(frf_usb_wheel_boot_delta(down,4)==-1);
    assert(frf_usb_wheel_emit(-1,send)==1);
    assert(emitted_n==2 && emitted[0]==0x7b && emitted[1]==0xfb);
    clear();
    assert(frf_usb_wheel_report_id_delta(iddown,5,5)==-2);
    assert(frf_usb_wheel_report_id_delta(iddown,5,1)==0);
    assert(frf_usb_wheel_report_id_delta(iddown,4,5)==0);
    assert(frf_usb_wheel_emit(-2,send)==2);
    assert(emitted_n==4);
    for(int i=0;i<4;i++)assert(emitted[i]==(uint8_t)((i%2)?0xfb:0x7b));
    clear();
    assert(frf_usb_wheel_emit(100,send)==8);
    assert(emitted_n==16);
    for(int i=0;i<16;i++)assert(emitted[i]==(uint8_t)((i%2)?0xfa:0x7a));
    clear();
    assert(frf_usb_wheel_emit(-128,send)==8);
    assert(emitted_n==16);
    for(int i=0;i<16;i++)assert(emitted[i]==(uint8_t)((i%2)?0xfb:0x7b));
    clear();
    assert(frf_usb_wheel_emit(-1,NULL)==0);
    assert(frf_usb_wheel_boot_delta(NULL,0)==0);
    puts("PASS: USB wheel to NewMouse raw events, signed delta, IDs, queue bounds");
    return 0;
}
