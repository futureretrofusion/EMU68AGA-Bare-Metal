#include <assert.h>
#include <stdint.h>
#include "../src/frf_usb/tinyusb/frf_r27_220d.h"
int main(void)
{
    uint8_t p[27]={1,0,15,128,128,128,128,0};
    uint16_t joy=99;int fire=7;
    assert(frf_r27_decode(p,27,&joy,&fire)&&joy==0&&fire==0);
    p[3]=0;assert(frf_r27_decode(p,27,&joy,&fire)&&joy==0x300);
    p[3]=255;assert(frf_r27_decode(p,27,&joy,&fire)&&joy==3);
    p[3]=128;p[4]=0;assert(frf_r27_decode(p,27,&joy,&fire)&&joy==0x100);
    p[4]=255;assert(frf_r27_decode(p,27,&joy,&fire)&&joy==1);
    p[4]=128;p[2]=6;assert(frf_r27_decode(p,27,&joy,&fire)&&joy==0x300);
    p[2]=15;p[1]=1;assert(frf_r27_decode(p,27,&joy,&fire)&&fire==1);
    p[0]=3;assert(!frf_r27_decode(p,27,&joy,&fire)); /* status report cannot zero motion */
    p[0]=1;assert(!frf_r27_decode(p,26,&joy,&fire));
    return 0;
}
