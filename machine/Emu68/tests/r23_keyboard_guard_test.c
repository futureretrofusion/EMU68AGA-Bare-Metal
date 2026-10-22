/* R23 regression: exact receiver HID does not become keyboard;
 * genuine keyboard press/hold/release remain correctly edge-triggered.
 */
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include "../src/frf_usb/tinyusb/frf_r23_keyboard_guard.h"
#include "../src/frf_usb/native/frf_native_keyboard.h"
struct results { uint8_t raw[64];unsigned count; };
static void collect(void *ctx,uint8_t raw) {
 struct results *r=ctx;
 assert(r->count<sizeof(r->raw));
 r->raw[r->count++]=raw;
}
static void send_keyboard(struct frf_native_keyboard *kbd,
 const uint8_t *buf, unsigned len,uint16_t vid,uint16_t pid,
 struct results *r) {
 if(frf_r23_keyboard_allowed(vid,pid,1u,len))
   frf_native_keyboard_report(kbd,buf,collect,r);
}
int main(void) {
 struct frf_native_keyboard k={0};
 struct results r={0};
 uint8_t gamepad[24]={0};
 gamepad[0]=2;gamepad[1]=4;gamepad[3]=128;
 assert(!frf_r23_keyboard_allowed(0x7e05u,0x0920u,1u,24u));
 assert(!frf_r23_keyboard_allowed(0x7e05u,0x0920u,1u,8u));
 assert(!frf_r23_keyboard_allowed(0x046du,0xc31cu,1u,24u));
 assert(!frf_r23_keyboard_allowed(0x046du,0xc31cu,2u,8u));
 assert(frf_r23_keyboard_allowed(0x046du,0xc31cu,1u,8u));
 send_keyboard(&k,gamepad,24,0x7e05u,0x0920u,&r);
 send_keyboard(&k,gamepad,8,0x7e05u,0x0920u,&r);
 assert(r.count==0u && k.reports==0u);
 uint8_t keydown[8]={0};
 keydown[2]=4u; /* A */
 send_keyboard(&k,keydown,8,0x046du,0xc31cu,&r);
 assert(r.count==1 && r.raw[0]==0x20u);
 for(unsigned i=0;i<1000u;++i)
   send_keyboard(&k,keydown,8,0x046du,0xc31cu,&r);
 assert(r.count==1u); /* holding never emits spurious down */
 uint8_t release[8]={0};
 send_keyboard(&k,release,8,0x046du,0xc31cu,&r);
 assert(r.count==2u && r.raw[1]==0xa0u);
 for(unsigned i=0;i<1000u;++i)
   send_keyboard(&k,gamepad,24,0x7e05u,0x0920u,&r);
 assert(r.count==2u && k.keys[0]==0u);
 /* Disconnect with key held: force report 0 releases all keys. */
 send_keyboard(&k,keydown,8,0x046du,0xc31cu,&r);
 frf_native_keyboard_report(&k,release,collect,&r);
 assert(r.count==4u && r.raw[3]==0xa0u);
 puts("R23 receiver blocked, malformed 24B blocked, 1000x held key no repeat, key release on unplug PASS");
 return 0;
}
