/* AmigaOS 3.x shell joystick trace for EMU68AGA virtual hardware.
 * Read-only: $DFF00C JOY1DAT and CIAA PRA $BFE001 (bit7 port2 fire).
 * Use on an actual Amiga or EMU68AGA as a normal 68000 shell program.
 */
#include <stdio.h>
#include <stdint.h>
#include <proto/dos.h>
int main(void)
{
    volatile const uint16_t *const joy1=(volatile const uint16_t *)0x00dff00cUL;
    volatile const uint8_t *const ciaa=(volatile const uint8_t *)0x00bfe001UL;
    uint16_t last_joy=0xffffu;
    uint8_t last_fire=0xffu;
    printf("EMU68AGA USB Joystick Port 2 Test (JOY1DAT / CIAAPRA)\n");
    printf("Move D-pad and press A/fire. Values print only on change.\n");
    fflush(stdout);
    for(unsigned i=0;i<600u;++i) {
        const uint16_t j=*joy1;
        const uint8_t fire=(uint8_t)((*ciaa&0x80u)==0u);
        if(j!=last_joy || fire!=last_fire) {
            printf("JOY1DAT=$%04X  FIRE=%u  (CIAA.PRA bit7)\n",(unsigned)j,(unsigned)fire);
            fflush(stdout);
            last_joy=j;last_fire=fire;
        }
        Delay(3u);  /* 3 VBlank ticks = ~60 ms PAL.  ~36s total */
    }
    puts("Test complete.");
    return 0;
}
