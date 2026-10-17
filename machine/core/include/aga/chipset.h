/* AGA-PISTORM - portable AGA chipset core, public API.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The core is single-threaded by design: all entry points must be called with
 * the chipset lock held (host build: no lock; Emu68 build: a spinlock shared by
 * the CPU trap handler on core 0 and the chipset loop on core 3).
 *
 * Coordinates:
 *   hpos  colour clock within the line, 0..226 (PAL)  (1 cck = 2 lores pixels);
 *         NTSC lines alternate 0..227 (long, VPOSR LOL) and 0..226
 *   vpos  line within the frame, 0..311 (+1 with LOF)
 *   Output framebuffer is in hires pixels (4 per cck) covering lores pixel
 *   columns [AGA_OUT_LEFT_LORES, AGA_OUT_LEFT_LORES + AGA_OUT_W/2) and lines
 *   [AGA_OUT_TOP, AGA_OUT_TOP + AGA_OUT_H).
 */
#ifndef AGA_CHIPSET_H
#define AGA_CHIPSET_H

#include <stdint.h>
#include <stddef.h>
#include "aga/regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGA_MAX_PLANES     8
#define AGA_MAX_SPRITES    8
#define AGA_MAX_DRIVES     2
#define AGA_OUT_LEFT_LORES 0x50            /* first visible lores pixel column  */
#define AGA_OUT_W          800             /* hires pixels per output line (400 lores) */
#define AGA_OUT_TOP        0x1A            /* first visible line (PAL)          */
#define AGA_OUT_H          286             /* lines 0x1A..0x137                 */
#define AGA_MAX_HPOS       228
#define AGA_MAX_VPOS       314
#define AGA_LINE_CHANGES   512             /* max recorded register changes per line */
#define AGA_PLANE_WORDS    40              /* uint64 words of planar data per plane per line (2560 bits) */
#define AGA_MAX_BLOCKS     128             /* max fetch blocks per line (fetchstart 2 cck) */

typedef struct aga aga_t;

typedef struct aga_hooks {
    /* Passthrough for registers served by the real chipset (JOYxDAT, POTx, SERDATR...).
       May be NULL: then the core returns/absorbs defaults. `reg` is the offset 0x000..0x1FE. */
    uint16_t (*ext_read)(void *user, uint32_t reg);
    void     (*ext_write)(void *user, uint32_t reg, uint16_t value);
    /* Called whenever the interrupt priority level requested by the virtual Paula changes. */
    void     (*ipl_changed)(void *user, int ipl);
    /* Called at the beginning of vertical blank: the framebuffer for the finished frame is ready. */
    void     (*frame_done)(void *user);
    void     *user;
    /* Called after each line is drawn - not for a woven frame's lines - with
       the line still in the core's own (cached) buffer, AGA_OUT_W pixels: y
       its frame row, page the framebuffer it went to (aga_set_framebuffers'
       order). May be NULL. Last, so a positional initializer of the fields
       above leaves it NULL. */
    void     (*line_done)(void *user, int y, int page, const uint32_t *line);
} aga_hooks_t;

typedef struct aga_config {
    int ntsc;               /* 0 = PAL (default), 1 = NTSC */
    int blit_immediate;     /* 1 = blits complete at BLTSIZE write (default) */
    int collisions;         /* 1 = always work out CLXDAT; 0 (default) = only while the program reads it */
    int render;             /* 0 = skip pixel rendering (headless timing only) */
    int weave;              /* 1 = interlaced frames woven, both fields at once (aga_set_weave) */
    int bright;             /* the picture's brightness in percent; 0 or 100 = the palette's own */
    int gamma;              /* the picture's gamma: 0 Exact (the palette's own), 1 CRT, 2 Lighter */
} aga_config_t;

/* ---- lifecycle ------------------------------------------------------------ */
aga_t   *aga_create(uint8_t *chipram, uint32_t chipram_size, const aga_config_t *cfg);
void     aga_destroy(aga_t *a);
void     aga_set_hooks(aga_t *a, const aga_hooks_t *hooks);
void     aga_reset(aga_t *a);

/* Chipset personality. Off (the default) is AGA. On presents ECS Denise to the
   game: BPLCON3 and BPLCON4 read as their reset values, so a COLORxx write is
   one whole 12-bit colour with no LOCT and no colour bank, and sprites take
   colours 16-31. Some ECS games come out at a sixteenth brightness on the AGA
   chipset because BPLCON3's LOCT bit is set while they load their palette,
   which puts every colour in the low nibbles. My AGA is faithful here -
   WinUAE's COLOR_WRITE does the same; the writer was graphics.library in AA
   mode after WHDLoad's SetChipRev, which agaboot stubs now.
   Survives aga_reset(), because the sandbox resets on entry. A program given
   ECS that turns out to draw for AGA gets AGA after all (playfield.c
   ecs_says_aga), unless it was forced: aga_get_ecs() then reads 0 again. */
