/* SPDX-License-Identifier: GPL-2.0-only
 * TinyUSB HID joystick/gamepad discovery and conservative first-pad mapping.
 * Recognition uses a real HID report descriptor, not a guessed USB VID/PID.
 * Mapping is enabled only for the 11-byte TinyUSB standard gamepad layout.
 */
#include <stdint.h>
#include <string.h>
#include "tusb.h"
#include "class/hid/hid_host.h"
#include "frf_r19_hid_descriptor.h"
#include "frf_r27_220d.h"
#include "frf_r33_id02.h"

#define FRF_PAD_SLOTS 8u
struct pad_itf {
    uint8_t addr,idx,report_id,report_is_common;
    /* report_is_common: 0=unknown candidate,1=11B standard HID,
       2=DYONDER 2563:0576 10B ID1,3=2563:0575 alt-mode candidate */

    uint32_t reports;
    uint32_t next_submit_ms,retries,recoveries;
    struct frf_hid_map desc_map;
    uint16_t joy_report_state;
    uint16_t joy_id02_state,joy_id03_state;
    uint16_t joy_by_report[4];
    uint8_t id02_seen,id03_seen;
    uint8_t fire_report_state;
    uint32_t buttons_state;
    uint16_t published_joy;
    uint8_t published_fire, published_valid;
};
static struct pad_itf pads[FRF_PAD_SLOTS];
volatile uint8_t frf_r26_receiver_addr;
volatile uint8_t frf_r26_receiver_idx;
volatile uint8_t frf_r32_receiver_addr;
static uint32_t active_pads,gamepad_reports;
static volatile uint32_t r34_mount_calls,r34_claimed,r34_late_claimed;
static volatile uint32_t r27_seen,r27_ok,r27_bad,r27_joy,r27_fire;
void frf_r34_note_late(void){++r34_late_claimed;}
void frf_r34_claim_stats(uint32_t o[3]){o[0]=r34_mount_calls;o[1]=r34_claimed;o[2]=r34_late_claimed;}
uint32_t frf_r27_seen(void){return r27_seen;}
uint32_t frf_r27_ok(void){return r27_ok;}
uint32_t frf_r27_bad(void){return r27_bad;}
uint32_t frf_r27_joy(void){return r27_joy;}
uint32_t frf_r27_fire(void){return r27_fire;}
static uint32_t dyonder_report_ok,dyonder_report_ignore;
static uint32_t pad_submit_deferred,pad_submit_recovered;
static volatile uint32_t nr,so,sf,pt;
void frf_r25_poll_stats(uint32_t *o){o[0]=nr;o[1]=so;o[2]=sf;o[3]=pt;}
static volatile uint32_t descriptor_packets,descriptor_valid,descriptor_joy,descriptor_fire;
static volatile uint32_t descriptor_last_address,descriptor_fields;
extern int frf_r20_is_receiver(uint16_t,uint16_t,uint8_t);
extern int frf_r20_observed_neutral(const uint8_t*,uint16_t);
extern int frf_r20_decode(const uint8_t*,uint16_t,uint16_t*,int*);
static volatile uint32_t r20_seen,r20_valid,r20_rejected,r20_joy,r20_fire,r20_dev;
uint32_t frf_r20_seen(void){return r20_seen;}
uint32_t frf_r20_valid(void){return r20_valid;}
uint32_t frf_r20_rejected(void){return r20_rejected;}
uint32_t frf_r20_joy(void){return r20_joy;}
uint32_t frf_r20_fire(void){return r20_fire;}
uint32_t frf_r20_dev(void){return r20_dev;}

uint32_t frf_r19_hid_packet_count(void){return descriptor_packets;}
uint32_t frf_r19_hid_valid_count(void){return descriptor_valid;}
uint32_t frf_r19_hid_last_joy(void){return descriptor_joy;}
uint32_t frf_r19_hid_last_fire(void){return descriptor_fire;}
uint32_t frf_r19_hid_last_address(void){return descriptor_last_address;}
uint32_t frf_r19_hid_field_count(void){return descriptor_fields;}
static volatile uint32_t frf_r32_last_buttons;
uint32_t frf_r32_button_mask(void){return frf_r32_last_buttons;}
uint32_t frf_r31_target_mode(void){for(unsigned i=0;i<FRF_PAD_SLOTS;i++)if(pads[i].addr && pads[i].report_is_common==4u)return 4u;else if(pads[i].addr && pads[i].report_is_common==6u)return 6u;return 0u;}

