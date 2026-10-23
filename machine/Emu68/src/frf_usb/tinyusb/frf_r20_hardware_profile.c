/* SPDX-License-Identifier: GPL-2.0-only
 * R21 hardware-observed 24-byte receiver compatibility profile.
 * Restricted to device VID:PID 057E:2009. TinyUSB HID instance is runtime-assigned.
 * This is an inferred report layout from R18 live packet evidence,
 * not a vendor-published specification. Tested separately.
 */
#include <stdint.h>
#include <stdbool.h>
int frf_r20_is_receiver(uint16_t vid,uint16_t pid,uint8_t itf) {
 (void)itf;
 return vid==0x057eu && pid==0x2009u;
}
int frf_r20_observed_neutral(const uint8_t *p,uint16_t size) {
 if(!p || size!=24u || p[0]!=0u) return 0;
 /* R18 capture: initial three bytes zero, left and right analog pairs
    centered near 128 at offsets 3..6, neutral hat marker FF at 9. */
 if(p[1] || p[2] || p[7] || p[8] || p[9]!=0xffu) return 0;
 for(unsigned i=3;i<=6u;i++) if(p[i]<96u || p[i]>160u) return 0;
 return 1;
}
int frf_r20_decode(const uint8_t *p,uint16_t size,uint16_t *joy,int *fire)
{
 if(!p || !joy || !fire || size!=24u) return 0;
 /* On observed controller the first report byte remains 00;
    don't confuse a report-ID-prefixed HID protocol with this one. */
 int x=(int)p[3]-128,y=(int)p[4]-128;
 int left=x<-40,right=x>40,up=y<-40,down=y>40;
 int hat=(int)p[9];
 if(hat>=0 && hat<=7) {
   up |=hat==0 || hat==1 || hat==7;
   right |=hat==1 || hat==2 || hat==3;
   down |=hat==3 || hat==4 || hat==5;
   left |=hat==5 || hat==6 || hat==7;
 }
 /* Button location is provisional, not vendor documented. Only
    non-neutral bits from low USB report bytes are treated as fire. */
 unsigned buttons=(unsigned)p[0]|(unsigned)p[1]|(unsigned)p[2]|
                  (unsigned)p[7]|(unsigned)p[8];
 if(left&&right)left=right=0;
 if(up&&down)up=down=0;
 *joy=(uint16_t)((left?0x200u:0) |
         (((left!=0)^(up!=0))?0x100u:0) |
         (right?2u:0) |
         (((right!=0)^(down!=0))?1u:0));
 *fire=buttons!=0u;
 return 1;
}