void     aga_set_ecs(aga_t *a, int on);
void     aga_set_ecs_forced(aga_t *a, int on);   /* the user's "always ECS": never given up */
int      aga_get_ecs(const aga_t *a);
/* What an ECS Denise has seen of AGA this session, for the kernel log:
   bit 0 an AGA fetch (FMODE, eight planes), bit 1 an AGA colour (LOCT, a
   bank, BPLCON4), bit 2 24-bit colour pairs in two banks, bit 3 24-bit
   pairs in bank 0 taken the AGA way; bit 7 the user's "always ECS". */
unsigned aga_ecs_signs(const aga_t *a);
/* Forget those signs: what the OS drew before a game took the machine -
   WHDLoad's splash, by an AA graphics.library - says nothing about the game.
   Returns what was forgotten (aga_ecs_signs bits 0-2). */
unsigned aga_ecs_forget_signs(aga_t *a);

/* The flicker fixer. On, an interlaced frame (BPLCON0 LACE) is drawn into the
   two pages taken as ONE frame of twice the height: its field on every other
   row - the long field on the even rows, the one a CRT draws on top - with the
   field before it on the rows between, so a still picture stands still and
   sharp, as a flicker fixer shows it. The first woven frame puts each line on
   both rows. Needs the two pages to be one block, as the core's own and the
   ones aga_set_framebuffers is given are. Survives aga_reset(). */
void     aga_set_weave(aga_t *a, int on);
/* 1 when the last completed frame was drawn woven: the frame is then the first
   page with 2 x AGA_OUT_H rows, and *row (if not NULL) says which rows are its
   own field - 0 the even ones (a long field), 1 the odd ones. */
int      aga_frame_woven(const aga_t *a, int *row);
/* Brightness in percent, 100 = off, and Gamma (aga_set_gamma): every colour
   shown goes through one table per channel (aga_tone_build) - for scanlines,
   which darken
   the picture. The palette the game reads back is not touched. Survives
   aga_reset(). */
void     aga_set_brightness(aga_t *a, int percent);
/* 0 Exact, 1 CRT (the 1084S's darker mid-tones), 2 Lighter: see aga_tone_build.
   Survives aga_reset() too. */
void     aga_set_gamma(aga_t *a, int gamma);
/* The table a channel goes through for a brightness and a gamma: lut[v] for v
   0..255, RGB2RTG's curve - the gamma, then the gain up to a knee and from
   there a smooth bend to white, so near-whites stay apart where straight gain
   clipped everything above 255 / gain. Returns 1 when it is not the identity.
   Integer only: it runs in the 68k's trap (a control word) as well. */
int      aga_tone_build(uint8_t lut[256], int percent, int gamma);
/* PAL (0) or NTSC (1) from the next aga_reset() on - the standard a game
   starts in, as the sandbox resets on entry. A game can still switch itself
   with BEAMCON0. aga_is_ntsc: the standard in force now. The setting is
   also the chip's PAL/NTSC strap from that reset on: VPOSR's ID reads $33
   for NTSC, $23 for PAL, whatever BEAMCON0 says. */
void     aga_set_ntsc(aga_t *a, int on);
int      aga_is_ntsc(const aga_t *a);

/* ---- CPU side: custom register access ------------------------------------ */
/* addr may be a full address (0xDFFxxx) or an offset (0..0x1FF). size = 1, 2 or 4. */
uint32_t aga_read(aga_t *a, uint32_t addr, int size);
void     aga_write(aga_t *a, uint32_t addr, uint32_t value, int size);
/* Word-granular primitives used by the copper and the accessors above. A read
   is the 68k's read and has its side effects: CLXDAT clears, COPJMP1/2
   restart the copper. Diagnostics read aga_peek_reg or the R registers. */
uint16_t aga_custom_rget(aga_t *a, uint32_t reg);
void     aga_custom_wput(aga_t *a, uint32_t reg, uint16_t value);

/* ---- chipset side: advancing time ---------------------------------------- */
void     aga_run_cycles(aga_t *a, int cck);    /* advance the beam by N colour clocks */
void     aga_run_line(aga_t *a);               /* advance to the start of the next line */
void     aga_run_frame(aga_t *a);              /* advance to the start of the next frame */

/* ---- status / output ------------------------------------------------------ */
/* 1 when the native display should be shown: the last frame had bitplane data
   inside the display window, or the AGA control register (0xDFF1F2) says so.
   AGA control register: bit 0 = 1 RTG in front (hide native), 0 native in front;
   bit 15 = 1 use the register instead of the automatic detection. */