extern uint32_t tusb_time_millis_api(void);
uint32_t frf_tinyusb_pad_deferred(void) {return pad_submit_deferred;}
uint32_t frf_tinyusb_pad_recovered(void) {return pad_submit_recovered;}

uint32_t frf_tinyusb_dyonder_reports(void) {return dyonder_report_ok;}
uint32_t frf_tinyusb_dyonder_ignored(void) {return dyonder_report_ignore;}
int frf_r19_hid_mapped_interface(uint8_t addr,uint8_t idx)
{
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++) {
        const struct pad_itf *p=&pads[i];
        if(p->addr==addr && p->idx==idx && p->report_is_common==4u)
            return 1;
    }
    return 0;
}
int frf_tinyusb_pad_is_dyonder_interface(uint8_t addr,uint8_t idx)
{
    const struct pad_itf *p;
    for(unsigned i=0;i<FRF_PAD_SLOTS;++i) {
        p=&pads[i];
        if(p->addr==addr && p->idx==idx &&
           (p->report_is_common==2u || p->report_is_common==3u))
            return 1;
    }
    return 0;
}

int frf_pad_is_dyonder(uint16_t vid,uint16_t pid)
{
    return vid==0x2563u && (pid==0x0576u || pid==0x0575u);
}
extern void aga_virtual_joy_set(int port,uint16_t joydat,int fire);
extern void aga_virtual_pad_buttons(int port,uint32_t buttons);
extern void kprintf(const char*,...);

extern uint32_t frf_tinyusb_xpad_count(void);
uint32_t frf_tinyusb_gamepad_count(void) {
    uint8_t unique[FRF_PAD_SLOTS]={0};
    unsigned n=0;
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++){
        if(!pads[i].addr)continue;
        bool found=false;
        for(unsigned j=0;j<n;j++)if(unique[j]==pads[i].addr)found=true;
        if(!found)unique[n++]=pads[i].addr;
    }
    return n+frf_tinyusb_xpad_count();
}
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
/* One physical USB address is one Amiga controller.
   A composite HID device may have independent direction and button
   interfaces; combine their states before publishing any guest register.
   First distinct device -> port 2, second -> port 1. */
