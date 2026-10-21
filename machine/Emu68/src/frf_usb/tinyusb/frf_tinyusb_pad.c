/* SPDX-License-Identifier: GPL-2.0-only
 * TinyUSB HID joystick/gamepad discovery and conservative first-pad mapping.
 * Recognition uses a real HID report descriptor, not a guessed USB VID/PID.
 * Mapping is enabled only for the 11-byte TinyUSB standard gamepad layout.
 */
#include <stdint.h>
#include <string.h>
#include "tusb.h"
#include "class/hid/hid_host.h"

#define FRF_PAD_SLOTS 8u
struct pad_itf {
    uint8_t addr,idx,report_id,report_is_common;
    uint32_t reports;
};
static struct pad_itf pads[FRF_PAD_SLOTS];
static uint32_t active_pads,gamepad_reports;
extern void aga_virtual_joy_set(int port,uint16_t joydat,int fire);
extern void kprintf(const char*,...);

uint32_t frf_tinyusb_gamepad_count(void) { return active_pads; }
uint32_t frf_tinyusb_gamepad_reports(void) { return gamepad_reports; }

static int first_free(void) {
    for(unsigned i=0;i<FRF_PAD_SLOTS;++i)
        if(!pads[i].addr) return (int)i;
    return -1;
}
static struct pad_itf *find(uint8_t addr,uint8_t idx) {
    for(unsigned i=0;i<FRF_PAD_SLOTS;++i)
        if(pads[i].addr==addr && pads[i].idx==idx) return &pads[i];
    return 0;
}
uint32_t frf_pad_is_joystick_usage(uint16_t page,uint8_t usage)
{
    return page==HID_USAGE_PAGE_DESKTOP &&
         (usage==HID_USAGE_DESKTOP_JOYSTICK ||
          usage==HID_USAGE_DESKTOP_GAMEPAD);
}
void frf_tinyusb_pad_mount(uint8_t addr,uint8_t idx,
                          const uint8_t *desc,uint16_t desc_len)
{
    if(!addr || !desc || !desc_len || find(addr,idx)) return;
    tuh_hid_report_info_t info[8];
    uint8_t n=tuh_hid_parse_report_descriptor(info,8u,desc,desc_len);
    for(uint8_t j=0;j<n;++j) {
        if(!frf_pad_is_joystick_usage(info[j].usage_page,info[j].usage))
            continue;
        int slot=first_free();
        if(slot<0) return;
        struct pad_itf *p=&pads[slot];
        p->addr=addr; p->idx=idx; p->report_id=info[j].report_id;
        p->report_is_common=1; /* exact payload length checked on receipt */
        ++active_pads;
        kprintf("[FRF-TINYUSB-PAD] HID joypad detected dev=%u itf=%u report_id=%u total=%u\\n",
                addr,idx,p->report_id,active_pads);
        return;
    }
}
void frf_tinyusb_pad_unmount(uint8_t addr,uint8_t idx)
{
    struct pad_itf *p=find(addr,idx);
    if(!p) return;
    memset(p,0,sizeof(*p));
    if(active_pads) --active_pads;
    /* Port 2 is exclusively assigned to first mapped gamepad. */
    if(!active_pads) aga_virtual_joy_set(1,0,0);
}
static void map_standard_report(const uint8_t *data)
{
    /* TinyUSB standard gamepad wire layout:
       signed x,y,z,rz,rx,ry; 1-based hat (0=center,1..8 compass);
       little-endian 32-bit buttons. Other layouts are detected and
       reported, but never guessed into joystick movement. */
    const int x=(int)(int8_t)data[0], y=(int)(int8_t)data[1];
    const uint8_t h=data[6];
    const uint32_t buttons=(uint32_t)data[7] |
      ((uint32_t)data[8]<<8) | ((uint32_t)data[9]<<16) |
      ((uint32_t)data[10]<<24);
    const int left=x< -40 || h==7 || h==8 || h==6;
    const int right=x>40 || h==2 || h==3 || h==4;
    const int up=y< -40 || h==1 || h==2 || h==8;
    const int down=y>40 || h==4 || h==5 || h==6;
    /* Amiga JOY1DAT X/Y quadrature, primary fire via CIA. */
    const uint16_t joy=(uint16_t)((left?0x0200u:0u) |
                (((left!=0)^(up!=0))?0x0100u:0u) |
                (right?0x0002u:0u) |
                (((right!=0)^(down!=0))?0x0001u:0u));
    aga_virtual_joy_set(1,joy,(buttons&0x0fu)!=0u);
}
void frf_tinyusb_pad_report(uint8_t addr,uint8_t idx,
                            const uint8_t *data,uint16_t len)
{
    struct pad_itf *p=find(addr,idx);
    if(!p || !data) return;
    if(p->report_id) {
        if(!len || data[0]!=p->report_id) return;
        ++data;--len;
    }
    ++p->reports;
    ++gamepad_reports;
    /* Only first detected controller drives port 2. Verify standard
       length; arbitrary vendor/XInput layouts require their own parser. */
    for(unsigned i=0;i<FRF_PAD_SLOTS;++i)
        if(pads[i].addr) {
            if(&pads[i]==p && len==11u) map_standard_report(data);
            break;
        }
}
