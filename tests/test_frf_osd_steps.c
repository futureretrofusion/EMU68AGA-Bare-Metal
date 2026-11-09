/* Portable RC9 menu steps regression.
 * SPDX-License-Identifier: GPL-2.0-only
 * cc -std=c11 -Wall -Wextra -Werror tests/test_frf_osd_steps.c -o /tmp/frf_osd_test && /tmp/frf_osd_test
 */
#include <stdio.h>
#include "../machine/arm/frf_osd_steps.h"
int main(void)
{
    for (int n=1;n<=5;n++) {
        int right=frf_osd_slot_step(n,1),left=frf_osd_slot_step(n,-1);
        if (right!=(n==5?1:n+1)||left!=(n==1?5:n-1)) return n;
    }
    if(frf_osd_volume_step(-1,-1)!=-1)return 10;
    if(frf_osd_volume_step(4,1)!=4)return 11;
    for(int n=-1;n<4;n++)
        if(frf_osd_volume_step(n,1)!=n+1)return n+20;
    puts("PASS: WHDLoad save-slot menu and HDMI gain/mute steps");
    return 0;
}
