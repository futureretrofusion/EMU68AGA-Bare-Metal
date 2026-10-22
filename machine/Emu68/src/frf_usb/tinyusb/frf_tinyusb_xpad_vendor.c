/* SPDX-License-Identifier: GPL-2.0-only
 * Native Pi3 TinyUSB vendor controller host driver.
 * Supports wired XInput FF/5D/01 and known wired Xbox One GIP FF/47/D0.
 * CPU1 owns USB requests; no USPi dependency.
 * Inputs are decoded only from a validated data-report signature.
 */
#include <stdint.h>
#include <string.h>
#include "tusb.h"
#include "host/usbh_pvt.h"
#define FRF_XPAD_MAX 4u

struct frf_xpad {
    uint8_t addr,itf,ep_in,ep_out,kind;
    uint8_t configured,in_flight,send_init,init_step;
    uint16_t vid,pid,ep_size;
    uint32_t packets,errors,decode;
    uint8_t report[64] __attribute__((aligned(64)));
    uint8_t tx[64] __attribute__((aligned(64)));
};
static struct frf_xpad xpads[FRF_XPAD_MAX];
static uint32_t xpad_packets,xpad_errors;
extern void aga_virtual_joy_set(int port,uint16_t joydat,int fire);
extern void kprintf(const char*,...);
extern void frf_post_native_usb_vendor_probe(uint32_t vidpid,uint32_t iface,uint32_t kind);
extern void frf_post_native_usb_device_event(uint32_t,uint32_t,uint32_t,uint32_t);

uint32_t frf_tinyusb_xpad_count(void) {
    uint32_t count=0;
    for(unsigned i=0;i<FRF_XPAD_MAX;i++) if(xpads[i].addr) ++count;
    return count;
}
uint32_t frf_tinyusb_xpad_packets(void) { return xpad_packets; }
uint32_t frf_tinyusb_xpad_errors(void) { return xpad_errors; }

