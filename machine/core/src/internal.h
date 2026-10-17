/* AGA-PISTORM - chipset core private state.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Many names follow UAE's (the sprite, audio, fetch and copper state). The
 * copper states and fields are the classic UAE copper's (E-UAE custom.c),
 * GET_PLANES is UAE's macro (E-UAE include/custom.h) and GET_RES follows
 * WinUAE's include/custom.h.
 *   E-UAE custom.c:          Copyright 1995-2002 Bernd Schmidt,
 *     Copyright 1995 Alessandro Bissacco, Copyright 2000-2004 Toni Wilen
 *   E-UAE include/custom.h:  (c) 1995 Bernd Schmidt
 *   WinUAE include/custom.h: (c) 1995 Bernd Schmidt
 * Changed from the UAE/WinUAE code for AGA-PISTORM in 2026.
 * UAE and WinUAE are GPL, like this file.
 */
#ifndef AGA_INTERNAL_H
#define AGA_INTERNAL_H

#include <stdint.h>
#include <string.h>
#include "aga/chipset.h"

/* ---- chip RAM access (big-endian) ---------------------------------------- */
static inline uint16_t chip_rd16(const aga_t *a, uint32_t addr);
static inline uint32_t chip_rd32(const aga_t *a, uint32_t addr);
static inline uint64_t chip_rd64(const aga_t *a, uint32_t addr);
static inline void     chip_wr16(aga_t *a, uint32_t addr, uint16_t v);

/* a CLXDAT read keeps collision detection on for this many frames (2 s PAL) */
#define CLX_FRAMES 100

#define RES_LORES 0
#define RES_HIRES 1
#define RES_SHRES 2

/* copper states */
enum {
    COP_stop = 0,
    COP_read1,          /* fetch first word */
    COP_read2,          /* fetch second word */
    COP_wait_in2,       /* WAIT: 2 cycles delay before compare */
    COP_wait,           /* waiting for beam position */
    COP_skip_in2,
    COP_bltwait,        /* waiting for blitter */
    COP_strobe_delay,   /* COPJMP written: restart after a fetch cycle */
};

typedef struct copper {
    uint32_t cop1lc, cop2lc;
    uint32_t pc;
    uint16_t i1, i2;
    int      state;
    int      vcmp, hcmp, hmask, vmask, blitwait;
    int      danger;            /* COPCON CDANG */
    int      enabled_line;      /* copper DMA active on this line */
    int      strobe;            /* pending COPJMP1/2 written by the copper itself (1/2), 0 = none */
    int      strobe_count;      /* instructions still to complete before it takes effect */
    int      in_move;           /* cop_sync is executing a MOVE (aga_custom_wput in progress) */
    int      reload;            /* 1/2: take pc from COP1LC/COP2LC at the next fetch (vblank, CPU COPJMP) */
    int      ignore_next;
    int      delay;             /* idle steps before the next access */
    int      last_hpos;         /* copper synced up to this hpos (even) */
    /* a WAIT on its own line: the colour clock it wakes at (cck_step leaves
       the copper alone before it) - known for line wake_seq (line_no), from
       colour clock wake_from (cop_wait_wake) */
    int      wake_ok;
    uint32_t wake_seq;
    int      wake_from, wake_h;
} copper_t;

typedef struct blitter {
    uint16_t con0, con1, afwm, alwm;
    uint32_t apt, bpt, cpt, dpt;
    int16_t  amod, bmod, cmod, dmod;
    uint16_t adat, bdat, cdat, ddat;
    uint16_t sizev, sizeh;        /* pending BLTSIZV value / lines */
    int      busy, zero;
    uint32_t irq_cck;       /* colour clocks left before the blit reports finished */
    uint32_t dur_cck;       /* how long the real blitter takes over this blit */
    uint32_t fin_count;     /* blits reported finished (BLIT raised) */
    uint32_t acked_fin;     /* fin_count at the last INTREQ clear of BLIT */
    uint32_t lost, lost_frame;  /* acknowledges that swallowed two completions */
    uint32_t read_fin;      /* fin_count when the CPU last read INTREQR */
    int      read_valid;    /* ...and it has not acknowledged BLIT since */
    int      no_hold;       /* A/B tests: no re-raise, exactly 1.102 (kept across aga_reset) */
    uint32_t held_count;    /* completions raised again after an acknowledge wiped them */
    int      line_mode;
    uint16_t bhold;               /* preloaded B */
    uint16_t aold, bold;          /* the A and B shifters' previous words: 0 as a blit starts */
} blitter_t;