static int frf_pad_device_port(uint8_t addr) {
    uint8_t unique[2]={0,0};
    unsigned count=0;
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++) {
        const uint8_t a=pads[i].addr;
        if(!a)continue;
        bool exists=false;
        for(unsigned j=0;j<count;j++)if(unique[j]==a)exists=true;
        if(exists)continue;
        if(count>=2u)break;
        unique[count++]=a;
    }
    for(unsigned j=0;j<count;j++)
        if(unique[j]==addr)return j==0u?1:0;
    return -1;
}
static void frf_pad_publish_device(uint8_t addr) {
    const int port=frf_pad_device_port(addr);
    if(port<0)return;
    uint16_t joy=0;
    uint32_t buttons=0;
    int fire=0;
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++){
        const struct pad_itf *p=&pads[i];
        if(p->addr!=addr || !p->published_valid)continue;
        joy=frf_r33_merge_joy(joy,p->published_joy);
        fire|=p->published_fire!=0;
        buttons|=p->buttons_state;
    }
    /* Primary fire and button 2/3 have independent Amiga paths. */
    aga_virtual_joy_set(port,joy,fire);
    aga_virtual_pad_buttons(port,buttons);
}
static void frf_pad_publish_report(struct pad_itf *p,uint16_t joy,
                                   int fire,uint32_t buttons) {
    p->published_joy=joy;
    p->published_fire=(uint8_t)(fire!=0);
    p->buttons_state=buttons;
    p->published_valid=1u;
    frf_pad_publish_device(p->addr);
}
void frf_pad_port_snapshot(uint32_t out[4]) {
    for(unsigned n=0;n<4u;n++)out[n]=0;
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++){
        struct pad_itf *p=&pads[i];
        if(!p->addr || !p->published_valid)continue;
        int port=frf_pad_device_port(p->addr);
        if(port<0)continue;
        out[port*2u]|=p->buttons_state;
        uint32_t old=out[port*2u+1u];
        out[port*2u+1u]=(uint32_t)frf_r33_merge_joy((uint16_t)old,p->published_joy) |
            ((old | ((uint32_t)p->published_fire<<16))&0x10000u);
    }
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
    ++r34_mount_calls;
    if(!addr || find(addr,idx)) return;
    uint16_t vid=0,pid=0;
    const bool have_vidpid=tuh_vid_pid_get(addr,&vid,&pid);
    /* A Dyonder can expose gamepad ID1 and mouse ID5 in one HID
       interface. Its boot-mouse protocol alone is NOT proof that the
       report descriptor is mouse-only. Inspect the report's top-level
       HID usage; only claim a boot-mouse interface if it also advertises
       a joystick or gamepad collection. Never claim a boot keyboard. */
    const uint8_t protocol=tuh_hid_interface_protocol(addr,idx);
    /* R30: the working USPi implementation uses the actual HID report
       descriptor for buttons, hat, and axes. Do that before raw fallback. */
    if(have_vidpid && vid==0x220du && pid==0x330cu &&
       protocol==HID_ITF_PROTOCOL_NONE) {
        const int slot=first_free();
        if(slot<0) return;
        struct pad_itf *p=&pads[slot];
        memset(p,0,sizeof(*p));
        p->addr=addr;p->idx=idx;
        frf_r32_receiver_addr=addr;
        ++r34_claimed;
        p->next_submit_ms=tusb_time_millis_api()+10u;
        if(desc && desc_len &&
           frf_hid_gamepad_parse(&p->desc_map,desc,desc_len)) {
            p->report_is_common=4u;
            p->report_id=p->desc_map.first_id;
            descriptor_fields=p->desc_map.count;
            descriptor_last_address=((uint32_t)addr<<16)|idx;
            kprintf("[FRF-R30] 220D:330C descriptor fields=%u firstID=%u\\n",
                p->desc_map.count,p->report_id);
        } else {
            /* Retain fallback only if descriptor is truly unavailable.
               R29's candidate button nibble is not validated. */
            p->report_is_common=6u;
            kprintf("[FRF-R30] 220D:330C descriptor unavailable, raw fallback\\n");
        }
        ++active_pads;
        return;
    }
    if(protocol==HID_ITF_PROTOCOL_KEYBOARD) return;
    tuh_hid_report_info_t info[8];
    uint8_t n=(desc && desc_len)?
        tuh_hid_parse_report_descriptor(info,8u,desc,desc_len):0u;
    bool gamepad_collection=false;
    for(uint8_t j=0;j<n;++j) {
        if(frf_pad_is_joystick_usage(info[j].usage_page,info[j].usage))
            gamepad_collection=true;
    }
    struct frf_hid_map parsed={0};
    const int descriptor_supported=frf_hid_gamepad_parse(&parsed,desc,desc_len);
    if(protocol==HID_ITF_PROTOCOL_MOUSE &&
       !gamepad_collection && !descriptor_supported) return;
    if(have_vidpid && frf_pad_is_dyonder(vid,pid)) {
        int slot=first_free();
        if(slot<0) return;
        struct pad_itf *p=&pads[slot];
        p->addr=addr; p->idx=idx; p->report_id=1u;
        p->report_is_common=(pid==0x0576u)?2u:3u;
        p->next_submit_ms=tusb_time_millis_api()+500u;
        ++active_pads;
        kprintf("[FRF-DYONDER] ShanWan %04x:%04x HID candidate addr=%u itf=%u mode=%u (ID1=10B, ID5 mouse)\n",
                vid,pid,addr,idx,p->report_is_common);
        return;
    }
    /* Prefer descriptor-driven real offsets, 24B or any legal report.
       This is the only generic path which may update JOY1DAT.
       No fallback uses unrelated keyboard/mouse values. */
    if(descriptor_supported) {
        int slot=first_free();
        if(slot>=0) {
            struct pad_itf *p=&pads[slot];
            p->addr=addr;p->idx=idx;p->report_is_common=4u;
            p->desc_map=parsed;p->report_id=parsed.first_id;
            p->next_submit_ms=tusb_time_millis_api()+500u;
            ++active_pads;
            descriptor_fields=parsed.count;
            descriptor_last_address=((uint32_t)addr<<16)|idx;
            kprintf("[FRF-R19] HID descriptor gamepad dev=%u interface=%u fields=%u id=%u\n",
                    addr,idx,parsed.count,parsed.first_id);
        }
        return;
    }
    /* R41: R40a fixed the big-endian HID descriptor length, so 057E:2009
       must prefer its real HID descriptor. The old early compatibility claim
       collapsed all buttons into a single Boolean fire. Only fall back to the
       legacy 24-byte profile when descriptor parsing genuinely failed. */
    if(have_vidpid && frf_r20_is_receiver(vid,pid,idx) &&
       protocol==HID_ITF_PROTOCOL_NONE) {
        const int slot=first_free();
        if(slot<0)return;
        struct pad_itf *p=&pads[slot];
        memset(p,0,sizeof(*p));
        p->addr=addr;p->idx=idx;p->report_is_common=5u;
        p->next_submit_ms=tusb_time_millis_api()+10u;
        ++active_pads;
        r20_dev=((uint32_t)addr<<16)|idx;
        frf_r26_receiver_addr=addr;
        frf_r26_receiver_idx=idx;
        kprintf("[FRF-R41] 057E:2009 descriptor unavailable; legacy 24B fallback at USB %u interface %u\n",
                addr,idx);
        return;
    }
    for(uint8_t j=0;j<n;++j) {
        if(!frf_pad_is_joystick_usage(info[j].usage_page,info[j].usage))
            continue;
        int slot=first_free();
        if(slot<0) return;
        struct pad_itf *p=&pads[slot];
        p->addr=addr; p->idx=idx; p->report_id=info[j].report_id;
        p->report_is_common=1; /* exact payload length checked on receipt */
        p->next_submit_ms=tusb_time_millis_api()+500u;
        ++active_pads;
        kprintf("[FRF-TINYUSB-PAD] HID joypad detected dev=%u itf=%u report_id=%u total=%u\\n",
                addr,idx,p->report_id,active_pads);
        return;
    }
    /* These exact receiver IDs previously appeared as game controllers in
     * the EMU68AGA USPi integration. HID descriptors can be unavailable
     * (too large, firmware quirk, or vendor-defined page). Count such an
     * interface as a known controller CANDIDATE, but NEVER assume that
     * its unknown reports use the 11-byte standard layout. */
    if(!have_vidpid || vid!=0x2dc8u) return;
    const int slot=first_free();
    if(slot<0) return;
    struct pad_itf *p=&pads[slot];
    p->addr=addr;p->idx=idx;p->report_id=0u;
    p->report_is_common=0u;
    p->next_submit_ms=tusb_time_millis_api()+500u;
    ++active_pads;
    kprintf("[FRF-TINYUSB-PAD] candidate VID=%04x PID=%04x addr=%u itf=%u; awaiting report layout\n",
           vid,pid,addr,idx);
}
void frf_tinyusb_pad_unmount(uint8_t addr,uint8_t idx)
{
    struct pad_itf *p=find(addr,idx);
    if(!p)return;
    const int oldport=frf_pad_device_port(addr);
    bool had_other=false;
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++)
        if(pads[i].addr && pads[i].addr!=addr)had_other=true;
    if(p->report_is_common==5u && frf_r26_receiver_addr==addr &&
       frf_r26_receiver_idx==idx) {
        frf_r26_receiver_addr=0;
        frf_r26_receiver_idx=0;
    }
    if((p->report_is_common==4u || p->report_is_common==6u) &&
        frf_r32_receiver_addr==addr)
        frf_r32_receiver_addr=0;
    memset(p,0,sizeof(*p));
    if(active_pads)--active_pads;
    /* Disconnect releases direction, CIA fire and both POTGOR buttons. */
    if(oldport>=0){
        aga_virtual_joy_set(oldport,0,0);
        aga_virtual_pad_buttons(oldport,0);
    }
    if(!active_pads)frf_r32_last_buttons=0;
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++){
        if(pads[i].addr && pads[i].published_valid)
            frf_pad_publish_device(pads[i].addr);
    }
    /* A second device may have moved from port 1 to port 2. */
    if(oldport==1 && had_other && active_pads){
        aga_virtual_joy_set(0,0,0);
        aga_virtual_pad_buttons(0,0);
    }
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
/* Exact legacy USPi ShanWan 2563:0576 191-byte descriptor decoding:
 * report 01 [2 button bytes] [hat 0..8] [X,Y,Rx,Ry 0..255] [2 vendor].
 * ID05 is a relative mouse report, ID02 is output; NEVER joystick input.
 * Return 1 only for a verified 10-byte ID1 report. */