static struct frf_xpad *find(uint8_t addr) {
    for(unsigned i=0;i<FRF_XPAD_MAX;i++) if(xpads[i].addr==addr) return &xpads[i];
    return 0;
}
static struct frf_xpad *free_slot(void) {
    for(unsigned i=0;i<FRF_XPAD_MAX;i++) if(!xpads[i].addr) return &xpads[i];
    return 0;
}
static int is_gamepad_vendor(uint16_t vid) {
    return vid==0x045eu || vid==0x146bu || vid==0x2dc8u;
}
int frf_xpad_interface_kind(uint16_t vid,uint16_t pid,
                            uint8_t itf,uint8_t cls,uint8_t sub,uint8_t proto)
{
    (void)pid;
    if(cls!=0xffu) return 0;
    if(sub==0x5du && proto==0x01u) return 1; /* Xbox 360 wired XInput */
    if(sub==0x47u && proto==0xd0u && itf==0u &&
       is_gamepad_vendor(vid)) return 2; /* Xbox One GIP */
    return 0;
}
static int16_t le_s16(const uint8_t *b) {
    return (int16_t)((uint16_t)b[0]|((uint16_t)b[1]<<8));
}
static void publish_joy(int x,int y,int up,int down,int left,int right,int fire)
{
    left  |= x < -10000;
    right |= x >  10000;
    up    |= y < -10000;
    down  |= y >  10000;
    if(left&&right) left=right=0;
    if(up&&down) up=down=0;
    const uint16_t joy=(uint16_t)
       ((left?0x0200u:0u)|(((left!=0)^(up!=0))?0x0100u:0u)|
        (right?0x0002u:0u)|(((right!=0)^(down!=0))?0x0001u:0u));
    aga_virtual_joy_set(1,joy,fire);
}
int frf_xpad_decode_report(int kind,const uint8_t *b,uint16_t len)
{
    if(!b) return 0;
    int dp=0,x=0,y=0,fire=0;
    if(kind==1) {
        if(len<14u || b[0]!=0x00u || b[1]!=0x14u) return 0;
        dp=b[2]&0x0fu;
        x=le_s16(b+6);
        y=-(int)le_s16(b+8);
        fire=((b[3]&0xf0u)!=0u) || ((b[3]&0x03u)!=0u);
    } else if(kind==2) {
        if(len<18u || b[0]!=0x20u) return 0;
        dp=b[5]&0x0fu;
        x=le_s16(b+10);
        y=-(int)le_s16(b+12);
        fire=((b[4]&0xf0u)!=0u) || ((b[5]&0x30u)!=0u);
    } else return 0;
    publish_joy(x,y,(dp&1)!=0,(dp&2)!=0,(dp&4)!=0,(dp&8)!=0,fire);
    return 1;
}
static bool driver_init(void) { memset(xpads,0,sizeof(xpads));return true; }
static bool driver_deinit(void) { return true; }
static uint16_t driver_open(uint8_t rhport,uint8_t addr,
                            const tusb_desc_interface_t *itf,uint16_t remaining)
{
    (void)rhport;
    uint16_t vid=0,pid=0;
    if(!tuh_vid_pid_get(addr,&vid,&pid)) return 0;
    int kind=frf_xpad_interface_kind(vid,pid,itf->bInterfaceNumber,
         itf->bInterfaceClass,itf->bInterfaceSubClass,itf->bInterfaceProtocol);
    if(itf->bInterfaceClass==0xffu) {
       const uint32_t classif=((uint32_t)itf->bInterfaceClass<<24) |
         ((uint32_t)itf->bInterfaceSubClass<<16)|
         ((uint32_t)itf->bInterfaceProtocol<<8)|itf->bInterfaceNumber;
       frf_post_native_usb_vendor_probe(((uint32_t)vid<<16)|pid,classif,(uint32_t)kind);
       frf_post_native_usb_device_event(addr,((uint32_t)vid<<16)|pid,classif,4u);
    }
    if(!kind || find(addr)) return 0;
    struct frf_xpad *p=free_slot();
    if(!p) return 0;
    uint16_t consumed=itf->bLength;
    if(consumed<sizeof(*itf) || consumed>remaining) return 0;
    uint8_t ep_in=0,ep_out=0;uint16_t ep_size=0;
    const tusb_desc_endpoint_t *epin_desc=0,*epout_desc=0;
    const uint8_t *cursor=(const uint8_t*)itf+consumed;
    while((uint32_t)consumed+2u<=remaining) {
        const uint8_t blen=cursor[0],dtype=cursor[1];
        if(blen<2 || (uint32_t)consumed+blen>remaining || dtype==TUSB_DESC_INTERFACE)
            break;
        if(dtype==TUSB_DESC_ENDPOINT && blen>=sizeof(tusb_desc_endpoint_t)) {
            const tusb_desc_endpoint_t *ep=(const tusb_desc_endpoint_t*)cursor;
            if(ep->bmAttributes.xfer==TUSB_XFER_INTERRUPT) {
                const uint16_t mps=tu_edpt_packet_size(ep);
                if((ep->bEndpointAddress&0x80u) && !ep_in &&
                   mps>0 && mps<=64) {
                    ep_in=ep->bEndpointAddress;ep_size=mps;
                    epin_desc=ep;
                } else if(!(ep->bEndpointAddress&0x80u) && !ep_out &&
                         mps>0 && mps<=64) {
                    ep_out=ep->bEndpointAddress;
                    epout_desc=ep;
                }
            }
        }
        cursor+=blen;consumed+=blen;
    }
    if(!epin_desc || (kind==2 && !epout_desc)) return 0;
    /* Check the COMPLETE interface before opening any endpoint. If
     * GIP lacks OUT, don't leak an already-allocated IN HCD channel. */
    if(!tuh_edpt_open(addr,epin_desc)) return 0;
    if(epout_desc && !tuh_edpt_open(addr,epout_desc)) {
        (void)tuh_edpt_close(addr,ep_in);
        return 0;
    }
    memset(p,0,sizeof(*p));
    p->addr=addr;p->itf=itf->bInterfaceNumber;p->ep_in=ep_in;
    p->ep_out=ep_out;p->ep_size=ep_size;
    p->kind=(uint8_t)kind;p->vid=vid;p->pid=pid;
    kprintf("[FRF-XPAD] vendor pad USB %04x:%04x kind=%u dev=%u itf=%u IN=%02x OUT=%02x\n",
            vid,pid,(unsigned)kind,addr,p->itf,ep_in,ep_out);
    return consumed;
}
static bool driver_set_config(uint8_t addr,uint8_t itf)
{
    struct frf_xpad *p=find(addr);
    if(!p || p->itf!=itf) return false;
    p->configured=1;
    p->send_init=(p->kind==2);
    usbh_driver_set_config_complete(addr,itf);
    return true;
}
static bool driver_xfer_cb(uint8_t addr,uint8_t ep,xfer_result_t result,uint32_t count)
{
    struct frf_xpad *p=find(addr);
    if(!p) return false;
    p->in_flight=0;
    if(result==XFER_RESULT_SUCCESS) {
        if(ep==p->ep_in) {
            ++p->packets;++xpad_packets;
            if(count<=sizeof(p->report) &&
               frf_xpad_decode_report(p->kind,p->report,(uint16_t)count))
                ++p->decode;
        } else if(ep==p->ep_out && p->kind==2) {
            ++p->init_step;
            /* Xbox One handshake runs in order, as completed USB OUT
             * transfers; never queue two OUT packets simultaneously. */
            unsigned steps=(p->vid==0x045eu && p->pid==0x02eau)?4u:3u;
            if(p->init_step<steps) p->send_init=1;
        }
    } else {
        ++p->errors;++xpad_errors;
        if(ep==p->ep_out) p->send_init=0; /* recover by polling input */
    }
    /* Retry from CPU1 poll, not synchronously from a failed IRQ callback. */
    return true;
}
static void driver_close(uint8_t addr)
{
    struct frf_xpad *p=find(addr);
    if(!p) return;
    memset(p,0,sizeof(*p));
    if(!frf_tinyusb_xpad_count()) aga_virtual_joy_set(1,0,0);
}
static const usbh_class_driver_t driver={
    "FRF-XINPUT",driver_init,driver_deinit,driver_open,
    driver_set_config,driver_xfer_cb,driver_close
};
const usbh_class_driver_t* usbh_app_driver_get_cb(uint8_t *count)
{
    if(count) *count=1;
    return &driver;
}
void frf_tinyusb_xpad_task(void)
{
    for(unsigned i=0;i<FRF_XPAD_MAX;i++) {
        struct frf_xpad *p=&xpads[i];
        if(!p->addr || !p->configured || p->in_flight) continue;
        if(p->send_init && p->ep_out) {
            static const uint8_t power[5]={5,0x20,0,1,0};
            static const uint8_t xbox_one_s[5]={5,0x20,1,0x0f,6};
            static const uint8_t led[7]={0x0a,0x20,2,3,0,1,0x14};
            static const uint8_t auth[6]={6,0x20,3,2,1,0};
            int s=(int)p->init_step;
            if(!(p->vid==0x045eu && p->pid==0x02eau) && s>=1) ++s;
            const uint8_t *src=s==0?power:s==1?xbox_one_s:s==2?led:auth;
            const unsigned len=s==2?7u:s==3?6u:5u;
            memcpy(p->tx,src,len);
            p->send_init=0;
            p->in_flight=usbh_edpt_xfer(p->addr,p->ep_out,p->tx,len);
            if(!p->in_flight) {p->send_init=1; ++p->errors;}
        } else if(!usbh_edpt_busy(p->addr,p->ep_in)) {
            p->in_flight=usbh_edpt_xfer(p->addr,p->ep_in,p->report,p->ep_size);
        }
    }
}
