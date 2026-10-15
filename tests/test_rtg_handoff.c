#include "frf_rtg_handoff.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    frf_rtg_handoff h;
    frf_rtg_handoff_init(&h, 0x15);
    for (int i=0;i<30;i++)
        assert(!frf_rtg_handoff_step(&h, 0x400, 0x400,0x4a0,1,0,1));
    /* P96 switches HDMI while Amiga-native DMA may still be active. */
    assert(frf_rtg_handoff_step(&h, 0x150, 0x400,0x4a0,1,1,1));
    assert(h.takeovers==1);
    for (int i=0;i<10;i++)
        assert(frf_rtg_handoff_step(&h, 0x150,0x400,0x4a0,1,1,0));
    assert(frf_rtg_handoff_step(&h, 0x160,0x400,0x4a0,0,1,0));
    /* Native game starts; edge causes controlled AGA reclaim. */
    assert(!frf_rtg_handoff_step(&h, 0x160,0x400,0x4a0,1,1,0));
    assert(h.returns==1);
    assert(!frf_rtg_handoff_step(&h, 0x400,0x400,0x4a0,1,0,1));
    /* Returning to P96; the same RTG display list works again. */
    assert(frf_rtg_handoff_step(&h, 0x160,0x400,0x4a0,0,1,1));
    assert(h.takeovers==2);
    for (int i=0;i<20;i++)
        assert(frf_rtg_handoff_step(&h, 0x160,0x400,0x4a0,0,1,0));
    /* Startup firmware list isn't mistaken for P96. */
    frf_rtg_handoff_init(&h,0x15);
    for(int i=0;i<30;i++)
        assert(!frf_rtg_handoff_step(&h,0x15,0x400,0x4a0,1,1,0));
    /* Invalid/readless lists do not steal output. */
    assert(!frf_rtg_handoff_step(&h,0x700,0x400,0x4a0,1,0,1));
    printf("PASS: P96 HDMI takeover, AGA native reclaim, RTG return and firmware-list guard\n");
    return 0;
}