int      aga_native_active(const aga_t *a);
int      aga_last_frame_display_lines(const aga_t *a);
int      aga_last_frame_had_data(const aga_t *a);   /* any non-zero bitplane fetch in the last frame */
/* the display window on the last frame's lines with a picture: hires pixels
   [x0, x1) and lines y0..y1 of the AGA_OUT_W x AGA_OUT_H frame (x may lie
   outside it); 0 when no line had one */
int      aga_last_frame_window(const aga_t *a, int *x0, int *x1, int *y0, int *y1);
uint32_t aga_blit_count(const aga_t *a);            /* blits started since reset */
void     aga_audio_stats(const aga_t *a, uint32_t *on, uint32_t *words, uint32_t *nz);  /* 4 each */
void     aga_audio_set_mask(aga_t *a, int mask);    /* bit n: mix channel n */
void     aga_audio_triggers(const aga_t *a, uint32_t *out, int *count);  /* 32 x 6 */
void     aga_audio_volstats(const aga_t *a, uint32_t *wr, uint32_t *nz, uint32_t *dmaoff);
void     aga_audio_wrapstats(const aga_t *a, uint32_t *wrap, uint32_t *irq);
void     aga_audio_wrapinfo(const aga_t *a, uint32_t *out, int *count);  /* 32 x 4 */
void     aga_audio_vols(const aga_t *a, uint32_t *vol, uint32_t *volmax, uint32_t *audible);
int      aga_blit_pending(aga_t *a);                /* a blit is waiting to report finished */
uint32_t aga_blit_duration(aga_t *a);              /* colour clocks the real blitter would take over it */
uint32_t aga_peek_blit_irq(const aga_t *a, int i); /* 0 finished, 1 swallowed interrupts, 2 frame of the last */
void     aga_blit_set_nohold(aga_t *a, int on);   /* A/B: no held completions (1.102 behaviour) */
void     aga_blit_complete(aga_t *a);               /* the host's timer says enough time has passed */
/* Windowed presentation: control register bit 1 set, rectangle from registers
   0xDFF1F4 (x), 0xDFF1F6 (y), 0xDFF1F8 (w), 0xDFF1FA (h) in RTG screen pixels. Returns 1 if windowed. */
int      aga_window_mode(const aga_t *a, int *x, int *y, int *w, int *h);
int      aga_get_ipl(const aga_t *a);
int      aga_get_vpos(const aga_t *a);
int      aga_get_hpos(const aga_t *a);
int      aga_frame_count(const aga_t *a);
int      aga_lines_per_frame(const aga_t *a);
int      aga_cycles_per_line(const aga_t *a);     /* this line's: NTSC lines alternate 227 and 228 */
/* Last completed frame, AGA_OUT_W x AGA_OUT_H, 0x00RRGGBB. */
const uint32_t *aga_framebuffer(const aga_t *a);
/* Frame currently being drawn (for live scanout, line `vpos` complete after aga_run_line). */
uint32_t *aga_framebuffer_live(aga_t *a);
void      aga_set_framebuffers(aga_t *a, uint32_t *f0, uint32_t *f1);   /* render into the caller's pages */
void      aga_set_framebuffers3(aga_t *a, uint32_t *f0, uint32_t *f1, uint32_t *f2); /* tear-free HVS triple buffer */

/* ---- audio ---------------------------------------------------------------- */
/* Pull `frames` stereo 16-bit samples at `rate` Hz. Returns frames produced. */
int      aga_audio_pull(aga_t *a, int16_t *lr, int frames, int rate);
/* Paula's steps, for HDMI's band-limited Paula (arm/aga_blep.h): each
   channel's level - its sample times its volume, 0 while it is silent,
   masked or modulating another - from the colour clock it changes at,
   counted from boot. Logged only while switched on (a channel's level is
   logged afresh then). aga_audio_steps takes all that are there, in the
   order they were made, and says the colour clock up to which every step is
   in: later ones come at it or after. */
typedef struct aga_step { uint64_t t; int16_t level; uint8_t ch, pad[5]; } aga_step_t;
#define AGA_STEP_RING 8192
void     aga_audio_steps_on(aga_t *a, int on);
int      aga_audio_steps(aga_t *a, aga_step_t *out, int max, uint64_t *now);

/* ---- virtual floppy drives (ADF images) ----------------------------------- */
/* While any virtual disk is inserted the DSK* registers are emulated (real
   drives are unreachable); otherwise they are forwarded to ext_write/ext_read. */
