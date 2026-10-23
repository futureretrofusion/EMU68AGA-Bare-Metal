/* SPDX-License-Identifier: GPL-2.0-or-later
 * External joystick bit assignment profile.
 * Persistent source: Raspberry Pi FAT32 config via emu68.dtbo properties.
 * Missing/invalid profile falls back to verified R40a low-nibble Fire1.
 */
#include <stdint.h>
#include <devicetree.h>
#include <support.h>
#include "frf_joymap.h"

static int bit_map[4]={-1,-1,-1,-1};
static int profile_active;

static int read_bit(of_node_t *n,const char *name)
{
    of_property_t *p=n?dt_find_property(n,name):0;
    if(!p || p->op_length<4u)return -1;
    uint32_t v=BE32(*(uint32_t*)p->op_value);
    return v<32u?(int)v:-1;
}
void frf_joymap_init(void)
{
    of_node_t *n=dt_find_node("/emu68/joystick");
    bit_map[0]=read_bit(n,"fire1-bit");
    bit_map[1]=read_bit(n,"fire2-bit");
    bit_map[2]=read_bit(n,"fire3-bit");
    bit_map[3]=read_bit(n,"fire4-bit");
    profile_active=n && dt_find_property(n,"profile-enable") &&
                   bit_map[0]>=0;
}
uint32_t frf_joymap_apply(uint32_t decoded)
{
    if(!profile_active)
        return decoded & 0x0fu; /* preserve Fire1/2/3/4 independently */
    uint32_t logical=0;
    for(unsigned i=0;i<4u;i++)
        if(bit_map[i]>=0 && (decoded&(1u<<(unsigned)bit_map[i])))
            logical|=1u<<i;
    return logical;
}
void frf_joymap_get(int out_bits[4],int *active)
{
    if(out_bits)for(unsigned i=0;i<4u;i++)out_bits[i]=bit_map[i];
    if(active)*active=profile_active;
}
