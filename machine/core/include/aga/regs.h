/* AGA-PISTORM — Amiga custom register map (offsets from 0xDFF000) and bit definitions.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_REGS_H
#define AGA_REGS_H

#define CUSTOM_BASE   0xDFF000u

/* Read-only */
#define BLTDDAT   0x000
#define DMACONR   0x002
#define VPOSR     0x004
#define VHPOSR    0x006
#define DSKDATR   0x008
#define JOY0DAT   0x00A
#define JOY1DAT   0x00C
#define CLXDAT    0x00E
#define ADKCONR   0x010
#define POT0DAT   0x012
#define POT1DAT   0x014
#define POTGOR    0x016
#define SERDATR   0x018
#define DSKBYTR   0x01A
#define INTENAR   0x01C
#define INTREQR   0x01E
/* Write-only */
#define DSKPTH    0x020
#define DSKPTL    0x022
#define DSKLEN    0x024
#define DSKDAT    0x026
#define REFPTR    0x028
#define VPOSW     0x02A
#define VHPOSW    0x02C
#define COPCON    0x02E
#define SERDAT    0x030
#define SERPER    0x032
#define POTGO     0x034
#define JOYTEST   0x036
#define STREQU    0x038
#define STRVBL    0x03A
#define STRHOR    0x03C
#define STRLONG   0x03E
#define BLTCON0   0x040
#define BLTCON1   0x042
#define BLTAFWM   0x044
#define BLTALWM   0x046
#define BLTCPTH   0x048
#define BLTCPTL   0x04A
#define BLTBPTH   0x04C
#define BLTBPTL   0x04E
#define BLTAPTH   0x050
#define BLTAPTL   0x052
#define BLTDPTH   0x054
#define BLTDPTL   0x056
#define BLTSIZE   0x058
#define BLTCON0L  0x05A
#define BLTSIZV   0x05C
#define BLTSIZH   0x05E
#define BLTCMOD   0x060
#define BLTBMOD   0x062
#define BLTAMOD   0x064
#define BLTDMOD   0x066
#define BLTCDAT   0x070
#define BLTBDAT   0x072
#define BLTADAT   0x074
#define SPRHDAT   0x078
#define BPLHDAT   0x07A
#define DENISEID  0x07C
#define DSKSYNC   0x07E
#define COP1LCH   0x080
#define COP1LCL   0x082
#define COP2LCH   0x084
#define COP2LCL   0x086
#define COPJMP1   0x088
#define COPJMP2   0x08A
#define COPINS    0x08C
#define DIWSTRT   0x08E
#define DIWSTOP   0x090
#define DDFSTRT   0x092
#define DDFSTOP   0x094
#define DMACON    0x096
#define CLXCON    0x098
#define INTENA    0x09A
#define INTREQ    0x09C
#define ADKCON    0x09E
#define AUD0LCH   0x0A0   /* AUDxLCH = 0xA0 + x*16 */
#define AUD0LCL   0x0A2
#define AUD0LEN   0x0A4
#define AUD0PER   0x0A6
#define AUD0VOL   0x0A8
#define AUD0DAT   0x0AA
#define BPL1PTH   0x0E0   /* BPLxPTH = 0xE0 + (x-1)*4 */
#define BPL1PTL   0x0E2
#define BPLCON0   0x100
#define BPLCON1   0x102
#define BPLCON2   0x104
#define BPLCON3   0x106
#define BPL1MOD   0x108
#define BPL2MOD   0x10A
#define BPLCON4   0x10C
#define CLXCON2   0x10E
#define BPL1DAT   0x110   /* BPLxDAT = 0x110 + (x-1)*2 */
#define SPR0PTH   0x120   /* SPRxPTH = 0x120 + x*4 */
#define SPR0PTL   0x122
#define SPR0POS   0x140   /* SPRxPOS = 0x140 + x*8, CTL +2, DATA +4, DATB +6 */
#define SPR0CTL   0x142
#define SPR0DATA  0x144
#define SPR0DATB  0x146
#define COLOR00   0x180   /* COLORxx = 0x180 + x*2 */
#define HTOTAL    0x1C0
#define HSSTOP    0x1C2
#define HBSTRT    0x1C4
#define HBSTOP    0x1C6
#define VTOTAL    0x1C8
#define VSSTOP    0x1CA
#define VBSTRT    0x1CC
#define VBSTOP    0x1CE
#define SPRHSTRT  0x1D0
#define SPRHSTOP  0x1D2
#define BPLHSTRT  0x1D4
#define BPLHSTOP  0x1D6
#define HHPOSW    0x1D8
#define HHPOSR    0x1DA
#define BEAMCON0  0x1DC
#define HSSTRT    0x1DE
#define VSSTRT    0x1E0
#define HCENTER   0x1E2
#define DIWHIGH   0x1E4
#define BPLHMOD   0x1E6
#define SPRHPTH   0x1E8
#define SPRHPTL   0x1EA
#define BPLHPTH   0x1EC
#define BPLHPTL   0x1EE
#define FMODE     0x1FC
#define NOOP      0x1FE