static int frf_dyonder_extract(const uint8_t *b,uint16_t len,
                               uint16_t *out_joy,uint32_t *out_buttons)
{
    if(!b || len!=10u || b[0]!=1u) return 0;
    const uint32_t buttons=(uint32_t)b[1] |
                            ((uint32_t)(b[2]&0x7fu)<<8);
    const uint8_t h=b[3]&15u;
    const int x=(int)b[4]-128, y=(int)b[5]-128;
    const int left=(x < -36) || h==5 || h==6 || h==7;
    const int right=(x > 36) || h==1 || h==2 || h==3;
    const int up=(y < -36) || h==0 || h==1 || h==7;
    const int down=(y > 36) || h==3 || h==4 || h==5;
    const uint16_t joy=(uint16_t)((left?0x0200u:0u) |
        (((left!=0)^(up!=0))?0x0100u:0u) |
        (right?0x0002u:0u) |
        (((right!=0)^(down!=0))?0x0001u:0u));
    if(!out_joy || !out_buttons)return 0;
    *out_joy=joy;
    *out_buttons=buttons;
    return 1;
}
int frf_dyonder_decode_report(const uint8_t *data,uint16_t len)
{
    uint16_t joy=0;uint32_t buttons=0;
    if(!frf_dyonder_extract(data,len,&joy,&buttons))return 0;
    /* Working USPi pad_status: only physical button 1 is primary fire. */
    aga_virtual_joy_set(1,joy,(buttons&1u)!=0u);
    aga_virtual_pad_buttons(1,buttons);
    return 1;
}
/* Return true only for an accepted report from the exact hardware
   candidate. Attach lazily after observing a specific real neutral
   24-byte fingerprint, not merely from an untrusted HID mount label.
   The original HID class driver stays mounted and feeds reports. */
