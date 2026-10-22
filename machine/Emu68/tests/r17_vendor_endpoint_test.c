/* Test the REAL TinyUSB R17 vendor class-driver against synthetic
 * USB interface+endpoint wire descriptors under x86 and AArch64 big-endian.
 * No synthetic success flags: driver open/config/USB IN/callback pathways. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include "tusb.h"
#include "host/usbh_pvt.h"
static unsigned opens,submits,configs,last_xfer_type;
static uint8_t *in_buffer;
static uint16_t in_len;
bool tuh_vid_pid_get(uint8_t daddr,uint16_t *vid,uint16_t *pid) {
  if(daddr!=4u) return false;
  *vid=0x2404u;*pid=0x0078u;return true;
}
void frf_post_native_usb_vendor_probe(uint32_t a,uint32_t b,uint32_t c)
  {(void)a;(void)b;(void)c;}
void frf_post_native_usb_device_event(uint32_t a,uint32_t b,uint32_t c,uint32_t d)
  {(void)a;(void)b;(void)c;(void)d;}
void aga_virtual_joy_set(int port,uint16_t joy,int fire)
  {(void)port;(void)joy;(void)fire;assert(0 && "Passive probe must never map joystick");}
void kprintf(const char*fmt,...) {(void)fmt;}
bool tuh_edpt_open(uint8_t daddr,tusb_desc_endpoint_t const*ep) {
  assert(daddr==4u);
  ++opens;
  last_xfer_type=((uint8_t const*)ep)[3]&3u;
  assert(ep->bEndpointAddress==0x81u);
  assert(tu_edpt_packet_size(ep)==64u);
  return true;
}
bool tuh_edpt_close(uint8_t daddr,uint8_t ep)
  {(void)daddr;(void)ep;return true;}
bool usbh_edpt_busy(uint8_t daddr,uint8_t ep) {
  assert(daddr==4u && ep==0x81u);
  return false;
}
bool usbh_edpt_xfer_with_callback(uint8_t daddr,uint8_t ep,
  uint8_t*buf,uint16_t size,tuh_xfer_cb_t cb,uintptr_t arg) {
  (void)cb;(void)arg;
  assert(daddr==4u && ep==0x81u && size==64u);
  in_buffer=buf;in_len=size;++submits;return true;
}
void usbh_driver_set_config_complete(uint8_t daddr,uint8_t itf) {
  assert(daddr==4u && itf==0u);++configs;
}
extern void frf_tinyusb_xpad_task(void);
extern void frf_tinyusb_vendor_probe_snapshot(uint32_t out[7]);
static void run_descriptor(uint8_t transfer_type)
{
  static uint8_t cfg[16]={
     9, TUSB_DESC_INTERFACE, 0, 0, 1, 0xff,0,0xff,0,
     7, TUSB_DESC_ENDPOINT, 0x81, 2, 64,0,1};
  cfg[12]=transfer_type;
  uint8_t count=0;
  const usbh_class_driver_t*d=usbh_app_driver_get_cb(&count);
  assert(count==1u && d && d->init);
  assert(d->open(0u,4u,(tusb_desc_interface_t*)cfg,16u)==16u);
  assert(opens>0u && last_xfer_type==transfer_type);
  assert(d->set_config(4u,0u) && configs>0u);
  uint32_t stats[7]={0};
  frf_tinyusb_vendor_probe_snapshot(stats);
  assert((stats[0]>>24)==6u);
  frf_tinyusb_xpad_task();
  assert(submits>0 && in_buffer && in_len==64u);
  frf_tinyusb_vendor_probe_snapshot(stats);
  assert((stats[0]>>24)==7u && stats[3]>0u);
  in_buffer[0]=0xa5;in_buffer[1]=0x11;
  in_buffer[2]=0x22;in_buffer[3]=0x33;
  assert(d->xfer_cb(4u,0x81u,XFER_RESULT_SUCCESS,4u));
  frf_tinyusb_vendor_probe_snapshot(stats);
  assert((stats[0]>>24)==8u && stats[5]>0u);
  d->close(4u);
  in_buffer=NULL;
}
int main(void)
{
  run_descriptor(TUSB_XFER_BULK);
  run_descriptor(TUSB_XFER_INTERRUPT);
  puts("R17 AArch64 BE REAL vendor class open BULK/INT, set-config, IN submit/receive PASS");
  return 0;
}