int      aga_disk_insert(aga_t *a, int drive, const uint8_t *adf, uint32_t size, int wprot); /* copies; 0 = ok */
void     aga_disk_eject(aga_t *a, int drive);
int      aga_disk_inserted(const aga_t *a, int drive);
int      aga_disk_active(const aga_t *a);
const uint8_t *aga_disk_image(const aga_t *a, int drive, uint32_t *size, int *dirty);
/* drive control lines: mirror CIA-B PRB writes; substitute CIA-A PRA bits 2-5 when it returns 1 */
void     aga_disk_cia_prb(aga_t *a, uint8_t prb);
int      aga_disk_cia_pra(aga_t *a, uint8_t *bits);
uint8_t  aga_disk_cia_prb_mask(const aga_t *a);   /* SELx bits to keep high on the real CIA */

/* ---- state injection (snapshots) ----------------------------------------- */
void     aga_set_color(aga_t *a, int index, uint32_t rgb);          /* palette entry 0..255 */
/* Load a WinUAE-style custom register image (256 x u16, offsets 0..0x1FE) as
   frame-start state: write-only registers are written, strobes skipped,
   SET/CLR registers loaded absolutely. Beam is reset to line 0. */
void     aga_load_registers(aga_t *a, const uint16_t *regs);

/* ---- DMA slot trace (build the core with -DAGA_TRACE) --------------------- */
/* One entry per (line, colour clock) of the frame being drawn, laid out as
   [AGA_MAX_VPOS][AGA_MAX_HPOS]. type 0 means the slot was free. The codes match
   the ones WinUAE writes into its profile DMA records, so a trace can be
   diffed against a snapshot directly (tools/tracecmp.py). */
#define AGA_DMA_FREE     0
#define AGA_DMA_REFRESH  1
#define AGA_DMA_CPU      2
#define AGA_DMA_COPPER   3
#define AGA_DMA_AUDIO    4
#define AGA_DMA_BLITTER  5
#define AGA_DMA_BITPLANE 6
#define AGA_DMA_SPRITE   7
#define AGA_DMA_DISK     8

typedef struct aga_slot {
    uint8_t  type;
    uint16_t reg;      /* the register the access belongs to */
    uint32_t addr;     /* chip address read or written */
} aga_slot_t;

/* NULL unless the core was built with AGA_TRACE. Valid until the next frame. */
const aga_slot_t *aga_dma_trace(const aga_t *a);

/* ---- debug ---------------------------------------------------------------- */
/* Lock-free beam read: VPOSR/VHPOSR straight from vpos/hpos, no mutex. These
   are the hottest registers a game touches (every short delay is a beam wait),
   and serialising them against the chipset thread costs more than the read. */
int      aga_is_beam_reg(uint32_t reg);
uint32_t aga_peek_beam(const aga_t *a, uint32_t reg, int size);
/* same, plus the colour clocks elapsed since the chipset thread last stepped,
   so a beam wait sees every position and not only multiples of the step */
uint32_t aga_peek_beam_at(const aga_t *a, uint32_t reg, int size, uint32_t add_cck, int same_line);
uint16_t aga_peek_reg(const aga_t *a, uint32_t reg);
uint16_t aga_peek_dmacon(const aga_t *a);   /* the live DMACON, not an accumulation */
uint32_t aga_peek_dmacon_log(const aga_t *a, int i);  /* the last 8 writes, oldest first */
uint32_t aga_peek_beamcon0_log(const aga_t *a, int i); /* last 8, oldest first */
uint32_t aga_peek_audirq(const aga_t *a, int i);     /* audio interrupts by source */
uint32_t aga_peek_copdbg(const aga_t *a, int i);     /* copper restart history, copper BPLCON0 writes */
void     aga_dbg_event_cpu(aga_t *a, uint32_t reg, uint16_t value);   /* a 68k register write, for the event log */
uint32_t aga_peek_evlog(const aga_t *a, int i);      /* 0: count, then A,B pairs */
uint32_t aga_peek_fetch_stat(const aga_t *a, int i);  /* 0 lines, 1 no window, 2 no DMA,
                                                         3 no planes, 4 too many, 5 fetched */
void     aga_set_frame_skip(aga_t *a, int on);   /* skip drawing whole frames while the host is behind */
uint32_t aga_frames_skipped(const aga_t *a);
void     aga_set_render(aga_t *a, int on);   /* 0 = timing only, no pixels: for a board with no video output */   /* last value written to a register */
uint32_t aga_peek_color(const aga_t *a, int index);     /* 24-bit palette entry 0..255 */
uint32_t aga_peek_bplpt(const aga_t *a, int plane);
uint32_t aga_peek_copper_pc(const aga_t *a);
void     aga_set_log(aga_t *a, void (*log)(void *user, const char *msg), void *user);

#ifdef __cplusplus
}
#endif
#endif /* AGA_CHIPSET_H */