/* DMACON bits */
#define DMAF_SETCLR  0x8000
#define DMAF_BBUSY   0x4000
#define DMAF_BZERO   0x2000
#define DMAF_BLTPRI  0x0400
#define DMAF_DMAEN   0x0200
#define DMAF_BPLEN   0x0100
#define DMAF_COPEN   0x0080
#define DMAF_BLTEN   0x0040
#define DMAF_SPREN   0x0020
#define DMAF_DSKEN   0x0010
#define DMAF_AUD3EN  0x0008
#define DMAF_AUD2EN  0x0004
#define DMAF_AUD1EN  0x0002
#define DMAF_AUD0EN  0x0001
#define DMAF_AUDEN   0x000F

/* INTENA / INTREQ bits */
#define INTF_SETCLR  0x8000
#define INTF_INTEN   0x4000
#define INTF_EXTER   0x2000   /* level 6 */
#define INTF_DSKSYN  0x1000   /* level 5 */
#define INTF_RBF     0x0800   /* level 5 */
#define INTF_AUD3    0x0400   /* level 4 */
#define INTF_AUD2    0x0200
#define INTF_AUD1    0x0100
#define INTF_AUD0    0x0080
#define INTF_BLIT    0x0040   /* level 3 */
#define INTF_VERTB   0x0020   /* level 3 */
#define INTF_COPER   0x0010   /* level 3 */
#define INTF_PORTS   0x0008   /* level 2 */
#define INTF_SOFTINT 0x0004   /* level 1 */
#define INTF_DSKBLK  0x0002
#define INTF_TBE     0x0001

/* BPLCON0 */
#define BPLCON0_HIRES   0x8000
#define BPLCON0_BPU0    0x1000  /* BPU2..0 = bits 14..12 */
#define BPLCON0_HAM     0x0800
#define BPLCON0_DPF     0x0400
#define BPLCON0_COLOR   0x0200
#define BPLCON0_GAUD    0x0100
#define BPLCON0_UHRES   0x0080
#define BPLCON0_SHRES   0x0040
#define BPLCON0_BYPASS  0x0020
#define BPLCON0_BPU3    0x0010
#define BPLCON0_LPEN    0x0008
#define BPLCON0_LACE    0x0004
#define BPLCON0_ERSY    0x0002
#define BPLCON0_ECSENA  0x0001

/* BPLCON2 */
#define BPLCON2_ZDBPSEL 0x7000
#define BPLCON2_ZDBPEN  0x0800
#define BPLCON2_ZDCTEN  0x0400
#define BPLCON2_KILLEHB 0x0200
#define BPLCON2_RDRAM   0x0100
#define BPLCON2_SOGEN   0x0080
#define BPLCON2_PF2PRI  0x0040
#define BPLCON2_PF2P    0x0038
#define BPLCON2_PF1P    0x0007

