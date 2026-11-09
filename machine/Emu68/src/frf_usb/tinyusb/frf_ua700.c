/* SPDX-License-Identifier: GPL-2.0-only
 * EDIROL UA-700 0582:002b Advanced-mode receive transport.
 * EXP01 only: no AHI/CAMD guest bridge and no audio streaming.
 */
#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
extern void kprintf(const char *, ...);
#define MIDI_RING 256u
typedef struct { uint8_t b[4]; } ua_packet_t;
static ua_packet_t midi_ring[MIDI_RING];
static volatile uint32_t wr_seq,rd_seq,rx_packets,rx_failures,rx_overruns;
static volatile uint32_t cable0_packets,cable1_packets,other_packets,last_packet;
static uint8_t dev_addr,midi_itf,ep_in,ep_out;
static bool midi_ready;
/* Enumeration/transfer stage diagnostics exported at UAM1 indices 13..24. */
static volatile uint32_t diag_seen,diag_vidpid,diag_itf,diag_open_attempts;
static volatile uint32_t diag_claims,diag_set_queue,diag_set_success;
static volatile uint32_t diag_rx_arm,diag_rx_complete,diag_rx_bytes,diag_rx_bad;
static volatile uint32_t diag_last_result,diag_set_calls,diag_set_failed;

CFG_TUH_MEM_SECTION CFG_TUH_MEM_ALIGN static uint8_t rx_usb[64];
static bool ua_match(uint8_t addr) {
    uint16_t vid=0,pid=0;
    return tuh_vid_pid_get(addr,&vid,&pid) && vid==0x0582u && pid==0x002bu;
}
bool frf_ua_init(void) {
    dev_addr=0;midi_ready=false;ep_in=ep_out=0;
    wr_seq=rd_seq=rx_packets=rx_failures=rx_overruns=0;
    cable0_packets=cable1_packets=other_packets=last_packet=0;
    diag_seen=diag_vidpid=diag_itf=diag_open_attempts=0;
    diag_claims=diag_set_queue=diag_set_success=0;
    diag_rx_arm=diag_rx_complete=diag_rx_bytes=diag_rx_bad=diag_last_result=0;
    diag_set_calls=diag_set_failed=0;
    return true;
}
bool frf_ua_deinit(void) {return true;}
/* TinyUSB's tu_bind_driver_to_ep_itf binds the interface driver ONLY
 * from alternate setting zero. Claim alt0, include the alt1 descriptor
 * and endpoints in the consumed span, then SET_INTERFACE(3,1) in
 * set_config before arming interrupt/bulk IN. */
