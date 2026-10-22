#include <stdint.h>
#include <assert.h>
#include "../src/frf_usb/tinyusb/frf_r33_id02.h"
int main(void){
 uint8_t b[27]={2,0,7,128,128,128,128,0};
 uint16_t v=42;
 assert(frf_r33_id02(b,27,&v)&&v==0);
 b[3]=0;assert(frf_r33_id02(b,27,&v)&&v==0x300);
 b[3]=255;assert(frf_r33_id02(b,27,&v)&&v==3);
 b[3]=128;b[4]=0;assert(frf_r33_id02(b,27,&v)&&v==0x100);
 b[4]=128;b[5]=0;assert(frf_r33_id02(b,27,&v)&&v==0x300);
 b[5]=128;b[6]=255;assert(frf_r33_id02(b,27,&v)&&v==1);
 b[6]=128;b[4]=0;b[5]=0;assert(frf_r33_id02(b,27,&v)&&v==0x200);
 assert(frf_r33_merge_joy(0x100,0x300)==0x200);
 assert(frf_r33_merge_joy(0x3,0x100)==0x103);
 b[0]=3;assert(!frf_r33_id02(b,27,&v));
 return 0;
}