typedef struct sprite {
    uint32_t pt;
    uint16_t pos, ctl;
    uint64_t data, datb;          /* up to 64 bits (FMODE) */
    int      armed;               /* DATA written since CTL */
    int      dmastate;            /* 1 = fetching data lines */
    int      dmacycle;            /* WinUAE: 1/2 = the next DMA slot fetches the first/second word */
    int      dmadata;             /* the words fetched on this line are DATA/DATB, not POS/CTL */
    int      vstart, vstop, xpos; /* xpos in shres pixels (4 per lores pixel) */
    int      attach;
} sprite_t;

/* A sprite's state from one moment of a line on, logged when the copper or
   the CPU rewrites the sprite while the beam is on the line (spr_build_line) */
typedef struct sprev {
    int      sx;                  /* sprline (shres) position from which it holds */
    int      xpos;
    uint64_t data, datb;
    uint8_t  n, armed;
} sprev_t;
#define AGA_SPR_EVENTS 128

typedef struct audio_channel {
    uint32_t lc, pt;              /* location (latched) and current pointer */
    uint16_t len, lencnt;
    uint16_t per, percnt;
    uint16_t vol;
    uint16_t dat;                 /* AUDxDAT last written */
    int      dmaen;
    int      state;               /* Paula state machine 0..5 */
    int      dat_written;
    int8_t   cur_sample;          /* sample currently output */
    int      intreq2;             /* pending interrupt state (per HRM) */
    /* resampling accumulator */
    int32_t  acc;
} audio_channel_t;

/* One register change recorded within a line for the renderer. A colour goes
   in as LC_COLOR + its palette index (0..255), outside the custom registers
   ($000-$1FE). As COLOR00 + index * 2 the 256 colours took $180-$37E, every
   register from $180 up with them: a copper DIWHIGH ($1E4) in mid-display
   drew the rest of its line in colour 50, set to the DIWHIGH value (found by
   review, 2026-10-05; test_linechange.c). */
#define LC_COLOR 0x400
typedef struct linechange {
    uint16_t hpos;                /* colour clock */
    uint16_t reg;                 /* register offset, or LC_COLOR + colour index */
    uint32_t value;               /* for a colour: expanded 24-bit value; others raw */
} linechange_t;

/* virtual floppy drives (disk.c) */
#define DSK_WORDS_PER_LINE 2          /* DD: 32 us per MFM word, ~2 words per scan line */
typedef struct disk_drive {
    uint8_t  *image;                  /* ADF image (copy) */
    uint32_t  size;
    int       inserted, wprot, dirty;
    int       sectors;                /* 11 (DD) or 22 (HD) per track */
    int       cyl, side, motor, changed;
    uint32_t  id_shift;
    uint16_t *mfm;                    /* current track, MFM words */
    int       mfm_words, mfm_track, mfm_valid;
    int       pos;                    /* word under the head */
} disk_drive_t;

typedef struct disk {
    disk_drive_t drv[AGA_MAX_DRIVES];
    int      selected;                /* virtual drive selected via CIA-B PRB, -1 none */
    uint8_t  prb;                     /* last CIA-B PRB value */
    uint16_t dsklen, dsksync, dskdat, dskbytr;
    int      dsklen_armed;            /* first of the two DMAEN writes seen */
    uint32_t dskpt;
    int      dma_active, dma_write, dma_left, synced, wordequal;
} disk_t;