uint16_t frf_ua_open(uint8_t rhport,uint8_t addr,
                     const tusb_desc_interface_t *itf,uint16_t max_len)
{
    (void)rhport;
    uint16_t vid=0,pid=0;
    if(tuh_vid_pid_get(addr,&vid,&pid) && vid==0x0582u &&
       (pid==0x002bu || pid==0x002cu)) {
        diag_seen++;
        diag_vidpid=((uint32_t)vid<<16)|pid;
        diag_itf=((uint32_t)itf->bInterfaceNumber<<24)|
          ((uint32_t)itf->bAlternateSetting<<16)|
          ((uint32_t)itf->bInterfaceClass<<8)|itf->bNumEndpoints;
    }
    if(!ua_match(addr) || itf->bInterfaceNumber!=3u ||
       itf->bAlternateSetting!=0u || itf->bInterfaceClass!=0xffu)
        return 0;
    diag_open_attempts++;

    const uint8_t *base=(const uint8_t*)itf;
    uint16_t used=0;
    bool alt1=false;
    const tusb_desc_endpoint_t *eps[2]={0,0};
    uint8_t count=0,in=0,out=0;
    uint32_t error=0;
    while((uint32_t)used+2u<=max_len){
        const uint8_t *d=base+used;
        uint8_t len=d[0],type=d[1];
        if(len<2u || (uint32_t)used+len>max_len){error=0xE2u;break;}
        if(type==TUSB_DESC_INTERFACE){
            if(len<sizeof(tusb_desc_interface_t)){error=0xE4u;break;}
            const tusb_desc_interface_t *candidate=(const tusb_desc_interface_t*)d;
            if(candidate->bInterfaceNumber!=3u)break;
            if(candidate->bAlternateSetting==1u){
                alt1=true;
                if(candidate->bNumEndpoints!=2u){error=0xE5u;break;}
            }else if(candidate->bAlternateSetting!=0u){
                break;
            }
        } else if(type==TUSB_DESC_ENDPOINT && alt1){
            if(len<sizeof(tusb_desc_endpoint_t)){error=0xE6u;break;}
            if(count>=2u){error=0xE7u;break;}
            const tusb_desc_endpoint_t *ep=(const tusb_desc_endpoint_t*)d;
            if(ep->bEndpointAddress&0x80u){
                if(ep->bmAttributes.xfer!=TUSB_XFER_INTERRUPT &&
                   ep->bmAttributes.xfer!=TUSB_XFER_BULK){error=0xE8u;break;}
                in=ep->bEndpointAddress;
            }else{
                if(ep->bmAttributes.xfer!=TUSB_XFER_BULK){error=0xE9u;break;}
                out=ep->bEndpointAddress;
            }
            eps[count++]=ep;
        }
        used=(uint16_t)(used+len);
        /* Stop at the end of both alt1 endpoint descriptors, but
         * preserve intervening class-specific descriptors when present. */
        if(alt1 && count==2u)break;
    }
    if(error || !alt1 || count!=2u || in!=0x84u || out!=0x03u){
        diag_last_result=error?error:0xEAu;
        kprintf("[UA700] alt0/alt1 claim rejected reason=%x alt1=%u endpoints=%u in=%x out=%x span=%u\n",
             diag_last_result,(unsigned)alt1,count,in,out,used);
        return 0;
    }
    for(uint8_t i=0;i<2u;i++){
        if(!tuh_edpt_open(addr,eps[i])){
            diag_last_result=0xEBu+i;
            kprintf("[UA700] endpoint open failed ep=%x\n",eps[i]->bEndpointAddress);
            return 0;
        }
    }
    diag_claims++;
    dev_addr=addr;midi_itf=3;ep_in=in;ep_out=out;midi_ready=false;
    kprintf("[UA700] bound interface3 alt0, MIDI alt1 in=%02x out=%02x\n",in,out);
    return used;
}
static bool ua_start_rx(void) {
    if(!midi_ready || !dev_addr || !ep_in)return false;
    bool submitted=usbh_edpt_xfer(dev_addr,ep_in,rx_usb,sizeof(rx_usb));
    if(submitted)diag_rx_arm++;
    return submitted;
}
static void ua_alt_ready(tuh_xfer_t *x)
{
    if(x->daddr!=dev_addr) return;
    if(x->result!=XFER_RESULT_SUCCESS) {
        diag_set_failed++;
        rx_failures++;
        kprintf("[UA700] ERROR: SET_INTERFACE alt1 failed\n");
        usbh_driver_set_config_complete(dev_addr,midi_itf);
        return;
    }
    midi_ready=true;
    diag_set_success++;
    kprintf("[UA700] interface3 alternate1 ready, MIDI receive active\n");
    if(!ua_start_rx()) {
        rx_failures++;
        kprintf("[UA700] ERROR: MIDI endpoint receive arm failed\n");
    }
    usbh_driver_set_config_complete(dev_addr,midi_itf);
}
bool frf_ua_set_config(uint8_t addr,uint8_t itf){
    diag_set_calls++;
    if(addr!=dev_addr || itf!=midi_itf)return false;
    /* Linux snd-usb-audio selects interface 3 alternate setting 1.
     * TinyUSB must explicitly SET_INTERFACE before reading endpoint 0x84. */
    if(!tuh_interface_set(addr,itf,1,ua_alt_ready,0)) {
        rx_failures++;
        kprintf("[UA700] ERROR: SET_INTERFACE alt1 queue failed\n");
        usbh_driver_set_config_complete(addr,itf);
    } else diag_set_queue++;
    return true;
}
bool frf_ua_xfer(uint8_t addr,uint8_t ep,xfer_result_t result,uint32_t len){
    if(addr!=dev_addr || ep!=ep_in)return false;
    diag_last_result=(uint32_t)result;
    if(result==XFER_RESULT_SUCCESS){
        diag_rx_complete++;
        diag_rx_bytes+=len;
        for(uint32_t pos=0;pos+3u<len;pos+=4u){
            if(wr_seq-rd_seq>=MIDI_RING){rx_overruns++;break;}
            ua_packet_t *pkt=&midi_ring[wr_seq & (MIDI_RING-1u)];
            pkt->b[0]=rx_usb[pos];pkt->b[1]=rx_usb[pos+1u];
            pkt->b[2]=rx_usb[pos+2u];pkt->b[3]=rx_usb[pos+3u];
            __asm__ volatile("dmb ishst" ::: "memory");
            {
                uint32_t raw=((uint32_t)pkt->b[0]<<24)|((uint32_t)pkt->b[1]<<16)|((uint32_t)pkt->b[2]<<8)|pkt->b[3];
                uint8_t cable=(uint8_t)(pkt->b[0]>>4);
                if(cable==0u) cable0_packets++;
                else if(cable==1u)cable1_packets++;
                else other_packets++;
                last_packet=raw;
            }
            wr_seq++;rx_packets++;
        }
    } else {rx_failures++;diag_rx_bad++;}
    if(!ua_start_rx()) rx_failures++;
    return true;
}
void frf_ua_close(uint8_t addr){
    if(addr==dev_addr){midi_ready=false;dev_addr=0;ep_in=ep_out=0;}
}
int frf_ua700_midi_pop(uint8_t out[4]){
    if(rd_seq==wr_seq)return 0;
    const ua_packet_t *p=&midi_ring[rd_seq & (MIDI_RING-1u)];
    out[0]=p->b[0];out[1]=p->b[1];out[2]=p->b[2];out[3]=p->b[3];
    rd_seq++;return 1;
}
/* Native TinyUSB standard USB-MIDI class data is forwarded to same
 * guest-side UAM1 queue as UA-700 advanced-mode packets. */
