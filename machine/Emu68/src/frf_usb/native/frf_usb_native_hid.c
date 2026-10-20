/* SPDX-License-Identifier: GPL-2.0-only
 * Independent direct-root HID boot input. NO hub/TT split support yet.
 * Genuine device/configuration descriptors and HID interrupt-IN reports.
 */
#include "frf_usb_native_hid.h"
#include <string.h>
extern void frf_native_usb_keyboard_boot_report(const uint8_t report[8]);
extern void aga_virtual_mouse_update(int dx,int dy,unsigned buttons);
extern volatile uint8_t frf_usb_mouse_present;
extern volatile uint8_t frf_usb_mouse_seen;
extern volatile uint8_t frf_usb_mouse_buttons;
static int submit(const struct frf_dwc2_ops *o,struct frf_native_hid *h,
                  uint8_t addr,uint8_t in,uint8_t pid,uint8_t type,
                  uint8_t ep,uint16_t mps,uint8_t *data,uint16_t len)
{
 struct frf_dwc2_packet p={addr,ep,in,type,0,pid,mps,len,0,data};
 int rc=frf_dwc2_pio_packet(o,&p,&h->stats);
 return rc ? rc : (int)p.received;
}
static int control(const struct frf_dwc2_ops *o,struct frf_native_hid *h,
                   uint8_t addr,const uint8_t setup[8],
                   uint8_t *data,uint16_t n)
{
 uint8_t packet[8];
 memcpy(packet,setup,8);
 int rc=submit(o,h,addr,0,FRF_USB_PID_SETUP,0,0,h->mps,packet,8);
 if(rc<0) return rc;
 const int has_in=(setup[0]&0x80u)!=0;
 uint16_t transferred=0;
 uint8_t pid=FRF_USB_PID_DATA1;
 while(transferred<n) {
    uint16_t count=n-transferred;
    if(count>h->mps) count=h->mps;
    rc=submit(o,h,addr,(uint8_t)has_in,pid,0,0,h->mps,data+transferred,count);
    if(rc<0) return rc;
    if(has_in) {
        transferred+=(uint16_t)rc;
        if((uint16_t)rc<count) break;
    } else transferred+=count;
    pid=pid==FRF_USB_PID_DATA1?FRF_USB_PID_DATA0:FRF_USB_PID_DATA1;
 }
 rc=submit(o,h,addr,(uint8_t)!has_in,FRF_USB_PID_DATA1,0,0,h->mps,NULL,0);
 return rc<0?rc:(int)transferred;
}
static int get_desc(const struct frf_dwc2_ops *o,struct frf_native_hid *h,
                    uint8_t addr,uint8_t type,uint8_t *data,uint16_t len)
{
 uint8_t setup[8]={0x80,6,0,type,0,0,(uint8_t)len,(uint8_t)(len>>8)};
 return control(o,h,addr,setup,data,len);
}
static int set_request(const struct frf_dwc2_ops *o,struct frf_native_hid *h,
                       uint8_t addr,uint8_t bm,uint8_t request,
                       uint16_t value,uint16_t index)
{
 uint8_t setup[8]={bm,request,(uint8_t)value,(uint8_t)(value>>8),
                  (uint8_t)index,(uint8_t)(index>>8),0,0};
 return control(o,h,addr,setup,NULL,0);
}
int frf_native_hid_enumerate(const struct frf_dwc2_ops *o,
                             struct frf_native_hid *h)
{
 if(!o||!h) return -1;
 memset(h,0,sizeof(*h));
 h->mps=8u;
 uint8_t dev[18]={0};
 int rc=get_desc(o,h,0,1,dev,8);
 if(rc!=8||dev[1]!=1||dev[0]<8) return rc<0?rc:-20;
 h->mps=dev[7];
 if(h->mps!=8&&h->mps!=16&&h->mps!=32&&h->mps!=64) return -21;
 rc=set_request(o,h,0,0,5,1,0); /* SET_ADDRESS 1 */
 if(rc<0) return rc;
 o->delay_us(o->ctx,10000);
 h->address=1;
 rc=get_desc(o,h,1,1,dev,18);
 if(rc!=18||dev[1]!=1) return rc<0?rc:-22;
 uint8_t config_head[9]={0};
 rc=get_desc(o,h,1,2,config_head,9);
 if(rc!=9 || config_head[1]!=2) return rc<0?rc:-23;
 uint16_t total=(uint16_t)config_head[2] | (uint16_t)((uint16_t)config_head[3]<<8);
 if(total<9||total>256) return -24;
 uint8_t cfg[256]={0};
 rc=get_desc(o,h,1,2,cfg,total);
 if(rc!=total) return rc<0?rc:-25;
 unsigned cfg_id=cfg[5];
 uint8_t active_itf=0xff,active_protocol=0;
 for(unsigned pos=0;pos+2<=total;) {
    uint8_t size=cfg[pos],type=cfg[pos+1];
    if(size<2||pos+size>total) return -26;
    if(type==4 && size>=9) {
        active_itf=cfg[pos+2];
        active_protocol=(cfg[pos+5]==3&&cfg[pos+6]==1)?
                         cfg[pos+7]:0; /* HID Boot only */
    } else if(type==5 && size>=7 && (cfg[pos+2]&0x80u) &&
              ((cfg[pos+3]&3u)==3u)) {
       struct frf_native_hid_ep *e=NULL;
       if(active_protocol==1&&!h->have_keyboard) {
           e=&h->keyboard;h->have_keyboard=1;
       } else if(active_protocol==2&&!h->have_mouse) {
           e=&h->mouse;h->have_mouse=1;
       }
       if(e) {
          e->ep=cfg[pos+2]&15u;
          e->interface=active_itf;
          e->protocol=active_protocol;
          e->interval=cfg[pos+6];
          e->max_packet=(uint16_t)cfg[pos+4]|((uint16_t)cfg[pos+5]<<8);
          if(!e->max_packet||e->max_packet>64) return -27;
       }
    }
    pos+=size;
 }
 if(!h->have_keyboard&&!h->have_mouse) return -28; /* e.g. root hub */
 rc=set_request(o,h,1,0,9,(uint16_t)cfg_id,0); /* SET_CONFIGURATION */
 if(rc<0) return rc;
 h->configured=1;
 if(h->have_keyboard) {
    rc=set_request(o,h,1,0x21,0x0b,0,h->keyboard.interface);
    if(rc<0) return rc; /* SET_PROTOCOL BOOT */
 }
 if(h->have_mouse) {
    rc=set_request(o,h,1,0x21,0x0b,0,h->mouse.interface);
    if(rc<0) return rc;
    frf_usb_mouse_present=1;
 }
 return 0;
}
static void poll_ep(const struct frf_dwc2_ops *o,struct frf_native_hid *h,
                    struct frf_native_hid_ep *ep)
{
 if(!ep->max_packet) return;
 uint8_t packet[64]={0};
 struct frf_dwc2_packet p={
    h->address,ep->ep,1,3,0,ep->toggle,ep->max_packet,
    ep->max_packet,0,packet
 };
 int rc=frf_dwc2_pio_packet(o,&p,&h->stats);
 if(rc==-5){ep->nak++;return;}
 if(rc<0){ep->faults++;return;}
 ep->success++;
 ep->toggle=ep->toggle==FRF_USB_PID_DATA0?
                      FRF_USB_PID_DATA1:FRF_USB_PID_DATA0;
 if(ep->protocol==1 && p.received>=8) {
     frf_native_usb_keyboard_boot_report(packet);
 } else if(ep->protocol==2 && p.received>=3) {
     unsigned buttons=packet[0]&3u;
     aga_virtual_mouse_update((int)(int8_t)packet[1],
                              (int)(int8_t)packet[2],buttons);
     frf_usb_mouse_seen=1;
     frf_usb_mouse_buttons=(uint8_t)buttons;
 }
}
void frf_native_hid_poll(const struct frf_dwc2_ops *o,
                         struct frf_native_hid *h)
{
 if(!o||!h||!h->configured)return;
 /* Prioritise keyboard; mouse and gamepad processing cannot block its
  * completion. Poll scheduling/TT splits still need final validation. */
 h->elapsed_ms++;
 if(h->have_keyboard &&
    (int32_t)(h->elapsed_ms-h->keyboard.next_poll_ms)>=0) {
    poll_ep(o,h,&h->keyboard);
    h->keyboard.next_poll_ms=h->elapsed_ms+
         (h->keyboard.interval?h->keyboard.interval:1u);
 }
 if(h->have_mouse &&
    (int32_t)(h->elapsed_ms-h->mouse.next_poll_ms)>=0) {
    poll_ep(o,h,&h->mouse);
    h->mouse.next_poll_ms=h->elapsed_ms+
         (h->mouse.interval?h->mouse.interval:1u);
 }
}