typedef struct playfield {
    /* register values */
    uint32_t bplpt[AGA_MAX_PLANES];
    uint16_t bplcon0, bplcon1, bplcon2, bplcon3, bplcon4;
    int ecs_mode;                  /* ECS Denise semantics: no LOCT, no colour bank;
                                      cleared when the program shows it is AGA after
                                      all (playfield.c ecs_says_aga) */
    int ecs_forced;                /* the user's "always ECS": never cleared that way */
    int aga_fetch_seen;            /* in ECS mode: a non-zero FMODE or eight planes */
    int aga_colour_seen;           /* ... and a colour write under LOCT or a bank, or a BPLCON4 */
    int aga_pair_bank;             /* in ECS mode: 24-bit colour pairs in two banks, one not bank 0 */
    uint8_t pair_banks;            /* the banks 24-bit pairs were seen in, bit n bank n */
    uint32_t aga_pairs;            /* in ECS mode: 24-bit pairs taken the AGA way (bank 0) */
    /* in ECS mode, per AGA colour index: the last write was a high-nibble one
       (LOCT clear) right after a BPLCON3 write cleared LOCT, its 12-bit value
       and when it came (pf_now: colour clocks since the reset) */
    uint8_t  col_hi_pending[256];
    uint16_t col_hi_val[256];
    uint32_t col_hi_when[256];
    int      loct_clear_seen;      /* a BPLCON3 write with LOCT clear, at loct_clear_when */
    uint32_t loct_clear_when;
    int16_t  bpl1mod, bpl2mod;
    uint16_t diwstrt, diwstop, diwhigh;
    int      diwhigh_written;
    uint16_t ddfstrt, ddfstop;
    uint16_t fmode;
    uint16_t bpldat[AGA_MAX_PLANES];
    /* derived */
    int      res;                 /* 0 lores, 1 hires, 2 shres */
    int      planes;              /* requested planes from BPLCON0 */
    int      fm;                  /* fetch mode 0,1,2 */
    int      fetchunit, fetchstart, fetchstart_shift, maxplanes;
    int      plfstrt, plfstop;    /* effective DDF start/stop (cck) */
    int      diw_hstart, diw_hstop;   /* superhires coordinates (DIW units, 4 per lores) */
    int      diw_vstart, diw_vstop;
    /* per-line fetch state */
    int      vdiw;                /* the window, as the fetch sees it: the
                                     flip-flop, gated by vertical blanking */
    int      vdiw_ff;             /* the flip-flop itself: set at DIWSTRT's
                                     line, cleared at DIWSTOP's */
    int      fetch_active;        /* fetching on this line */
    int      fetch_cycle;         /* cck since plfstrt */
    int      fetch_last_hpos;     /* fetch synced up to this hpos */
    int      fetch_total;         /* total fetch cycles for this line */
    int      fetch_start_h;       /* hpos of the first fetch cycle this line (-1 = not started) */
    int      fetch_done;          /* fetch finished this line (modulos added) */
    int      mod_added;           /* the modulos were added for this line */
    unsigned mod_planes;          /* bit n: plane n+1 was fetched in the line's last block,
                                     so it takes its modulo */
    uint64_t slot_used[4];        /* bit h: the bitplane DMA fetched at colour clock h */
    int      line_plfleft;       /* hpos where first block became visible (-1 none) */
    int      line_blocks;         /* number of blocks fetched */
    int      line_res, line_fm;   /* latched at fetch start */
    int      line_planes;         /* the most planes the line fetched, for drawing
                                     (a mid-line BPLCON0 can raise it, never lower it;
                                     0 while the line has fetched nothing) */
    int      blocks_dirty;        /* blocks of blockdata written this line: cleared at the next */
    int      line_bpl1dat;        /* the CPU or the copper wrote BPL1DAT this line: Denise
                                     shows sprites on it (render_segment) */
    int      line_delay1, line_delay2;         /* native-pixel delays (odd/even planes) */
    int      line_bplcon0;
    /* planar line data: bit i of plane p = native pixel (block origin) + i, MSB first */
    uint64_t bits[AGA_MAX_PLANES][AGA_PLANE_WORDS];
    /* raw fetched data per block and the scroll delays valid when the block completed */
    uint64_t blockdata[AGA_MAX_BLOCKS][AGA_MAX_PLANES];
    int16_t  blockdelay1[AGA_MAX_BLOCKS], blockdelay2[AGA_MAX_BLOCKS];
    /* chunky index buffers for the current line (native resolution), odd and even planes
       separately because they are shifted by different BPLCON1 delays at display time */
    uint8_t  idx_odd[AGA_PLANE_WORDS * 64 + 64];
    uint8_t  idx_even[AGA_PLANE_WORDS * 64 + 64];
    int      line_delayoffset;    /* UAE delay offset for unaligned fetch starts (lores px) */
} playfield_t;