static volatile uint32_t generic_mounts,generic_packets,generic_addr;
void frf_ua700_generic_mount(uint8_t daddr)
{
    generic_addr=daddr;
    generic_mounts++;
}
void frf_ua700_generic_unmount(uint8_t daddr)
{
    if(generic_addr==daddr)generic_addr=0;
}
void frf_ua700_generic_receive(const uint8_t packet[4])
{
    if(wr_seq-rd_seq>=MIDI_RING) {rx_overruns++;return;}
    ua_packet_t *pkt=&midi_ring[wr_seq & (MIDI_RING-1u)];
    pkt->b[0]=packet[0]; pkt->b[1]=packet[1];
    pkt->b[2]=packet[2]; pkt->b[3]=packet[3];
    uint8_t cable=(uint8_t)(packet[0]>>4);
    if(cable==0) cable0_packets++;
    else if(cable==1) cable1_packets++;
    else other_packets++;
    last_packet=((uint32_t)packet[0]<<24)|((uint32_t)packet[1]<<16)|
      ((uint32_t)packet[2]<<8)|packet[3];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr_seq++;rx_packets++;generic_packets++;
}
void frf_ua700_stats(uint32_t out[5]){
    out[0]=dev_addr;out[1]=rx_packets;out[2]=rx_failures;
    out[3]=rx_overruns;out[4]=wr_seq-rd_seq;
}

/* Guest-visible MIDI mailbox on unused 64K FRUM Z2 board at offset 0x4000.
 * Host publishes producer sequence only after storing packet; consumer ACKs.
 * DO NOT alter the usbmass v1 ABI. */
#define UA_GUEST_OFF 0x4000u
#define UA_GUEST_MAGIC 0x55414d31u /* UAM1 */
#define UA_GUEST_Q 128u
extern volatile struct frf_um_shared *frf_um_host;
#include "../frf_usb_mass.h"
void frf_ua700_guest_service(void)
{
    volatile struct frf_um_shared *um =
        __atomic_load_n(&frf_um_host,__ATOMIC_ACQUIRE);
    if(!um || um->magic!=FRF_UM_MAGIC) return;
    volatile uint32_t *m=(volatile uint32_t*)((volatile uint8_t*)um+UA_GUEST_OFF);
    if(m[0]!=UA_GUEST_MAGIC) {
        m[0]=0; m[1]=1u; m[2]=0u; m[3]=0u;
        m[4]=0u; m[5]=0u;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        m[0]=UA_GUEST_MAGIC;
    }
    m[4]=(uint32_t)(dev_addr?dev_addr:generic_addr);
    m[5]=rx_failures;
    m[6]=cable0_packets;
    m[7]=cable1_packets;
    m[8]=other_packets;
    m[9]=last_packet;
    m[10]=rx_overruns;
    m[11]=(uint32_t)midi_ready;
    m[12]=rx_packets;
    m[25+UA_GUEST_Q]=generic_mounts;
    m[26+UA_GUEST_Q]=generic_packets;
    m[27+UA_GUEST_Q]=generic_addr;
    m[28+UA_GUEST_Q]=diag_set_calls;
    m[29+UA_GUEST_Q]=diag_set_failed;
    m[13]=diag_seen; m[14]=diag_vidpid; m[15]=diag_itf;
    m[16+UA_GUEST_Q]=diag_open_attempts;
    m[17+UA_GUEST_Q]=diag_claims;
    m[18+UA_GUEST_Q]=diag_set_queue;
    m[19+UA_GUEST_Q]=diag_set_success;
    m[20+UA_GUEST_Q]=diag_rx_arm;
    m[21+UA_GUEST_Q]=diag_rx_complete;
    m[22+UA_GUEST_Q]=diag_rx_bytes;
    m[23+UA_GUEST_Q]=diag_rx_bad;
    m[24+UA_GUEST_Q]=diag_last_result;
    while(rd_seq!=wr_seq) {
        uint32_t prod=m[2],cons=m[3];
        if(prod-cons>=UA_GUEST_Q) return; /* guest must drain */
        const ua_packet_t *pkt=&midi_ring[rd_seq&(MIDI_RING-1u)];
        m[16u+(prod&(UA_GUEST_Q-1u))]=
          ((uint32_t)pkt->b[0]<<24)|((uint32_t)pkt->b[1]<<16)|
          ((uint32_t)pkt->b[2]<<8)|pkt->b[3];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        m[2]=prod+1u;
        rd_seq++;
    }
}
