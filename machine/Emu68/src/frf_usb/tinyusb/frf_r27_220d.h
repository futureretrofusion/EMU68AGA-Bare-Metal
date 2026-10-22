/* R28 candidate: HID 220D:330C gameplay ID01 report, 27 bytes.
 * Verified neutral capture: 01 00 0F 80 80 80 80 00 ... .
 * ID03 packets are not interpreted as directional state.
 */
#ifndef FRF_R27_220D_H
#define FRF_R27_220D_H
#include <stdint.h>
static inline int frf_r27_decode(const uint8_t *b,unsigned n,uint16_t *joy,int *fire)
{
    if(!b || !joy || !fire || n!=27u || b[0]!=1u) return 0;
    const int x=(int)b[3]-128, y=(int)b[4]-128;
    int left=x< -36, right=x>36, up=y< -36, down=y>36;
    /* Neutral hat nibble is 0xF in the physical ID01 capture.
     * Standard compass 0=N, 2=E, 4=S, 6=W is provisional. */
    const unsigned hat=b[2]&0x0fu;
    if(hat<=7u) {
        up    |=hat==0u || hat==1u || hat==7u;
        right |=hat==1u || hat==2u || hat==3u;
        down  |=hat==3u || hat==4u || hat==5u;
        left  |=hat==5u || hat==6u || hat==7u;
    }
    if(left && right)left=right=0;
    if(up && down)up=down=0;
    *joy=(uint16_t)((left?0x0200u:0u) |
                    ((left^up)?0x0100u:0u) |
                    (right?0x0002u:0u) |
                    ((right^down)?0x0001u:0u));
    /* Candidate buttons at byte 1; neutral capture has zero.
     * Do not reuse ID03 bytes as buttons or clear ID01 joystick state. */
    *fire=((b[1]&0x0fu)!=0u) || ((b[2]&0xf0u)!=0u);
    return 1;
}
#endif