struct aga {
    aga_config_t cfg;
    aga_hooks_t  hooks;
    uint8_t     *chip;
    uint32_t     chipmask;
    void       (*log)(void *, const char *);
    void        *log_user;

    /* beam */
    int hpos, vpos, lof, maxhpos, maxvpos, ntsc;
    int lol;                    /* this line is a long one (NTSC, 228 cck): maxhpos includes it */
    int ntsc_strap;             /* the Alice's PAL/NTSC strap, VPOSR's ID: cfg.ntsc at the reset */
    int vblank_end;
    int vertb_hold;             /* line 0: VERTB not raised yet, the copper's setup block first */
    int sprite_vblank;          /* line the sprite DMA restarts on (UAE sprite_vblank_endline) */
    int frame;
    int lof_toggle;
    uint32_t line_no;           /* lines since the reset, for "within two lines" */

    /* generic registers */
    uint16_t regs[256];
    uint16_t dmacon, intena, intreq, adkcon;
    uint32_t ecs_upgrades;         /* ECS Denise given up for AGA (ecs_says_aga) */
    uint32_t color_aga[256];       /* in ECS mode: the palette as an AGA Lisa would hold it */
    uint16_t clxcon, clxcon2, clxdat;
    uint16_t beamcon0;
    uint16_t copcon;
    int      ipl;

    copper_t   cop;
    blitter_t  blt;
    sprite_t   spr[AGA_MAX_SPRITES];
    int        sprite_width;       /* 16/32/64 */
    int        sprres;             /* sprite resolution 0..2 */
    audio_channel_t aud[4];
    playfield_t pf;

    /* palette: 256 x 24-bit */
    uint32_t color[256];

    /* per-line change list */
    linechange_t changes[AGA_LINE_CHANGES];
    int          nchanges;

    /* sprite line buffer, in shres pixels relative to lores pixel 0 (DIW coords):
       bits 0-3 colour (within 16), bit 4 attached, bits 5-6 pair, bit 7 present */
    uint8_t  sprline[AGA_OUT_W * 2 + 512];
    int      sprline_lo, sprline_hi;  /* the part of it written since it was last cleared */
    int      sprline_valid;

    /* Collision detection (CLXDAT). Worked out only while the program reads
       CLXDAT - most never do, and it costs time on every pixel - or always
       with cfg.collisions. sprclx has the same pixels as sprline but keeps
       every sprite apart: bit n = sprite n has a pixel there. */
    int      clx_frames;           /* frames left, re-armed by every CLXDAT read */
    uint8_t  sprclx[AGA_OUT_W * 2 + 512];
    int      sprclx_valid;

