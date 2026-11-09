/* SPDX-License-Identifier: GPL-2.0-only
 * Independent Pi3 HID boot input: root-hub port scan and split routing.
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
 struct frf_dwc2_packet p={addr,ep,in,type,h->low_speed,pid,mps,
                          len,0,data,h->hub_address,h->hub_port};
 int rc=0;
 /* Enumerating an FS keyboard through a hub TT can legitimately NAK while
  * recovering from reset. Retry control phases within a strict deadline;
  * interrupt polling still returns NAK to the caller without blocking. */
 for(unsigned retry=0;retry<(type==0?12u:1u);++retry) {
    rc=frf_dwc2_pio_packet(o,&p,&h->stats);
    if(rc!=-5 && rc!=-8) break;
    o->delay_us(o->ctx,1000u);
 }
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
static int enumerate_device(const struct frf_dwc2_ops *o,
                            struct frf_native_hid *h,
                            unsigned root_speed,uint8_t assigned_addr)
{
 if(!o||!h) return -1;
 /* High-speed default EP0 requires 64-byte packets; FS/LS start at 8. */
 h->mps=(root_speed==0u)?64u:8u;
 uint8_t dev[18]={0};
 int rc=get_desc(o,h,0,1,dev,8);
 if(rc!=8||dev[1]!=1||dev[0]<8) return rc<0?rc:-20;
 h->mps=dev[7];
 if(h->mps!=8&&h->mps!=16&&h->mps!=32&&h->mps!=64) return -21;
 rc=set_request(o,h,0,0,5,assigned_addr,0); /* SET_ADDRESS */
 if(rc<0) return rc;
 o->delay_us(o->ctx,10000);
 h->address=assigned_addr;
 rc=get_desc(o,h,assigned_addr,1,dev,18);
 if(rc!=18||dev[1]!=1) return rc<0?rc:-22;
 h->root_device_class=dev[4];
 uint8_t config_head[9]={0};
 rc=get_desc(o,h,assigned_addr,2,config_head,9);
 if(rc!=9 || config_head[1]!=2) return rc<0?rc:-23;
 uint16_t total=(uint16_t)config_head[2] | (uint16_t)((uint16_t)config_head[3]<<8);
 if(total<9||total>256) return -24;
 uint8_t cfg[256]={0};
 rc=get_desc(o,h,assigned_addr,2,cfg,total);
 if(rc!=total) return rc<0?rc:-25;
 unsigned cfg_id=cfg[5];
 uint8_t active_itf=0xff,active_protocol=0;
 for(unsigned pos=0;pos+2<=total;) {
    uint8_t size=cfg[pos],type=cfg[pos+1];
    if(size<2||pos+size>total) return -26;
    if(type==4 && size>=9) {
        if(cfg[pos+5]==9u) h->root_hub_interface=1u;
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
          e->device_address=assigned_addr;
          e->hub_address=h->hub_address;
          e->hub_port=h->hub_port;
          e->low_speed=h->low_speed;
          e->interface=active_itf;
          e->protocol=active_protocol;
          e->interval=cfg[pos+6];
          e->max_packet=(uint16_t)cfg[pos+4]|((uint16_t)cfg[pos+5]<<8);
          if(!e->max_packet||e->max_packet>64) return -27;
       }
    }
    pos+=size;
 }
 if(!h->have_keyboard&&!h->have_mouse &&
    !h->root_hub_interface && h->root_device_class!=9u) return -28;
 rc=set_request(o,h,assigned_addr,0,9,(uint16_t)cfg_id,0);
 if(rc<0) return rc;
 if(!h->have_keyboard&&!h->have_mouse) return -29; /* hub configured */
 /* SET_CONFIGURATION */
 if(rc<0) return rc;
 h->configured=1;
 if(h->have_keyboard) {
    rc=set_request(o,h,assigned_addr,0x21,0x0b,0,h->keyboard.interface);
    if(rc<0) return rc; /* SET_PROTOCOL BOOT */
 }
 if(h->have_mouse) {
    rc=set_request(o,h,assigned_addr,0x21,0x0b,0,h->mouse.interface);
    if(rc<0) return rc;
    frf_usb_mouse_present=1;
 }
 return 0;
}

/* USB 2.0 hub class. Root high-speed hub is addr 1; FS/LS devices
 * behind its transaction translator use HCSPLT routed through that port.
 * Connection snapshot is read after hub power and per-port reset.
 */
