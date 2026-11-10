/* RC11 native PAL reset / P96 handoff policy.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <assert.h>
#include <stdio.h>
#include "../machine/arm/frf_rtg_handoff.h"
int main(void) {
    frf_rtg_handoff h;
    frf_rtg_handoff_init(&h,0x200);
    h.rtg_owns=1;
    frf_rtg_handoff_guest_pal_reset(&h);
    assert(h.force_pal_until_rtg && !h.rtg_owns);
    /* Old RTG list is not allowed to take over before a native PAL frame. */
    for(int i=0;i<25;i++)
        assert(frf_rtg_handoff_pal_gate(&h,0,1,0x300,1));
    assert(!h.rtg_owns);
    assert(frf_rtg_handoff_pal_gate(&h,1,1,0x300,1));
    assert(h.pal_seen_native);
    /* Native still active: background P96 flips don't steal output. */
    assert(frf_rtg_handoff_pal_gate(&h,1,1,0x400,1));
    /* Now guest switched away from PAL to a new RTG screen. */
    assert(!frf_rtg_handoff_pal_gate(&h,0,1,0x400,1));
    assert(h.rtg_owns && !h.force_pal_until_rtg);
    int rtg=frf_rtg_handoff_step(&h,0x400,0x100,0x1a0,0,1,1);
    assert(rtg);
    /* A future native screen may reclaim HDMI. */
    rtg=frf_rtg_handoff_step(&h,0x100,0x100,0x1a0,1,0,1);
    assert(!rtg);
    puts("PASS: warm reset forces PAL; old P96 blocked; new RTG accepted");
    return 0;
}