    /* sprites rewritten during the current line (see spr_build_line) */
    sprev_t  sprev[AGA_SPR_EVENTS];
    int      nsprev, sprev_full;
    unsigned spr_touched;          /* bit n: the copper or the CPU wrote sprite n this line */
    /* the last DMACON writes, to find out who switches a display off:
       value in bits 0-15, bit 16 set when the copper did it, frame in 17-31 */
    uint32_t dma_log[8];
    uint32_t dma_log_head;
    /* why a line fetched no bitplane data: counted where the decision is
       made, one bucket per reason */
    uint32_t dbg_fetch[6];
    /* audio interrupts by source: DAT write on an idle channel, buffer
       wrap, first word of a DMA start, manual word, play-out after DMA off */
    uint32_t dbg_audirq[5];
    /* where the copper restarted at each vertical blank (the last eight
       CHANGES of it, with the frame), and its own writes to BPLCON0 */
    uint32_t dbg_coprs_addr[8], dbg_coprs_frame[8], dbg_coprs_n, dbg_coprs_last;
    uint32_t dbg_cop_bc0_n, dbg_cop_bc0_pc, dbg_cop_bc0_val, dbg_cop_bc0_frame;
    /* event log (aga_dbg_event): 16 before the trigger, then 32 after, stopped */
    uint32_t ev_pre[16][2], ev_pre_n, ev_log[48][2], ev_log_n;
    int      ev_trig;
    /* the last BEAMCON0 writes, same packing as dma_log: a lone write with
       the PAL bit clear turns a 313-line frame into a 263-line one */
    uint32_t bc0_log[8];
    uint32_t bc0_log_head;
    /* sprite resolution along the line: the value at the start of it, then
       every change with the beam position it happened at (SPR_NOW units) */
    int      sprres_line0;
    int      nsprres_ev;
    int      sprres_ev_sx[16];
    int8_t   sprres_ev_res[16];
    uint8_t  spv[AGA_MAX_SPRITES][AGA_OUT_W * 2 + 512];   /* replay: each sprite's 2-bit pixels */

    /* output */
    /* Where finished lines go. By default the two frames below; the ARM
       side points them at the HVS's own pages instead (aga_set_framebuffers)
       and marks them external, which adds the alpha byte the HVS wants.
       Lines are rendered into linebuf first - the renderer stores a pixel at
       a time, and an external page is uncached memory, where one streaming
       copy per line is right and 800 scattered stores are not. */
    uint32_t *fb[3];
    int      fb_external;
    int      fb_count;             /* 2 internal, 3 for tear-free external HVS */
    uint32_t linebuf[AGA_OUT_W];
    uint32_t fb_store[2][AGA_OUT_W * AGA_OUT_H];
    int      fb_cur;               /* index of frame being drawn */
    int      fb_done;              /* last fully completed frame */
    /* The flicker fixer (aga_set_weave). weave_now: the frame being drawn is
       woven, its lines on rows 2y + weave_row of the two pages as one frame;
       weave_fill: it is the first woven one, each line on both rows, so the
       rows between do not show an old picture. weave_done and weave_done_row:
       the same for the last completed frame, for aga_frame_woven. */
    int      weave_now, weave_fill, weave_row;
    int      weave_done, weave_done_row;
    /* Brightness and Gamma (aga_set_brightness, aga_set_gamma): with tone_on
       every channel of every shown colour goes through bright_lut. color_b
       is the palette through it, kept beside a->color; rebuilt whole when
       bright_dirty. */
    uint8_t  bright_lut[256];
    int      tone_on;
    uint32_t color_b[256];
    int      bright_dirty;
    /* Frame skip. skip_req is the host's wish, latched at each frame end
       into frame_skip for the whole next frame: its lines are not drawn
       and the pages are not swapped after it, so the page on screen stays
       on screen and the next drawn frame goes to the page that is not.
       Never while collisions are being computed (they come out of the
       renderer), never more than SKIP_RUN_MAX frames in a row. */
    int      skip_req;
    int      frame_skip;
    int      skip_run;
    uint32_t frames_skipped;
    int      frame_done_count;
    uint32_t blit_count;           /* blits started since reset (diagnostics) */
    uint32_t aud_on[4];            /* audio DMA switched on, per channel (diagnostics) */
    uint32_t aud_words[4];         /* sample words fetched, per channel (diagnostics) */
    uint32_t aud_nz[4];            /* |sample * vol| accumulated: what actually reaches the mix */
    uint32_t aud_volmax[4];        /* loudest AUDxVOL ever set on this channel */
    uint32_t aud_volwr[4];         /* AUDxVOL writes seen, per channel */
    uint32_t aud_volnz[4];         /* ...of which asked for a NON-zero volume */
    uint32_t aud_dmaoff[4];        /* times the game switched this channel's DMA OFF */
    uint32_t aud_wrap[4];          /* times the sample looped back to AUDxLC */
    uint32_t aud_irq[4];           /* times INTF_AUDx was raised for it */
    /* What I loop BACK to, sampled every 512th wrap - the loops run tens of
       thousands of times, so every one of them cannot be probed. */
    uint32_t aud_wrapinfo[32][4];  /* chan, lc, len, peak of the loop buffer */
    uint32_t aud_wrapinfo_head;
    uint32_t aud_audible[4];       /* steps with a non-zero sample AND non-zero volume */
    /* The last 32 times a channel's DMA was switched on, with everything that
       decides whether it makes a sound: 9 of every 10 starts on the effects
       channel produce silence, and one of these four values must say why. */
    uint32_t aud_trig[32][6];      /* chan, lc, len, per, vol, first sample word */
    uint32_t aud_trig_head;
    int      aud_mask;             /* bit n: mix channel n. 0 = not yet set = all */
    int      pf_data_nonzero;      /* a bitplane fetch in this frame returned non-zero data */
    int      last_frame_data;      /* same, for the last completed frame */
    int      display_lines;        /* lines with bitplane data in the frame being drawn */
    int      last_display_lines;   /* same for the last completed frame */
    /* where the display window was on those lines, in the frame's hires
       pixels [x0, x1) and lines y0..y1 - x may be below 0 or past AGA_OUT_W,
       a window opened outside the frame; x0 > x1 when there were none. For
       the HDMI picture's Size Auto (aga_last_frame_window), RGB2RTG's idea */
    int      win_x0, win_x1, win_y0, win_y1;
    int      last_win_x0, last_win_x1, last_win_y0, last_win_y1;
    uint16_t aga_ctrl;             /* AGA control register 0xDFF1F2 (native/RTG switch, windowed) */
    uint16_t aga_win[4];           /* 0xDFF1F4..0xDFF1FA: window x, y, w, h on the RTG screen */