static int hub_feature(const struct frf_dwc2_ops *o,
                       struct frf_native_hid *h,uint8_t port,
                       uint8_t req,uint16_t feature)
{
 uint8_t setup[8]={0x23,req,(uint8_t)feature,(uint8_t)(feature>>8),
                   port,0,0,0};
 return control(o,h,1,setup,NULL,0);
}
static int hub_status(const struct frf_dwc2_ops *o,
                      struct frf_native_hid *h,uint8_t port,
                      uint32_t *status)
{
 uint8_t setup[8]={0xa3,0,0,0,port,0,4,0};
 uint8_t data[4]={0};
 int rc=control(o,h,1,setup,data,4);
 if(rc!=4) return rc<0?rc:-41;
 *status=(uint32_t)data[0]|((uint32_t)data[1]<<8)|
         ((uint32_t)data[2]<<16)|((uint32_t)data[3]<<24);
 return 0;
}
static int scan_root_hub(const struct frf_dwc2_ops *o,
                         struct frf_native_hid *h)
{
 uint8_t setup[8]={0xa0,6,0,0,0,0,9,0};
 uint8_t desc[9]={0};
 int rc=control(o,h,1,setup,desc,9);
 if(rc!=9||desc[1]!=0x29u) {
    /* Some USB hub implementations accept only the descriptor type in
     * wValue, others implement the hub class request with wValue=0. */
    setup[3]=0x29;
    rc=control(o,h,1,setup,desc,9);
 }
 if(rc!=9||desc[1]!=0x29u) return rc<0?rc:-42;
 if(!desc[2]||desc[2]>15u) return -43;
 h->root_hub_ports=desc[2];
 /* All ports powered before checking connection, as in hub class state machine */
 for(uint8_t port=1;port<=h->root_hub_ports;++port) {
    rc=hub_feature(o,h,port,3,8); /* SET_FEATURE(PORT_POWER) */
    if(rc<0) return -44;
 }
 o->delay_us(o->ctx,((unsigned)desc[5]*2u+25u)*1000u);
 struct frf_native_hid_ep kept_keyboard={0},kept_mouse={0};
 int kept_kb=0,kept_ms=0;
 for(uint8_t port=1;port<=h->root_hub_ports;++port) {
    uint32_t st=0;
    rc=hub_status(o,h,port,&st);
    if(rc<0) continue;
    h->port_status=st&0xffffu;
    h->port_change=st>>16;
    if(!(st&1u)) continue;
    h->ports_seen++;
    rc=hub_feature(o,h,port,3,4); /* SET_FEATURE(PORT_RESET) */
    if(rc<0) continue;
    o->delay_us(o->ctx,30000u);
    /* Hub reset completion is asynchronous; sample status for <=200 ms. */
    for(unsigned wait=0;wait<10u;++wait) {
        rc=hub_status(o,h,port,&st);
        if(rc==0 && (st&2u)) break;
        o->delay_us(o->ctx,20000u);
    }
    if(rc<0||!(st&2u)) continue; /* port not enabled */
    h->port_status=st&0xffffu;
    h->port_change=st>>16;
    (void)hub_feature(o,h,port,1,20); /* CLEAR_FEATURE(C_PORT_RESET) */
    /* Device address zero is port-scoped by TT. FS/LS need split,
       HS devices use their native high-speed root controller route. */
    uint8_t low=(st&(1u<<9))!=0u;
    uint8_t high=(st&(1u<<10))!=0u;
    h->hub_address=high?0u:1u;
    h->hub_port=high?0u:port;
    h->low_speed=low;
    h->have_keyboard=h->have_mouse=0u;
    h->keyboard=(struct frf_native_hid_ep){0};
    h->mouse=(struct frf_native_hid_ep){0};
    h->root_device_class=0;
    h->root_hub_interface=0;
    rc=enumerate_device(o,h,high?0u:(low?2u:1u),(uint8_t)(1u+port));
    h->port_error=rc;
    if(rc==0 && h->configured) {
       if(h->have_keyboard && !kept_kb) {
          kept_keyboard=h->keyboard;kept_kb=1;
       }
       if(h->have_mouse && !kept_ms) {
          kept_mouse=h->mouse;kept_ms=1;
       }
    }
    if(kept_kb&&kept_ms) break;
    /* Restore direct root-hub control route for next port. */
    h->hub_address=h->hub_port=h->low_speed=0;
    h->mps=64;
    h->address=1;
 }
 h->have_keyboard=kept_kb;
 h->have_mouse=kept_ms;
 h->keyboard=kept_keyboard;
 h->mouse=kept_mouse;
 h->configured=(kept_kb||kept_ms)?1u:0u;
 return h->configured?0:-45;
}
int frf_native_hid_enumerate(const struct frf_dwc2_ops *o,
                             struct frf_native_hid *h,unsigned root_speed)
{
 if(!o||!h) return -1;
 memset(h,0,sizeof(*h));
 int rc=enumerate_device(o,h,root_speed,1u);
 if(rc!=-29) return rc;
 /* Root hub configured; control requests still use address one. */
 h->hub_address=h->hub_port=h->low_speed=0;
 h->mps=64u;
 h->address=1u;
 return scan_root_hub(o,h);
}
static void poll_ep(const struct frf_dwc2_ops *o,struct frf_native_hid *h,
                    struct frf_native_hid_ep *ep)
{
 if(!ep->max_packet) return;
 uint8_t packet[64]={0};
 struct frf_dwc2_packet p={
    ep->device_address,ep->ep,1,3,ep->low_speed,ep->toggle,
    ep->max_packet,ep->max_packet,0,packet,
    ep->hub_address,ep->hub_port
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