int frf_r27_receive(uint8_t addr,uint8_t idx,const uint8_t *data,uint16_t len)
{
    struct pad_itf *p=find(addr,idx);
    if(!p || p->report_is_common!=6u)return 0;
    ++r27_seen;
    uint16_t joy=0;
    int fire=0;
    if(frf_r27_decode(data,len,&joy,&fire)) {
        p->joy_report_state=joy;
        p->fire_report_state=(uint8_t)(fire!=0);
    } else if(frf_r33_id02(data,len,&joy)) {
        /* ID02 physically observed on R28. Never clear ID01 fire. */
        p->joy_id02_state=joy;
        p->id02_seen=1;
    } else if(frf_r37_id03(data,len,&joy)) {
        p->joy_id03_state=joy;
        p->id03_seen=1;
    } else {
        ++r27_bad;
        return 1;
    }
    ++r27_ok;
    r27_joy=frf_r33_merge_joy(p->joy_report_state,frf_r33_merge_joy(p->joy_id02_state,p->joy_id03_state));
    r27_fire=p->fire_report_state;
    frf_pad_publish_report(p,(uint16_t)r27_joy,(int)r27_fire,p->buttons_state);
    ++gamepad_reports;
    ++p->reports;
    return 1;
}

int frf_r20_compat_receive(uint8_t addr,uint8_t idx,
                            const uint8_t *data,uint16_t len)
{
    if(!data || len!=24u) return 0;
    struct pad_itf *p=find(addr,idx);
    if(!p || p->report_is_common!=5u) {
        uint16_t vid=0,pid=0;
        if(!tuh_vid_pid_get(addr,&vid,&pid) ||
           !frf_r20_is_receiver(vid,pid,idx)) return 0;
        /* R21 fallback: any complete 24-byte report from exact VID:PID
           and interface identifies the mounted receiver. Active sticks
           and pressed buttons must not prevent initial detection. */
        if(tuh_hid_interface_protocol(addr,idx)!=HID_ITF_PROTOCOL_NONE)
            return 0;
        if(p) return 0; /* Another genuine HID pad owns this interface. */
        const int slot=first_free();
        if(slot<0) return 0;
        p=&pads[slot];
        memset(p,0,sizeof(*p));
        p->addr=addr;p->idx=idx;p->report_is_common=5u;
        p->next_submit_ms=tusb_time_millis_api()+10u;
        ++active_pads;
        r20_dev=((uint32_t)addr<<16)|idx;
        frf_r26_receiver_addr=addr;
        frf_r26_receiver_idx=idx;
        kprintf("[FRF-R39] 057E:2009 observed 24B controller candidate at USB %u HID %u\n",addr,idx);
    }
    ++r20_seen;
    uint16_t joy=0;
    int fire=0;
    if(frf_r20_decode(data,len,&joy,&fire)) {
        ++r20_valid;
        r20_joy=joy;r20_fire=(uint32_t)fire;
        frf_pad_publish_report(p,joy,fire,(uint32_t)(fire!=0));
        ++gamepad_reports;
        ++p->reports;
    } else ++r20_rejected;
    return 1;
}
int frf_tinyusb_pad_port2_owner(uint8_t addr,uint8_t idx){
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++)
        if(pads[i].addr)return pads[i].addr==addr && pads[i].idx==idx;
    return 0;
}
int frf_tinyusb_pad_has(uint8_t addr,uint8_t idx){return find(addr,idx)!=0;}
int frf_r30_descriptor_receiver(uint8_t addr,uint8_t idx){struct pad_itf *p=find(addr,idx);return p && p->report_is_common==4u;}
void frf_tinyusb_pad_report(uint8_t addr,uint8_t idx,
                            const uint8_t *data,uint16_t len)
{
    struct pad_itf *p=find(addr,idx);
    if(!p || !data) return;
    if(p->report_is_common==4u) {
        ++descriptor_packets;
        uint16_t joy=0;int fire=0;
        if(frf_hid_gamepad_decode(&p->desc_map,data,len,&joy,&fire)) {
            const unsigned caps=frf_hid_gamepad_report_capabilities(
                &p->desc_map,p->desc_map.has_ids?data[0]:0u);
            if(caps&1u){
                unsigned rid=p->desc_map.has_ids?(unsigned)data[0]:0u;
                if(rid<4u)p->joy_by_report[rid]=joy;
                else p->joy_report_state=joy;
            }
            uint32_t state=0,present=0;
            if(frf_hid_gamepad_decode_buttons(&p->desc_map,data,len,&state,&present)) {
              p->buttons_state=(p->buttons_state&~present)|(state&present);
              p->fire_report_state=(uint8_t)((p->buttons_state&1u)!=0u);
              frf_r32_last_buttons=p->buttons_state;
            }
            joy=p->joy_report_state;
            for(unsigned rid=0;rid<4u;rid++)
                joy=frf_r33_merge_joy(joy,p->joy_by_report[rid]);
            fire=p->fire_report_state;
            frf_pad_publish_report(p,joy,fire,p->buttons_state);
            descriptor_joy=joy;
            descriptor_fire=(uint32_t)fire;
            ++descriptor_valid;
            ++p->reports;
            ++gamepad_reports;
        }
        return;
    }
    if(p->report_is_common==2u || p->report_is_common==3u) {
        /* Keep HID report ID in-place. The old driver validated length=10
           including ID1; stripping it silently broke the R13 decoder.
           0575 is mode-switch capable: promote only after a real ID1. */
        uint16_t decoded_joy=0;uint32_t decoded_buttons=0;
        if(frf_dyonder_extract(data,len,&decoded_joy,&decoded_buttons)) {
            /* Same state transfer as working USPi: axes/hat + 15 buttons
               are stored on the owning physical controller before publishing. */
            frf_pad_publish_report(p,decoded_joy,
                                   (decoded_buttons&1u)!=0u,decoded_buttons);
            if(p->report_is_common==3u) {
                p->report_is_common=2u;
                kprintf("[FRF-DYONDER] 0575 mode supplied valid 10B ID1 pad input\n");
            }
            ++p->reports;
            ++gamepad_reports;
            ++dyonder_report_ok;
        } else ++dyonder_report_ignore;
        return;
    }
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
            if(&pads[i]==p && p->report_is_common && len==11u)
                map_standard_report(data);
            break;
        }
}