    /* audio output */
    int      audio_rate;
    int32_t  audio_frac;

    /* virtual floppy (allocated separately: survives aga_reset) */
    disk_t  *dsk;

#ifdef AGA_TRACE
    aga_slot_t *trace[2];       /* trace remains independently double buffered */
    int trace_cur, trace_done;
#endif
};

/* Record a DMA slot. Compiled out entirely unless AGA_TRACE is defined. */
static inline void aga_trace_at(aga_t *a, int hpos, uint8_t type, uint16_t reg, uint32_t addr)
{
#ifdef AGA_TRACE
    aga_slot_t *buf = a->trace[a->trace_cur];
    if (buf && a->vpos >= 0 && a->vpos < AGA_MAX_VPOS && hpos >= 0 && hpos < AGA_MAX_HPOS) {
        aga_slot_t *s = &buf[a->vpos * AGA_MAX_HPOS + hpos];
        s->type = type; s->reg = reg; s->addr = addr;
    }
#else
    (void)a; (void)hpos; (void)type; (void)reg; (void)addr;
#endif
}

/* A short line's length: every PAL line, and the NTSC lines without LOL.
   A compare against "the end of the line" uses it, so a register gives the
   same answer whichever kind of line it was written on: a DDFSTOP of $E4 is
   past the last colour clock of a long NTSC line ($E3) too (calcdiw). */
static inline int aga_short_line(const aga_t *a) { return a->maxhpos - a->lol; }

/* custom.c */
void aga_intreq_set(aga_t *a, uint16_t bits);
void aga_update_ipl(aga_t *a);
void aga_record_change(aga_t *a, uint32_t reg, uint32_t value);
int  aga_dma_enabled(const aga_t *a, uint16_t mask);
void aga_logf(aga_t *a, const char *fmt, ...);

/* copper.c */
void cop_reset(aga_t *a);
void cop_start_line(aga_t *a);
void cop_sync(aga_t *a, int hpos);
void cop_vsync(aga_t *a);
void cop_write_lc(aga_t *a, int which, int high, uint16_t v);
void cop_strobe(aga_t *a, int which);
void cop_blitter_done(aga_t *a);