/* BPLCON3 */
#define BPLCON3_BANK     0xE000  /* palette bank, bits 15..13 */
#define BPLCON3_PF2OF    0x1C00  /* playfield 2 colour offset, bits 12..10 */
#define BPLCON3_LOCT     0x0200
#define BPLCON3_SPRES    0x00C0
#define BPLCON3_BRDRBLNK 0x0020
#define BPLCON3_BRDNTRAN 0x0010
#define BPLCON3_ZDCLKEN  0x0004
#define BPLCON3_BRDSPRT  0x0002
#define BPLCON3_EXTBLKEN 0x0001

/* BPLCON4 */
#define BPLCON4_BPLAM    0xFF00
#define BPLCON4_ESPRM    0x00F0
#define BPLCON4_OSPRM    0x000F

/* FMODE */
#define FMODE_SSCAN2  0x8000
#define FMODE_BSCAN2  0x4000
#define FMODE_SPAGEM  0x0008
#define FMODE_SPR32   0x0004
#define FMODE_BPAGEM  0x0002
#define FMODE_BPL32   0x0001

/* BEAMCON0: the HRM's bit names, laid out as in WinUAE's include/custom.h */
#define BEAMCON0_HARDDIS  0x4000
#define BEAMCON0_LPENDIS  0x2000
#define BEAMCON0_VARVBEN  0x1000
#define BEAMCON0_LOLDIS   0x0800
#define BEAMCON0_CSCBEN   0x0400
#define BEAMCON0_VARVSYEN 0x0200
#define BEAMCON0_VARHSYEN 0x0100
#define BEAMCON0_VARBEAMEN 0x0080
#define BEAMCON0_DISPLAYDUAL 0x0040
#define BEAMCON0_DISPLAYPAL 0x0020
#define BEAMCON0_VARCSYEN 0x0010
#define BEAMCON0_BLANKEN  0x0008
#define BEAMCON0_CSYTRUE  0x0004
#define BEAMCON0_VSYTRUE  0x0002
#define BEAMCON0_HSYTRUE  0x0001

/* Chipset identification (VPOSR bits 14..8) */
#define VPOSR_ID_AGA_PAL   0x2300   /* Alice 8374 PAL rev 3/4: 0x23 */
#define VPOSR_ID_AGA_NTSC  0x3300   /* ... NTSC: 0x33 - bit 12 is the PAL/NTSC strap */
#define VPOSR_LOL          0x0080   /* this line is a long one (NTSC: 228 colour clocks) */
#define DENISEID_LISA      0x00F8

/* BLTCON0 / BLTCON1 */
#define BLTCON0_ASH   0xF000
#define BLTCON0_USEA  0x0800
#define BLTCON0_USEB  0x0400
#define BLTCON0_USEC  0x0200
#define BLTCON0_USED  0x0100
#define BLTCON1_BSH   0xF000
#define BLTCON1_DOFF  0x0080
#define BLTCON1_EFE   0x0010
#define BLTCON1_IFE   0x0008
#define BLTCON1_FCI   0x0004
#define BLTCON1_DESC  0x0002
#define BLTCON1_LINE  0x0001
#define BLTCON1_SIGN  0x0040
#define BLTCON1_SUD   0x0010
#define BLTCON1_SUL   0x0008
#define BLTCON1_AUL   0x0004
#define BLTCON1_SING  0x0002

/* Beam geometry */
#define AGA_PAL_HPOS      227     /* colour clocks per line (0..226) */
#define AGA_NTSC_HPOS     227     /* a short line; NTSC alternates 227 and 228 (LOL) */
#define AGA_PAL_VPOS      312     /* lines per frame, +1 when LOF */
#define AGA_NTSC_VPOS     262
#define AGA_VBLANK_END_PAL  0x1A  /* first line after vertical blank */
#define AGA_VBLANK_END_NTSC 0x15
/* Sprites restart before the display does, on their own absolute lines:
   UAE src/include/custom.h has VBLANK_SPRITE_PAL 25 and VBLANK_SPRITE_NTSC 20,
   independent of its display constants (which are 27 and 28 there). WinUAE's
   DMA records confirm 25 for PAL: the first SPRxPOS/SPRxCTL pair of a frame is
   fetched on that line. */
#define AGA_SPRITE_VBLANK_PAL  25
#define AGA_SPRITE_VBLANK_NTSC 20

#endif /* AGA_REGS_H */
