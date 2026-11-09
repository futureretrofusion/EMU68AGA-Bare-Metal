/* FRF standalone AGA OSD: portable menu value stepping.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef FRF_OSD_STEPS_H
#define FRF_OSD_STEPS_H

/* One game's five WHDLoad save-state slots. */
static inline int frf_osd_slot_step(int current,int dir)
{
    if(current<1 || current>5) current=1;
    int n=current+dir;
    if(n<1)return 5;
    if(n>5)return 1;
    return n;
}

/* HDMI volume -1 mute, index 0..4 (100%,150%,200%,300%,400%). */
static inline int frf_osd_volume_step(int current,int dir)
{
    int next=current+dir;
    return next<-1?-1:next>4?4:next;
}
#endif