/* blitter.c */
void blt_reset(aga_t *a);
void blt_write(aga_t *a, uint32_t reg, uint16_t v);
int  aga_blit_pending(aga_t *a);
void aga_blit_complete(aga_t *a);
uint16_t blt_dmaconr_bits(const aga_t *a);

/* playfield.c */
void pf_reset(aga_t *a);
void pf_write(aga_t *a, uint32_t reg, uint16_t v, int hpos);
void pf_start_line(aga_t *a);
void pf_sync(aga_t *a, int hpos);
void pf_finish_line(aga_t *a);     /* fetch remainder, render line into framebuffer */
void pf_vsync(aga_t *a);
void pf_color_write(aga_t *a, int num, uint16_t v, int hpos);
uint16_t pf_con3(const playfield_t *p);
uint16_t pf_con4(const playfield_t *p);
int  pf_is_bpl_slot(aga_t *a, int hpos);   /* 1 if a bitplane fetch occupies this cycle */
int  pf_bpl_running(aga_t *a, int hpos);   /* 1 while the line's bitplane fetch window runs (WinUAE bprun) */

/* sprites.c */
void spr_reset(aga_t *a);
void spr_write(aga_t *a, uint32_t reg, uint16_t v);
void spr_dma_slot(aga_t *a, int hpos);
void aga_dbg_event(aga_t *a, int type, uint16_t value, int trigger);
void spr_build_line(aga_t *a, int clx);   /* fill a->sprline (and a->sprclx if clx) for the current line */
void spr_vsync(aga_t *a);
void spr_start_line(aga_t *a);
void spr_expand_res(aga_t *a);

/* disk.c */
void dsk_create(aga_t *a);
void dsk_destroy(aga_t *a);
void dsk_reset(aga_t *a);
int  dsk_read(aga_t *a, uint32_t reg, uint16_t *v);     /* 1 = handled (virtual disk present) */
int  dsk_write(aga_t *a, uint32_t reg, uint16_t v);
void dsk_line_tick(aga_t *a);

/* audio.c */
void aud_reset(aga_t *a);
void aud_clock(aga_t *a);    /* the sample clock again, after PAL <-> NTSC */
void aud_write(aga_t *a, uint32_t reg, uint16_t v);
void aud_dma_slot(aga_t *a, int hpos);
void aud_dmacon_changed(aga_t *a, uint16_t old, uint16_t new_);
void aud_line_tick(aga_t *a);

/* ---- inline helpers ------------------------------------------------------- */
/* a 0x00RRGGBB colour through the brightness and gamma table (bright_lut) */
static inline uint32_t bright_rgb(const aga_t *a, uint32_t c)
{
    return ((uint32_t)a->bright_lut[(c >> 16) & 255] << 16) | ((uint32_t)a->bright_lut[(c >> 8) & 255] << 8)
           | a->bright_lut[c & 255];
}

static inline uint16_t chip_rd16(const aga_t *a, uint32_t addr)
{
    addr &= a->chipmask;
    return (uint16_t)((a->chip[addr] << 8) | a->chip[(addr + 1) & a->chipmask]);
}
static inline uint32_t chip_rd32(const aga_t *a, uint32_t addr)
{
    return ((uint32_t)chip_rd16(a, addr) << 16) | chip_rd16(a, addr + 2);
}
static inline uint64_t chip_rd64(const aga_t *a, uint32_t addr)
{
    return ((uint64_t)chip_rd32(a, addr) << 32) | chip_rd32(a, addr + 4);
}
static inline void chip_wr16(aga_t *a, uint32_t addr, uint16_t v)
{
    addr &= a->chipmask;
    a->chip[addr] = (uint8_t)(v >> 8);
    a->chip[(addr + 1) & a->chipmask] = (uint8_t)v;
}

#define GET_RES(con0)     (((con0) & BPLCON0_SHRES) ? RES_SHRES : ((con0) & BPLCON0_HIRES) ? RES_HIRES : RES_LORES)
#define GET_PLANES(con0)  (((((con0) >> 12) & 7) | (((con0) & BPLCON0_BPU3) >> 1)))

#endif /* AGA_INTERNAL_H */
