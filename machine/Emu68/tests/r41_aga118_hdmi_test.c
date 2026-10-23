#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "aga/chipset.h"
#include "aga_sync.h"
static void test_frames(int ntsc) {
    unsigned char *ram=(unsigned char *)calloc(1,2*1024*1024);
    assert(ram);
    aga_config_t cfg={0};cfg.ntsc=ntsc;cfg.blit_immediate=1;
    aga_t *a=aga_create(ram,2*1024*1024,&cfg);
    assert(a);
    uint32_t prev=0;
    for(int i=0;i<4;i++){
        aga_run_frame(a);
        uint32_t n=aga_last_frame_cck(a);
        assert(n>0);
        if(ntsc) assert(n>=262u*227u && n<=265u*228u);
        else assert(n>=311u*227u && n<=315u*228u);
        if(i>0 && n!=prev) printf("frame %d mode %s length changed %u to %u clocks\n",i,ntsc?"NTSC":"PAL",prev,n);
        prev=n;
    }
    printf("PASS: %s measured frame %u clocks\n",ntsc?"NTSC":"PAL",prev);
    aga_destroy(a);
    free(ram);
}
static void test_sync(void){
    struct hsync h={0};
    hsync_reset(&h);
    uint32_t a=313u*227u,b=312u*227u,c=310u*227u;
    hsync_len(&h,a);
    hsync_len(&h,b);
    h.blocks=3; h.reach=1; h.steady=1;
    hsync_len(&h,a);
    assert(h.blocks==3 && h.remeasures==0);
    hsync_len(&h,c);
    assert(h.blocks==0 && h.remeasures==1 && h.reach==0 && h.steady==0);
    printf("PASS: frame length change triggers HDMI remeasurement\n");
}
int main(void){test_frames(0);test_frames(1);test_sync();return 0;}
