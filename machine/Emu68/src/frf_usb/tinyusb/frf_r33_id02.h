/* R37: restore real 220D:330C HID ID03 directional packets (03 00 03/06 80...).
   Previously the 27-byte ID03 input reports were counted but discarded. */
#ifndef FRF_R33_ID02_H
#define FRF_R33_ID02_H
#include <stdint.h>
static inline int frf_r37_direction_report(const uint8_t*b,unsigned n,uint16_t*joy){
 if(!b||n!=27u||(b[0]!=2u && b[0]!=3u)||!joy)return 0;
 int l=b[3]<92u||b[5]<92u,r=b[3]>164u||b[5]>164u;
 int u=b[4]<92u||b[6]<92u,d=b[4]>164u||b[6]>164u;
 unsigned h=b[2]&15u;
 /* R28 ID02 base low nibble=7; treat 7 as neutral unless axes move. */
 if(h<7u){u|=h==0u||h==1u;r|=h==1u||h==2u||h==3u;
 d|=h==3u||h==4u||h==5u;l|=h==5u||h==6u;}
 if(l&&r)l=r=0;
 if(u&&d)u=d=0;
 *joy=(uint16_t)((l?512u:0u)|((l^u)?256u:0u)|(r?2u:0u)|((r^d)?1u:0u));
 return 1;
}
static inline int frf_r33_id02(const uint8_t*b,unsigned n,uint16_t*joy){
 return b && b[0]==2u && frf_r37_direction_report(b,n,joy);
}
static inline int frf_r37_id03(const uint8_t*b,unsigned n,uint16_t*joy){
 return b && b[0]==3u && frf_r37_direction_report(b,n,joy);
}
static inline uint16_t frf_r33_merge_joy(uint16_t a,uint16_t b){
 int al=(a&512u)!=0,bl=(b&512u)!=0,ar=(a&2u)!=0,br=(b&2u)!=0;
 int l=al|bl,r=ar|br,u=(al^((a&256u)!=0))|(bl^((b&256u)!=0));
 int d=(ar^((a&1u)!=0))|(br^((b&1u)!=0));
 if(l&&r)l=r=0;
 if(u&&d)u=d=0;
 return (uint16_t)((l?512u:0u)|((l^u)?256u:0u)|(r?2u:0u)|((r^d)?1u:0u));
}
#endif
