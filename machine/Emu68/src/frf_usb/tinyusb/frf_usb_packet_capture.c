/* SPDX-License-Identifier: GPL-2.0-only
 * Passive packet fingerprint capture for Pi3 ARM64 big-endian TinyUSB.
 * No guest input emitted; only actual USB report bytes are recorded.
 */
#include <stdint.h>
#include <string.h>
#define N 64u
struct capture {
    uint32_t received,changed,last_changed,signature;
    uint8_t addr,if_num,len;
    uint8_t base[N],now[N],previous[N];
};
static struct capture cap[2];

/* R35: real 220D:330C HID reports must never share a baseline with
   another ID or a vendor endpoint. Keep an independent capture per ID. */
static struct capture target_id[4];
static uint8_t target_focus;
static uint32_t target_packets;
void frf_usb_target_capture(uint8_t addr,uint8_t idx,
                            const uint8_t *data,uint16_t length)
{
    if (!data || length==0u || length>64u) return;
    const unsigned id=data[0];
    if(id==0u || id>3u) return;
    struct capture *c=&target_id[id];
    const unsigned len=(unsigned)length;
    if(!c->received || c->addr!=addr || c->if_num!=idx || c->len!=len) {
        memset(c,0,sizeof(*c));
        memcpy(c->base,data,len);
        memcpy(c->previous,data,len);
        target_focus=(uint8_t)id;
    } else {
        unsigned changes=0;
        for(unsigned i=0;i<len;++i)
            if(c->previous[i]!=data[i])++changes;
        c->last_changed=changes;
        if(changes) {
            ++c->changed;
            target_focus=(uint8_t)id;
        }
    }
    memcpy(c->now,data,len);
    memcpy(c->previous,data,len);
    c->addr=addr;c->if_num=idx;c->len=(uint8_t)len;
    c->signature=((uint32_t)len<<8)|(uint32_t)id;
    ++c->received;
    ++target_packets;
}
uint32_t frf_usb_target_rx_count(void){return target_packets;}


void frf_usb_packet_capture(uint8_t kind,uint8_t addr,uint8_t interface_or_ep,
                             const uint8_t *data,uint16_t length)
{
    if(kind!=1u && kind!=2u) return;
    if(!data && length) return;
    struct capture *c=&cap[kind-1u];
    const uint8_t len=(uint8_t)((length>N)?N:length);
    const uint32_t sig=((uint32_t)length<<8) | (length?data[0]:0u);
    if(c->received==0u || c->signature!=sig || c->addr!=addr ||
       c->if_num!=interface_or_ep) {
        memset(c->base,0,sizeof(c->base));
        memset(c->previous,0,sizeof(c->previous));
        if(len) {
            memcpy(c->base,data,len);
            memcpy(c->previous,data,len);
        }
        c->signature=sig;
    } else {
        c->last_changed=0u;
        for(unsigned i=0;i<len;++i)
            if(c->previous[i]!=data[i]) ++c->last_changed;
        if(c->last_changed) ++c->changed;
        memset(c->previous,0,sizeof(c->previous));
        if(len) memcpy(c->previous,data,len);
    }
    memset(c->now,0,sizeof(c->now));
    if(len) memcpy(c->now,data,len);
    c->len=len; c->addr=addr; c->if_num=interface_or_ep;
    ++c->received;
}
static uint32_t word4(const uint8_t*data,unsigned w)
{
    const unsigned pos=w*4u;
    return ((uint32_t)data[pos]<<24)|((uint32_t)data[pos+1]<<16)|
        ((uint32_t)data[pos+2]<<8)|(uint32_t)data[pos+3];
}
/* 7 fields + 16x3: base/current/XOR at [7+3*i+j], all in BIG
 * numeral hex for direct left-to-right screenshots on big-endian ARM.
 */
void frf_usb_packet_capture_snapshot(uint32_t out[55])
{
    if(!out) return;
    const struct capture *c;
    if(target_focus && target_id[target_focus].received){
        c=&target_id[target_focus];
        out[0]=3u; /* observed exact-VID gamepad, independent report ID */
    } else {
        c=cap[1].received?&cap[1]:&cap[0];
        out[0]=cap[1].received?2u:(cap[0].received?1u:0u);
    }
    out[1]=c->received;
    out[2]=c->changed;
    out[3]=((uint32_t)c->addr<<24)|((uint32_t)c->if_num<<16)|
            ((uint32_t)c->len<<8)|(c->signature&0xffu);
    out[4]=c->last_changed;
    out[5]=cap[0].received;
    out[6]=cap[1].received;
    for(unsigned i=0;i<16u;++i) {
        const uint32_t base=word4(c->base,i),now=word4(c->now,i);
        out[7u+i*3u]=base;
        out[7u+i*3u+1u]=now;
        out[7u+i*3u+2u]=base^now;
    }
}
