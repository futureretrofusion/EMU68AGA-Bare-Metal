/* R27: observed 220D:330C HID ID3/27B fallback; profile needs hardware validation. */
#ifndef FRF_R27_220D_H
#define FRF_R27_220D_H
#include <stdint.h>
static inline int frf_r27_decode(const uint8_t *b,unsigned n,uint16_t *joy,int *fire)
{
 if(!b || !joy || !fire || n!=27u || b[0]!=3u) return 0;
 /* Captured neutral sample: 03 00 03 80 80 80 80 FF...
  * Offsets 3,4 are candidate centered axes. Buttons remain provisional. */
 int x=(int)b[3]-128,y=(int)b[4]-128;
 int l=x< -36,r=x>36,u=y< -36,d=y>36;
 *joy=(uint16_t)((l?0x0200u:0u)|((l^u)?0x0100u:0u)|
                 (r?0x0002u:0u)|((r^d)?0x0001u:0u));
 *fire=(b[1]&0x0fu)!=0u;
 return 1;
}
#endif