/* R14: port known-working USPi RC9 deferred interrupt-IN recovery.
 * TinyUSB's HID mount callback can fail its one initial channel submission;
 * merely having a mounted HID descriptor does not mean we are receiving
 * reports. CPU1 periodically retries detected gamepads without touching
 * keyboard/mouse USB endpoints or reenumbering the USB root hub.
 * Never submit to an already-busy endpoint. */
void frf_tinyusb_pad_retry_task(void)
{
    const uint32_t now=tusb_time_millis_api();
    for(unsigned i=0;i<FRF_PAD_SLOTS;i++) {
        struct pad_itf *p=&pads[i];
        if(!p->addr || (int32_t)(now-p->next_submit_ms)<0) continue;
        /* Only exact R21 HID receiver is paced at 100Hz to prevent
           its interrupt-IN reports starving genuine keyboard releases. */
        p->next_submit_ms=now+((p->report_is_common==5u || p->report_is_common==6u ||
            (p->report_is_common==4u && p->desc_map.valid))?10u:500u);
        ++pt; if(!tuh_hid_receive_ready(p->addr,p->idx)){++nr;continue;}
        if(!tuh_hid_receive_report(p->addr,p->idx)) {
            ++sf; ++p->retries;
            ++pad_submit_deferred;
        } else { ++so; if(p->retries) {
            ++p->recoveries;
            ++pad_submit_recovered;
            p->retries=0;
        }}
    }
}
