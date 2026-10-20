/* SPDX-License-Identifier: GPL-2.0-only
 * Entire Pi3 HS hub -> full-speed HID keyboard enumeration simulated
 * using USB packet-level HCD mock. No physical hardware claim.
 */
#include "../src/frf_usb/native/frf_usb_native_hid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
volatile uint8_t frf_usb_mouse_present,frf_usb_mouse_seen,frf_usb_mouse_buttons;
static unsigned key_reports, mouse_reports,split_packets, hub_port_power;
static unsigned reset_port, setup_index, configured_root,configured_child;
static uint8_t current_setup[8];
static unsigned data_offset, active_child;
void frf_native_usb_keyboard_boot_report(const uint8_t report[8])
{
 assert(report[2]==4);key_reports++;
}
void aga_virtual_mouse_update(int dx,int dy,unsigned buttons)
{
 (void)dx;(void)dy;(void)buttons;mouse_reports++;
}
static const uint8_t root_dev[18]={
 18,1,0,2,9,0,2,64,0x24,0x04,0x14,0x95,0,1,1,2,0,1
};
static const uint8_t child_dev[18]={
 18,1,0,2,0,0,0,8,0x34,0x12,0x78,0x56,0,1,1,2,0,1
};
static const uint8_t root_cfg[25]={
 9,2,25,0,1,1,0,0x80,50,
 9,4,0,0,1,9,0,2,0,
 7,5,0x81,3,1,0,12
};
static const uint8_t child_cfg[34]={
 9,2,34,0,1,1,0,0x80,50,
 9,4,0,0,1,3,1,1,0,
 9,0x21,0x11,1,0,1,0x22,0x3f,0,
 7,5,0x81,3,8,0,10
};
static const uint8_t hubdesc[9]={9,0x29,4,0,0,10,0,0,0xff};
static void fake_delay(void *ctx,unsigned us) {(void)ctx;(void)us;}
int frf_dwc2_pio_packet(const struct frf_dwc2_ops *o,
                       struct frf_dwc2_packet *p,
                       struct frf_dwc2_pio_stats *st)
{
 (void)o;assert(p); if(st)st->attempts++;
 p->received=0;
 if(p->hub_address) {
    assert(p->hub_address==1&&p->hub_port==1);
    split_packets++;
 }
 if(p->type==3) {
    assert(p->address==2&&p->in&&p->hub_address==1&&p->hub_port==1);
    const uint8_t report[8]={0,0,4,0,0,0,0,0};
    assert(p->length>=8);
    memcpy(p->buffer,report,8); p->received=8;
    return 0;
 }
 assert(p->type==0);
 if(!p->in&&p->pid==FRF_USB_PID_SETUP&&p->length==8){
     memcpy(current_setup,p->buffer,8);
     setup_index++; data_offset=0;
     active_child=p->hub_address!=0;
     return 0;
 }
 uint8_t req=current_setup[1];
 if(p->in&&p->length) {
    const uint8_t *src=NULL;unsigned length=0;
    if(req==6) {
       if(current_setup[3]==1){src=active_child?child_dev:root_dev;length=18;}
       if(current_setup[3]==2){src=active_child?child_cfg:root_cfg;
                               length=active_child?34:25;}
       if(current_setup[3]==0x29 || (current_setup[3]==0 && current_setup[0]==0xa0)){assert(!active_child);
                                  src=hubdesc;length=9;}
    } else if(req==0&&current_setup[0]==0xa3) {
       static uint8_t port[4]={0};
       unsigned n=current_setup[4];
       assert(n>=1 && n<=4);
       unsigned val=n==1?((reset_port?3u:1u)):0u;
       port[0]=(uint8_t)val;port[1]=port[2]=port[3]=0;
       src=port;length=4;
    }
    if(!src || length<data_offset) printf("missing src req=%u bm=%02x descriptor=%02x port=%u address=%u split=%u/%u offset=%u\n",req,current_setup[0],current_setup[3],current_setup[4],p->address,p->hub_address,p->hub_port,data_offset);
    assert(src&&length>=data_offset);
    unsigned available=length-data_offset;
    unsigned take=p->length<available?p->length:available;
    memcpy(p->buffer,src+data_offset,take);
    data_offset+=take;p->received=(uint16_t)take;
 } else if(p->length==0){
    if(req==9){if(active_child)configured_child++;else configured_root++;}
    if(req==3&&current_setup[0]==0x23){
       if(current_setup[2]==8)hub_port_power++;
       if(current_setup[2]==4)reset_port=current_setup[4];
    }
 }
 if(st)st->completed++;
 return 0;
}
int main(void) {
 struct frf_native_hid h={0};
 struct frf_dwc2_ops o={NULL,NULL,fake_delay,NULL};
 int rc=frf_native_hid_enumerate(&o,&h,0);
 if(rc)printf("enumeration returned %d setup_count=%u route=%u/%u child_error=%d ports=%u\n",
             rc,setup_index,h.hub_address,h.hub_port,h.port_error,h.ports_seen);
 assert(rc==0);
 assert(h.root_hub_ports==4);
 assert(h.have_keyboard&&h.keyboard.device_address==2);
 assert(h.keyboard.hub_address==1&&h.keyboard.hub_port==1);
 assert(hub_port_power==4&&reset_port==1);
 assert(configured_root==1&&configured_child==1);
 frf_native_hid_poll(&o,&h);
 assert(key_reports==1&&split_packets>0);
 printf("Hub TT FS keyboard simulated enumeration PASS: setup=%u split=%u report=%u\n",
        setup_index,split_packets,key_reports);
 return 0;
}
