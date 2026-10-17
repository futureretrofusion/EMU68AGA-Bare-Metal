/* AGA-PISTORM - the emulated Paula's mix on HDMI as well.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The Pi's HDMI encoder has an audio input of its own, the MAI: a 32-word
 * FIFO of IEC958 subframes that it sends to the screen as audio sample
 * packets. Under Emu68 the firmware owns the HDMI output and leaves the MAI
 * idle. This file starts it the way Linux does - drivers/gpu/drm/vc4/
 * vc4_hdmi.c (GPL-2.0-only): vc4_hdmi_audio_startup, _prepare,
 * _set_mai_clock, _set_n_cts and vc4_hdmi_write_infoframe, with the values
 * Linux uses for a Pi 4 - and feeds it with the legacy DMA engine, paced by
 * the MAI's DREQ, from a ring the chipset core fills.
 *
 * What the firmware leaves set up was probed first (docs/HDMI-AUDIO.md,
 * tools/hdmi): an HDMI link with the audio clock running, but no audio
 * infoframe - its packet slots are 0 General Control, 2 AVI and 4 SPD, not
 * Linux's numbering by type - and no N/CTS. So the audio infoframe goes into
 * slot 7, which it leaves off, and N is set; CTS the hardware measures for
 * itself (EXTERNAL_CTS_EN): a CTS off by half played exactly the same.
 *
 * The ring is a whole number of 192-frame IEC958 blocks, so every frame's
 * place in its block follows from its index and the DMA loops it without a
 * seam: silence written once plays for ever. Core 3 resamples the mix from
 * Paula's 27,928 Hz to 48 kHz a fixed lead ahead of the DMA, trims the rate
 * by the lead it measures (the DMA's SOURCE_AD is the exact play position),
 * and keeps silence written a stretch further on. When the mix stops - the
 * sandbox left, a game paused in the debugger - the housekeeper fills the
 * ring with silence before the DMA could come round to old sound again.
 *
 * Off until asked: Chipset menu - Sound - HDMI On sends $5821 (agaboot SOUND,
 * which saves the choice and sends it again at every boot and game), $5820
 * stops it and puts back every register it changed the way the firmware had
 * it. While it is wanted and cannot play (no link, the slot or the MAI taken,
 * no free DMA channel) the housekeeper tries again once a second.
 *
 * The Pi 4's HDMI0 and the Pi 3's HDMI, from one table each (struct hdmi_hw),
 * picked by the board it runs on: the Pi 4's MAI runs from a fixed 108 MHz,
 * the Pi 3's from its HSM, whose rate comes from CPRMAN's divider.
 *
 * An HDMI register read with the HDMI state machine's clock (the HSM)
 * stopped stalls the core that reads it - the 68k's, or the housekeeper's,
 * which samples the Amiga's interrupt lines. So boot reads no HDMI register
 * at all, only the clock (the firmware's measured rate on a Pi 4, CPRMAN on a
 * Pi 3); the firmware's values are saved at the first start. After boot every
 * round that touches an HDMI register first needs the HDMI picture to be
 * moving at that moment - HVS channel 1, which feeds it, seen changing its
 * status between two reads just before (the HVS runs from the core clock, so
 * that read never stalls) - and on a Pi 3 the HSM running. Guards of the same
 * kind as RGB2RTG's port of this file (a moving HDMI picture, and on a Pi 3
 * the HSM), whose reviewers found what they close; the detail differs (its
 * Pi 4 guard is a blank its own HVS poll saw, and its aga.hdmiforce skips the
 * Pi 3 check, where mine only waives the source). Two gaps remain, damn it.
 * On a Pi 4 a moving picture stands in for a running HSM, which nothing here
 * can read after boot: an assumption, not yet tested on hardware (unplug while
 * sound plays, with and without hdmi_force_hotplug). And the clock can still
 * stop between the guard's reads and the register access, a window of at most
 * about 100 microseconds (200 ms in the first port of the fixes). The link
 * counts as lost only after 200 ms with no change; until then a look just
 * waits for the picture. A Pi
 * that booted with no HDMI clock (no screen, no hdmi_force_hotplug) is left
 * alone - a Pi 3 looks again while sound is wanted, since CPRMAN can be read
 * any time. cmdline.txt aga.nohdmiaudio switches it off for good.
 *
 * 1.114: 24 bits and the rate an AHI program plays at. Each subframe carries
 * 24 bits of sample: Paula's mix fills the top 16 as before, an AHI program
 * mixed in 32 bits (tools/ahi/aga-hdmi.c, HiFi) the whole 24. And HDMI runs
 * at 48 kHz until an AHI program plays at 44.1, 88.2, 96, 176.4 or 192 kHz:
 * then at that rate (set_rate) - music from a CD or an MP3 played as it was
 * made, not resampled to 48 kHz - with Paula resampled to it. Only what the
 * screen takes: its EDID, read at boot (aga_edid.h), says which rates and
 * whether 24 bits; a screen that takes 16 bits gets the top 16, rounded. A
 * new rate is a short gap while the screen locks on; it stays until another
 * program asks for another, or a game starts the sandbox (48 kHz again).
 */
#include <stdint.h>
#include "aga_glue.h"
#include "aga_hdmi_filter.h"
#include "aga_mix.h"
#include "aga_edid.h"
#include "aga_blep.h"
#include "aga/chipset.h"                                 /* aga_step_t */
#include "devicetree.h"

extern void kprintf(const char *fmt, ...);
extern uint32_t get_clock_rate(uint32_t clock_id);     /* Emu68 support_rpi.c: mailbox */
extern uint32_t get_clock_rate_measured(uint32_t clock_id);
extern int get_edid_block(uint32_t block, uint8_t *out);

int aga_hdmi_audio_off;                                  /* cmdline aga.nohdmiaudio */
int aga_hdmi_force;                                      /* cmdline aga.hdmiforce: no clock check */

#define LE32(x)  __builtin_bswap32(x)                    /* the ARM runs big-endian */
#define PERI(b)  ((volatile uint32_t *)(0xF2000000UL + ((uint32_t)(b) - 0x7E000000UL)))
static inline uint32_t rd(uint32_t b)             { return LE32(*PERI(b)); }
static inline void     wr(uint32_t b, uint32_t v) { *PERI(b) = LE32(v); }

/* Each SoC's HDMI block: Linux vc4_hdmi_regs.h (vc5_hdmi_hdmi0_fields for the
   Pi 4's HDMI0, vc4_hdmi_fields for the Pi 3), the addresses and DREQs from
   bcm2711.dtsi and bcm2835-common.dtsi, and the threshold and channel-map
   values vc4_hdmi.c gives each generation. The Pi 3's firmware was probed as
   well (2026-10-03, a 3 A+): the same packet slots as the Pi 4's, the same
   idle MAI, no N/CTS. */
struct hdmi_hw {
    const char *name;
    uint32_t mai_ctl, mai_thr, mai_fmt, mai_data, mai_smp;   /* the MAI ("HD") block */
    uint32_t ch_map, mai_cfg, aud_pkt, pkt_cfg, pkt_stat, crp, cts0, cts1, sched, pkt_ram;
    uint32_t phy, phy_rng;                       /* the register with the RNG power-down bit */
    uint32_t dreq, thr, chmap, dma_mask;         /* dma_mask: the DT's default */
};
static const struct hdmi_hw hw_pi4 = { "HDMI0 (Pi 4)",
    0x7EF20010, 0x7EF20014, 0x7EF20018, 0x7EF2001C, 0x7EF20020,
    0x7EF0079C, 0x7EF007A0, 0x7EF007B8, 0x7EF007BC, 0x7EF007C4, 0x7EF007C8, 0x7EF007CC, 0x7EF007D0, 0x7EF007E0,
    0x7EF01B00,
    0x7EF00F04, 1u << 4,                         /* TX_PHY_POWERDOWN_CTL, RNDGEN_PWRDN */
    10, 0x10101C1C, 0x10, 0x07F5 };              /* 4 bits a channel */
static const struct hdmi_hw hw_pi3 = { "HDMI (Pi 3)",
    0x7E808014, 0x7E808018, 0x7E80801C, 0x7E808020, 0x7E80802C,
    0x7E902090, 0x7E902094, 0x7E90209C, 0x7E9020A0, 0x7E9020A4, 0x7E9020A8, 0x7E9020AC, 0x7E9020B0, 0x7E9020C0,
    0x7E902400,
    0x7E9022C4, 1u << 25,                        /* TX_PHY_CTL_0, RNG_PWRDN */
    17, 0x08080608, 0x08, 0x7F35 };              /* 3 bits a channel; thresholds lowered by Linux 1c594eeccf92 */
static const struct hdmi_hw *hw;
/* The table goes by the board, not by the kernel: the Pi 3 kernel on a Pi 4
   works as the Pi 4 kernel (aga_glue.h), and a Pi 4's CPRMAN has no HSM - it
   reads 0x636D there, which would look like a stopped clock (found by
   RGB2RTG). */
static int pi3;
static uint32_t mai_smp;                         /* the MAI clock / the rate, as N << 8 | M - 1 */
static uint32_t mai_hz;                          /* ... that clock */
static uint32_t hsm_div;                         /* Pi 3: the HSM divider mai_smp was worked out from */
static int smp_new;                              /* ... changed under my running MAI: MAI_SMP to write */

#define HSM_CLOCK 13                             /* Pi 4: firmware clock "hdmi" (M2MC) */
#define CM_HSMCTL 0x7E101088u                    /* Pi 3: CPRMAN's HSM, the MAI clock */
#define CM_HSMDIV 0x7E10108Cu
#define SLOT      7                              /* the firmware uses 0, 2 and 4 */

/* The rates HDMI may run at, the order aga_edid.h's bits and
   aga_hdmi_filter.h's hf_k have: N for the sink (HDMI's table for 148.5 MHz
   and most other pixel clocks; CTS the hardware measures), the MAI's code
   for the rate (vc4_hdmi's VC4_HDMI_MAI_SAMPLE_RATE_*), and IEC 60958's in
   channel status byte 3. */
#define N_RATES 6
#define R_48K   1
static const uint32_t rate_hz[N_RATES]  = { 44100, 48000, 88200, 96000, 176400, 192000 };
static const uint32_t rate_n[N_RATES]   = { 6272, 6144, 12544, 12288, 25088, 24576 };
static const uint8_t  rate_mai[N_RATES] = { 8, 9, 11, 12, 14, 15 };
static const uint8_t  rate_cs[N_RATES]  = { 0x0, 0x2, 0x8, 0xA, 0xC, 0xE };
_Static_assert(N_RATES == HF_RATES, "a filter set for every rate");
static volatile int rate_i = R_48K;              /* what the MAI, the ring and the tables are set for */
static uint32_t sink_rates = 3;                  /* bit i: the screen takes rate_hz[i] (EDID); 44.1 and 48 kHz without one */
static int sink_24;                              /* ... and 24-bit samples */

/* The Sound window's HDMI sound rate (agaboot SOUND RATE), $5868 + E2(m) with
   E2(m) = (m & 1) | (m & 2) << 1: 0 follow the playing program within what
   the screen's EDID lists, 1 follow it at any rate (an EDID that undersells
   its receiver), 2 always 96 kHz, 3 always 192 kHz - Paula and AHI alike.
   What is offered (to the driver, through AGA_D_AHI) follows from it, and
   past the EDID the receiver is taken to want 24 bits too. */
static volatile int rate_mode;
static const int fixed_ri[4] = { -1, -1, 3, 5 };
static uint32_t offer_rates(void)
{
    int m = rate_mode;
    return m == 0 ? sink_rates : m == 1 ? 0x3Fu : 1u << fixed_ri[m];
}
static int out_24(void) { return sink_24 || rate_mode != 0; }
#define SLOT_AT   (hw->pkt_ram + SLOT * 0x24)

/* HVS channel 1, the one the firmware shows HDMI with (and aga_hvs.c draws
   into): SCALER_DISPSTAT1 (vc4_regs.h). Channel 1's own fields only - mode,
   FIFO full/empty, frame count, line; the word also carries channel 0's frame
   count (bits 23:18 on the Pi 3, 25:20 on the Pi 4), which a display on the
   DSI port would keep moving. */
#define HVS_DISPSTAT1 0x7E400058u
#define SCAN_MASK     (pi3 ? 0xF003FFFFu : 0xF00FFFFFu)

/* MAI_CTL (vc4_regs.h) */
#define M_DLATE   (1u << 15)
#define M_CHALIGN (1u << 13)
#define M_WHOLSMP (1u << 12)
#define M_FLUSH   (1u << 9)
#define M_ENABLE  (1u << 3)
#define M_ERRORE  (1u << 2)
#define M_ERRORF  (1u << 1)
#define M_RESET   (1u << 0)
#define M_FLAGS   (M_DLATE | M_ERRORE | M_ERRORF)        /* write 1 to clear */
/* the MAI running: two channels, whole samples, aligned. A write that clears
   the flags while it plays carries these too - without ENABLE it stops. */
#define M_RUN     ((2u << 4) | M_WHOLSMP | M_CHALIGN | M_ENABLE)

/* the legacy DMA engine (bcm2835-dma.c) */
#define DMA_BASE    0x7E007000u
#define DMA_CS(c)   (DMA_BASE + (c) * 0x100u + 0x00)
#define DMA_CB(c)   (DMA_BASE + (c) * 0x100u + 0x04)
#define DMA_SRC(c)  (DMA_BASE + (c) * 0x100u + 0x0C)
#define DMA_DEBUG(c) (DMA_BASE + (c) * 0x100u + 0x20)
#define DMA_ENABLE  (DMA_BASE + 0xFF0)
#define CS_ACTIVE   (1u << 0)
#define CS_WAIT_WR  (1u << 6)                    /* WAITING_FOR_OUTSTANDING_WRITES */
#define CS_ERROR    (1u << 8)
#define CS_RESET    (1u << 31)
/* the channel's AXI priority, and the one it gets while the MAI signals
   panic (its FIFO below MAI_THR's panic level): with 0, the default, those
   thresholds raise nothing. Not yet compared on hardware: the MAI's error
   flags (hdmi_mai_errs) are the number to watch. */
#define CS_PRIO     ((8u << 16) | (15u << 20))
/* DEBUG's error flags - read last not set, FIFO error, read error - write 1
   to clear. CS's ERROR only shows that one of them is set; a reset of the
   channel is not documented to clear them, so they are cleared here. */
#define DBG_ERRORS  7u
#define TI_WAIT_RESP (1u << 3)
#define TI_DEST_DREQ (1u << 6)
#define TI_SRC_INC   (1u << 8)
#define TI_PERMAP(n) ((uint32_t)(n) << 16)

/* The ring and the lead, per board. Whole 192-frame blocks, two subframes a
   frame. The Pi 4 writes 21 ms ahead in an 80 ms ring and did not resync
   once in 5 minutes on a CM4 (2026-10-02). The Pi 3's chipset loop stalls
   for up to 31 ms - memory starvation of the A53 cluster (2026-09-14) - and
   at 21 ms it resynced 14 times in 8 minutes, the Paula path with its 37 ms
   lead 6 times in the same run (2026-10-03): it writes 48 ms ahead in a
   120 ms ring. Lead and silence horizon together stay longer than the idle
   wait, so the DMA meets silence, never old sound, when the mix stops.

   And the idle wait ends inside the pauses the lead check catches by
   itself. A feed after a pause of more than (lead_target - lead_low) / 48
   ms resyncs on its lead, until the pause is so long that the writer, now
   BEHIND the DMA, shows a lead under lead_high again: at
   (ring_frames - lead_high + lead_target) / 48 ms. With 3000 that was
   38.8 ms on the Pi 4, under its 40 ms wait: a feed 39 ms late found the
   writer 870 frames behind the DMA with a lead that looked fine, and the
   DMA played the rest of the silence and then 48 ms of 80 ms-old sound
   (found by review). 2700 makes it 45 ms; the Pi 3's 4800 makes it 68 ms
   against its 60. Neither board's lead comes near its limit in play.

   Those are the numbers at 48 kHz (base_*). At another rate every count of
   frames is scaled to the same time (rate_params), the ring rounded up to
   whole blocks - a few ms longer, which only widens the margins above. At
   192 kHz the Pi 3's 120 ms are 23,040 frames: RING_MAX, 184 KB. */
#define RING_MAX    (120 * 192)                          /* the most either board uses, at 192 kHz */
static uint32_t base_ring = 20 * 192, base_lead = 1024, base_high = 2700;   /* at 48 kHz */
static uint32_t ring_frames = 20 * 192;                  /* 3840: 80 ms */
static uint32_t lead_target = 1024;                      /* 21 ms ahead of the DMA */
static uint32_t lead_high   = 2700;                      /* outside lead_low..lead_high: resync */
static uint32_t lead_low    = 192;
static uint32_t horizon     = 1536;                      /* silence a further 32 ms on */
static uint32_t idle_ms     = 40;                        /* no mix this long: the ring goes silent */

static uint32_t at_rate(uint32_t frames_48k)
{
    return (uint32_t)(((uint64_t)frames_48k * rate_hz[rate_i] + 24000) / 48000);
}

/* the counts above for rate_i - with nothing playing */
static void rate_params(void)
{
    ring_frames = (at_rate(base_ring) + 191) / 192 * 192;
    if (ring_frames > RING_MAX) ring_frames = RING_MAX;
    lead_target = at_rate(base_lead);
    lead_high   = at_rate(base_high);
    lead_low    = at_rate(192);
    horizon     = at_rate(1536);
}

static volatile uint32_t *ring;                          /* uncached, identity-mapped */
static uint32_t ring_bus, cb_bus;                        /* what the DMA sees */
static uint32_t dma_mask;                                /* the channels the firmware leaves to the ARM */
static volatile int dma_ch = -1;
static uint32_t dma_enable_was;                          /* its DMA_ENABLE bit before my start: put back at stop */
static volatile int state = -1;                          /* AGA_D_HDMI_STATE; -1 before boot */
/* 6 is unused: before 1.112 it meant "not on the Pi 3 yet", and agaboot
   reads the states by number, so it stays free */
enum { H_OFF, H_RUNNING, H_NO_LINK, H_NO_CLOCK, H_SLOT_BUSY, H_NO_DMA, H_UNUSED_6, H_SWITCHED_OFF,
       H_BUSY_OTHER };                                   /* BUSY_OTHER: someone else's MAI is enabled */
static volatile int wanted;                               /* the last $582N: 1 sound wanted */
static int ready;                                        /* boot found a clock (or a Pi 3 to wait for) and DMA channels */
/* The MAI registers and slot 7 hold my values: put back at stop - or, when
   the clock stopped under them, once the guards let me. Slot 7 on its own as
   well: after another driver took the MAI over while mine played (check),
   my infoframe stays on until Sound Off or my next start. */
static volatile int mai_ours, slot_ours;
static inline int ours(void) { return mai_ours || slot_ours; }
static int info_pending;                                 /* the infoframe waits for its slot to go quiet */
/* the firmware's values of everything I change, read at the first start,
   put back at stop (the PHY's RNG bit and MAI_CTL each their own way) */
enum { S_THR, S_FMT, S_SMP, S_CHMAP, S_CFG, S_PKT, S_CRP, S_CTS0, S_CTS1, S_PHY, S_CTL, S_N };
static uint32_t saved[S_N], saved_reg[S_N];
/* start, stop (core 0, from the 68k's control word) and the housekeeper's
   repairs each write a whole sequence of registers: one at a time */
static volatile int idle = 1;            /* the ring holds silence: core 3 resyncs on its next feed */
static volatile int reg_lock;
static inline void lock_regs(void)   { while (__atomic_exchange_n(&reg_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static inline int  try_regs(void)    { return !__atomic_exchange_n(&reg_lock, 1, __ATOMIC_ACQUIRE); }
static inline void unlock_regs(void) { __atomic_store_n(&reg_lock, 0, __ATOMIC_RELEASE); }

static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t cntfrq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* ---- IEC958 subframes ---------------------------------------------------------
   One little-endian word per channel, left first: bits 3:0 the preamble (8 for
   the left subframe that starts a 192-frame block, 2 for any other left, 4 for
   right), the 24-bit sample in 27:4, bit 30 this frame's bit of the channel
   status block, bit 31 even parity over bits 4-30. Channel status as
   Raspberry Pi OS sends it: consumer PCM, the rate (byte 3), and the word
   length (byte 4): 24 bits for a screen that takes them, else 16 - whose
   subframes then carry 16 bits, rounded, the 8 below them zero. */
static uint32_t base_w[192][2];
static uint8_t  base_p[192][2];

static void make_tables(void)
{
    uint8_t cs[24] = { 0x04, 0x82, 0x00, rate_cs[rate_i], (uint8_t)(out_24() ? 0x0B : 0x02) };
    for (int f = 0; f < 192; f++)
        for (int ch = 0; ch < 2; ch++) {
            uint32_t w = ch ? 0x4 : f == 0 ? 0x8 : 0x2;
            if ((cs[f >> 3] >> (f & 7)) & 1) w |= 1u << 30;
            base_w[f][ch] = w;
            base_p[f][ch] = (uint8_t)__builtin_parity((w >> 4) & 0x07FFFFFF);
        }
}

#define S24_MAX 8388607
static inline int32_t clamp24(int32_t v) { return v > S24_MAX ? S24_MAX : v < -S24_MAX - 1 ? -S24_MAX - 1 : v; }

/* s: 24 bits, signed */
static inline uint32_t subframe(uint32_t f, int ch, int32_t s)
{
    uint32_t smp = (uint32_t)s & 0xFFFFFFu;
    uint32_t w = base_w[f][ch] | (smp << 4);
    if (base_p[f][ch] ^ __builtin_parity(smp)) w |= 1u << 31;
    return LE32(w);
}

/* k below RING_MAX always - the ring's memory - even should a count from
   before a new rate meet the new ring_frames */
static void put_frame(uint32_t k, int32_t l, int32_t r)
{
    if (k >= RING_MAX) return;
    if (!out_24()) {                                     /* 16 bits, rounded */
        l = clamp24(l + 128) & ~255;
        r = clamp24(r + 128) & ~255;
    }
    uint32_t f = k % 192;
    ring[2 * k]     = subframe(f, 0, l);
    ring[2 * k + 1] = subframe(f, 1, r);
}

static void silence(uint32_t from, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = from + i;
        while (k >= ring_frames) k -= ring_frames;
        put_frame(k, 0, 0);
    }
}

/* The whole ring is silence and nothing writes it: a start may begin at once.
   Core 0 silences it in one go; the housekeeper in pieces (prep_*), as it
   does the idle fill. */
static int ring_silent;
static uint32_t prep_at, prep_left;

static void set_state(int s)
{
    __atomic_store_n(&state, s, __ATOMIC_RELEASE);
    aga_diag_set(AGA_D_HDMI_STATE, (uint32_t)s);
}

/* ---- the guards ------------------------------------------------------------ */

/* The MAI takes a sample every N/(M+1) clocks: the best N/M for the rate with
   M+1 at most 256 (vc4_hdmi_audio_set_mai_clock's rational approximation).
   108 MHz gives 2250/1 for 48 kHz exactly; the Pi 3's 163,682,864 Hz
   228474/67. */
static uint32_t smp_for(uint32_t clock_hz, uint32_t rate)
{
    uint64_t best_n = 0, best_m = 1, best_err = ~0ULL;
    for (uint64_t m = 1; m <= 256; m++) {
        uint64_t n = ((uint64_t)clock_hz * m + rate / 2) / rate;
        if (!n || n >= (1u << 24)) continue;
        uint64_t got = n * rate, want = (uint64_t)clock_hz * m;
        uint64_t err = (got > want ? got - want : want - got) * 256 / m;    /* comparable across m */
        if (err < best_err) { best_err = err; best_n = n; best_m = m; }
    }
    return (uint32_t)(best_n << 8) | (uint32_t)(best_m - 1);
}

/* Pi 3: CPRMAN's HSM, the MAI clock. 1 when it runs (ENAB and BUSY): HDMI
   registers may be read. 0 when it does not: then none may. CPRMAN never
   stalls a read, so this is a guard any time. Whether its rate is known is
   a separate matter (hsm_known): from PLLD - source 6, which the firmware
   runs at 500 MHz - the rate follows from the divider (12.12): CM_HSMCTL
   $2D6, CM_HSMDIV $30E0, 163,682,864 Hz on a 3 A+. From another source the
   rate is unknown: my MAI is not started or repaired then, unless
   aga.hdmiforce says to go on at the rate last known, but it can be put
   back. The firmware sizes the HSM from the pixel clock, so a new mode can
   change the rate: SMP is worked out again, and a MAI already running gets
   it at the next look (smp_new). With reg_lock held, or at boot. */
static int hsm_known = 1;
static int hsm_pi3(void)
{
    uint32_t ctl = rd(CM_HSMCTL), div = rd(CM_HSMDIV) & 0xFFFFFF;
    aga_diag_set(AGA_D_HDMI_HSM_SET_KHZ, ctl);           /* the raw control word, for agastat */
    if ((ctl & 0x90) != 0x90)
        return 0;                                        /* not ENAB+BUSY: stopped */
    int plld = (ctl & 0x9F) == 0x96 && div >= 0x1000;    /* else another source, or nonsense */
    hsm_known = plld || aga_hdmi_force;
    if (plld && div != hsm_div) {
        hsm_div = div;
        mai_hz = (uint32_t)((500000000ULL * 4096) / div);
        mai_smp = smp_for(mai_hz, rate_hz[rate_i]);
        smp_new = mai_ours;                              /* the MAI runs on the old one */
        aga_diag_set(AGA_D_HDMI_HSM_KHZ, mai_hz / 1000);
    }
    return 1;
}

/* Is the HDMI picture moving? The housekeeper samples HVS channel 1's status
   at most once a millisecond while it may need to know; any change - the
   line, the mode, the frame count - stamps the time. Two samples further
   apart than 2 ms prove nothing (a stale sample against a fresh one), so the
   first after a gap only sets the base. agastat shows the masked word and
   the changes seen (hdmi_scan_stat, hdmi_scan_moves): state 2 with a picture
   on the screen and the count standing still means this field does not move
   on that board, not that the link is down. */
static uint32_t scan_stat;
static volatile uint64_t scan_at;                        /* the HDMI picture last seen moving, 0 never */
static uint64_t scan_prev;                               /* the last sample's time */

static void scan_sample(uint64_t now, uint64_t hz)
{
    if (now - scan_prev < hz / 1000) return;
    uint32_t st = rd(HVS_DISPSTAT1) & SCAN_MASK;
    if (now - scan_prev <= hz / 500 && st != scan_stat) {
        __atomic_store_n(&scan_at, now, __ATOMIC_RELEASE);
        aga_diag_inc(AGA_D_HDMI_SCAN_MOVES);
    }
    aga_diag_set(AGA_D_HDMI_SCAN_STAT, st);
    scan_stat = st;
    scan_prev = now;
}

/* A change between this read and one at most 100 us before it: the picture
   moves now, not just a millisecond ago. Read once a visit while a look
   waits for it - a line takes 15 to 64 us, the vertical blank at most about
   1.6 ms - so the look follows within a line or a blank. */
static uint32_t look_stat;
static uint64_t look_prev;

static int moving_now(uint64_t now, uint64_t hz)
{
    uint32_t st = rd(HVS_DISPSTAT1) & SCAN_MASK;
    int moved = st != look_stat && now - look_prev <= hz / 10000;
    look_stat = st;
    look_prev = now;
    if (moved) __atomic_store_n(&scan_at, now, __ATOMIC_RELEASE);
    return moved;
}

/* The housekeeper's guard. 1: the picture moves this moment and, on a Pi 3,
   the HSM runs - HDMI registers may be touched now. 0: the link is lost - no
   change for 200 ms, or a Pi 3's HSM stopped. -1: not seen moving in the last
   5 ms, or not yet in this look's reads - wait for the next visit, without
   giving the link up. The time between the move seen and the access is what
   is left of the old 200 ms window: the clock that stops in it still stalls
   the core. */
static int guard_ok(uint64_t now, uint64_t hz)
{
    uint64_t at = __atomic_load_n(&scan_at, __ATOMIC_ACQUIRE);
    if (!at || now - at > hz / 5) return 0;
    if (now - at > hz * 5 / 1000) return -1;
    if (pi3 && !hsm_pi3()) return 0;
    return moving_now(now, hz) ? 1 : -1;
}

/* core 0's: it watches the HVS itself for up to 3 ms and goes on at the first
   change between two reads (no cached stamp: a picture seen moving a while
   ago says nothing about the clock now) */
static int guard_now(void)
{
    uint64_t hz = cntfrq(), now = cntvct();
    uint32_t st = rd(HVS_DISPSTAT1) & SCAN_MASK;
    uint64_t end = now + hz * 3 / 1000;
    int moving = 0;
    while (!moving && cntvct() < end)
        moving = (rd(HVS_DISPSTAT1) & SCAN_MASK) != st;
    if (!moving) return 0;
    __atomic_store_n(&scan_at, cntvct(), __ATOMIC_RELEASE);
    return !pi3 || hsm_pi3();
}

/* ---- the HDMI side: all of it behind the guards, with reg_lock held -------- */

/* The audio infoframe into slot 7: type 0x84 v1 length 10, checksum 0x70, two
   channels. Its packet RAM is written only once the slot is off and the
   encoder has stopped sending it (STATUS clear): until then it is pending and
   a later visit looks again - Linux waits up to 100 ms for that, and neither
   the 68k's core nor the housekeeper may stop that long. Whether STATUS
   comes on after it is the once-a-second look's (check): a slot left dark is
   written again and counted as a repair. */
static void infoframe(void)
{
    slot_ours = 1;                                       /* mine from here on, pending or sent */
    uint32_t cfg = rd(hw->pkt_cfg);
    if (cfg & (1u << SLOT))
        wr(hw->pkt_cfg, cfg & ~(1u << SLOT));
    if (rd(hw->pkt_stat) & (1u << SLOT)) {
        info_pending = 1;
        return;
    }
    wr(SLOT_AT + 0, 0x000A0184);
    wr(SLOT_AT + 4, 0x00000170);
    for (int w = 2; w < 9; w++) wr(SLOT_AT + 4 * w, 0);  /* the rest zero, or the checksum is off */
    wr(hw->pkt_cfg, rd(hw->pkt_cfg) | (1u << SLOT));
    info_pending = 0;
}

/* the MAI as vc4_hdmi_audio_startup and _prepare leave it: stereo PCM at
   rate_i's rate from the MAI clock (mai_smp), and N for the sink */
static void program_mai(void)
{
    wr(hw->mai_ctl, M_RESET | M_FLUSH | M_FLAGS);
    wr(hw->phy, rd(hw->phy) & ~hw->phy_rng);             /* the RNG powered up */
    wr(hw->mai_smp, mai_smp);
    wr(hw->mai_ctl, M_RUN);
    wr(hw->mai_fmt, (2u << 16) | ((uint32_t)rate_mai[rate_i] << 8));   /* PCM, the rate */
    wr(hw->mai_thr, hw->thr);
    wr(hw->mai_cfg, (1u << 27) | (1u << 26) | 3);        /* format and bit reverse, channels 0-1 */
    wr(hw->ch_map, hw->chmap);
    wr(hw->aud_pkt, (1u << 29) | (1u << 24) | (8u << 10) | 3);
    wr(hw->crp, (1u << 24) | rate_n[rate_i]);            /* EXTERNAL_CTS_EN: CTS measured */
    wr(hw->cts0, 148500);                                /* 1080p50's, for show */
    wr(hw->cts1, 148500);
    mai_ours = 1;
    smp_new = 0;
}

/* vc4_hdmi_audio_shutdown and _reset, then the firmware's values back. My
   infoframe slot is switched off; the encoder stops sending it by itself,
   nothing waits for that. Of the PHY register only the RNG bit is mine, put
   back by itself as Linux's phy_rng_disable does: the rest is the firmware's
   link (lane power, and on a Pi 3 the drive), which it sets up afresh on a
   hotplug or a new mode - the word read at start would undo that (found by
   RGB2RTG). A MAI another driver took over is theirs: only my slot goes. */
static void restore(void)
{
    if (mai_ours) wr(hw->mai_ctl, M_FLAGS);              /* stopped: ENABLE clear */
    wr(hw->pkt_cfg, rd(hw->pkt_cfg) & ~(1u << SLOT));
    info_pending = 0;
    slot_ours = 0;
    if (!mai_ours) return;
    wr(hw->mai_ctl, M_RESET);
    wr(hw->mai_ctl, M_ERRORF);
    wr(hw->mai_ctl, M_FLUSH);
    for (int i = 0; i < S_PHY; i++) wr(saved_reg[i], saved[i]);
    wr(hw->phy, (rd(hw->phy) & ~hw->phy_rng) | (saved[S_PHY] & hw->phy_rng));
    wr(hw->mai_ctl, saved[S_CTL] & (15u << 4));          /* its channel count, not enabled */
    mai_ours = 0;
}

/* ---- the DMA side: its registers are always clocked ------------------------ */

/* A channel the firmware leaves to the ARM (brcm,dma-channel-mask) that
   nobody runs now - not ACTIVE, no control block loaded - or my own from
   before. The mask alone is not enough: a 68k driver (the SD card, Wi-Fi)
   may have taken one of those channels (found by RGB2RTG). CONBLK_AD 0 is an
   idle channel, as Linux's bcm2835_dma_abort reads it. */
static int dma_free(int c)
{
    uint32_t cb = rd(DMA_CB(c));
    return cb == cb_bus || (cb == 0 && !(rd(DMA_CS(c)) & CS_ACTIVE));
}

/* 0 when the channel is no longer free: a 68k driver on core 0 can take it
   any time, so it is looked at again right before the reset. A take between
   that look and the reset - a few bus cycles - cannot be ruled out without a
   lock both drivers keep: my reset would end that driver's transfer. */
static int start_dma(void)
{
    int c = dma_ch;
    volatile uint32_t *cb = ring + 2 * RING_MAX;         /* one block: the ring, then itself, at any rate */
    cb[0] = LE32(TI_PERMAP(hw->dreq) | TI_DEST_DREQ | TI_SRC_INC | TI_WAIT_RESP);
    cb[1] = LE32(ring_bus);
    cb[2] = LE32(hw->mai_data);
    cb[3] = LE32((ring_frames * 8));
    cb[4] = 0;
    cb[5] = LE32(cb_bus);
    cb[6] = cb[7] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!dma_free(c)) return 0;
    wr(DMA_ENABLE, rd(DMA_ENABLE) | (1u << c));
    wr(DMA_CS(c), CS_RESET);
    uint64_t end = cntvct() + cntfrq() / 10000;          /* 100 us at most */
    while ((rd(DMA_CS(c)) & CS_RESET) && cntvct() < end) ;
    wr(DMA_DEBUG(c), DBG_ERRORS);                        /* an old error would show as mine */
    wr(DMA_CB(c), cb_bus);
    wr(DMA_CS(c), CS_ACTIVE | CS_PRIO);
    return 1;
}

/* the one I have, while it still is free; else the first free one. A
   channel another driver took from me has left dma_mask (check): between
   that driver's transfers it would look free again. */
static int dma_pick(void)
{
    static const int pref[] = { 6, 5, 4, 2, 8, 9, 10 };
    int c = dma_ch;
    if (c < 0 || !(dma_mask & (1u << c)) || !dma_free(c)) {
        c = -1;
        for (unsigned i = 0; i < sizeof pref / sizeof pref[0] && c < 0; i++)
            if ((dma_mask & (1u << pref[i])) && dma_free(pref[i])) c = pref[i];
        dma_ch = c;
        aga_diag_set(AGA_D_HDMI_DMA_CH, (uint32_t)(c + 1));
    }
    return c >= 0;
}

/* my DMA stopped - only while the channel still runs my control block: a
   channel someone else has taken is theirs to run (found by RGB2RTG). Paused
   first, and reset once its last write to MAI_DATA is answered (TI's
   WAIT_RESP), as Linux 6.6's bcm2835_dma_abort waits for
   WAITING_FOR_OUTSTANDING_WRITES - 50 us at most, for a peripheral that
   never damn well answers. */
static void dma_stop(void)
{
    int c = dma_ch;
    if (c < 0 || rd(DMA_CB(c)) != cb_bus) return;
    wr(DMA_CS(c), 0);                                    /* pause, then forget the block */
    uint64_t end = cntvct() + cntfrq() / 20000;
    while ((rd(DMA_CS(c)) & CS_WAIT_WR) && cntvct() < end) ;
    wr(DMA_CS(c), CS_RESET);
    if (!dma_enable_was)
        wr(DMA_ENABLE, rd(DMA_ENABLE) & ~(1u << c));
}

/* The channel is not mine any more, found at a start or a look: my MAI and
   slot put back, state 5, and the retry a second on looks for another. A
   channel that runs someone else's control block (a 68k driver took it after
   I did) leaves dma_mask for the rest of the boot: between that driver's
   transfers it looks free, and the retry would take it back from that driver
   over and over, for fuck's sake. CONBLK_AD 0 there is only an idle channel
   and stays in the mask. With reg_lock held, behind the guards. */
static void dma_lost(int c)
{
    uint32_t cb = rd(DMA_CB(c));
    aga_diag_inc(AGA_D_HDMI_FIXUPS);
    set_state(H_NO_DMA);
    restore();
    if (cb && cb != cb_bus) dma_mask &= ~(1u << c);
    dma_ch = -1;
    aga_diag_set(AGA_D_HDMI_DMA_CH, 0);
    aga_diag_log("[AGA] HDMI audio: another driver took its DMA channel - looking for another");
}

/* ---- start and stop -------------------------------------------------------- */

static uint64_t last_check;                              /* the housekeeper's last once-a-second look */

/* with reg_lock held, behind the guards, the ring all silence (ring_silent);
   core0: the 68k's command, which may also print */
static void start(int core0)
{
    if (pi3 && !hsm_known) { set_state(H_NO_CLOCK); return; }   /* the HSM's rate unknown: tried again */
    uint32_t sched = rd(hw->sched), pkt = rd(hw->pkt_cfg), ctl = rd(hw->mai_ctl);
    if ((sched & 3) != 3 || !(pkt & 0x10000)) { set_state(H_NO_LINK); return; }   /* DVI, or no link */
    if ((pkt & (1u << SLOT)) && !ours()) { set_state(H_SLOT_BUSY); return; }
    /* a MAI already enabled that is not mine: another driver plays HDMI audio
       (a 68k driver that drives the MAI itself) - not reset under it */
    if ((ctl & M_ENABLE) && !mai_ours) { set_state(H_BUSY_OTHER); return; }
    if (!dma_pick()) { set_state(H_NO_DMA); return; }
    if (!mai_ours) {                                     /* the firmware's, not mine, left from a lost clock */
        const uint32_t regs[S_N] = { hw->mai_thr, hw->mai_fmt, hw->mai_smp, hw->ch_map, hw->mai_cfg, hw->aud_pkt,
                                     hw->crp, hw->cts0, hw->cts1, hw->phy, hw->mai_ctl };
        for (int i = 0; i < S_N; i++) saved[i] = rd(saved_reg[i] = regs[i]);
    }
    __atomic_store_n(&idle, 1, __ATOMIC_RELEASE);
    program_mai();
    infoframe();
    dma_enable_was = rd(DMA_ENABLE) & (1u << dma_ch);
    if (!start_dma()) { dma_lost(dma_ch); return; }      /* taken since the pick */
    /* The MAI is enabled before the DMA runs, as in 1.112: what it flagged
       while it waited for the first word goes, so that hdmi_mai_errs counts
       what happens while it plays */
    wr(hw->mai_ctl, M_RUN | M_FLAGS);
    ring_silent = 0;
    last_check = cntvct();                               /* the first look a whole second on */
    set_state(H_RUNNING);
    if (core0)
        kprintf("[AGA] HDMI audio: on - %u Hz stereo, %d bit, on %s, DMA channel %d, ring at %08x\n",
                rate_hz[rate_i], out_24() ? 24 : 16, hw->name, dma_ch, ring_bus);
    else
        aga_diag_log("[AGA] HDMI audio: on (tried again once a second)");   /* the console would hold this core up */
}

/* $5821 on, $5820 off - from agaboot SOUND, on core 0. It acts at once when
   the guards let it, and leaves the rest to the housekeeper's look a second
   later. */
void aga_hdmi_enable(int on)
{
    /* not ready: switched off, a Pi 4 with no clock at boot, no channel in
       the DT's mask. Channels dropped from the mask later do not count here:
       Off must still end the wish and the retries (state 5 with an empty
       mask is not retried) */
    if (!ready) return;
    wanted = on;
    lock_regs();
    int s = state;
    if (on && s != H_RUNNING) {
        if (!guard_now()) {
            set_state(s == H_NO_CLOCK ? H_NO_CLOCK : H_NO_LINK);
        } else {
            if (!ring_silent) { silence(0, ring_frames); ring_silent = 1; }
            prep_left = 0;
            start(1);
        }
    } else if (!on) {
        if (s == H_RUNNING) {
            set_state(H_OFF);
            /* core 3 and the housekeeper look at the state before they touch
               anything; give one already past that look the time to finish */
            uint64_t t = cntvct() + cntfrq() / 10000;
            while (cntvct() < t) ;
            /* The producer first, then the MAI (1.112 had it the other way
               round): the DMA's registers are always clocked, so it stops
               whatever the guard says, while the MAI is put back only when
               the guard lets me. An underflow the MAI flags in between goes
               with restore's reset. */
            dma_stop();
        } else if (s != H_NO_CLOCK) {
            set_state(H_OFF);                            /* no longer waiting for a link */
        }
        if (ours() && guard_now()) restore();            /* else the housekeeper's, once it may */
        if (s == H_RUNNING) kprintf("[AGA] HDMI audio: off\n");
    }
    unlock_regs();
}

/* What the EDID says, into the diagnostic log (agastat LOG): whose it is -
   the display name, which tells an AV receiver from the TV behind it - how
   many blocks came, and every short audio descriptor as its three bytes
   (format and channels, rates, sizes; aga_edid.h). */
static void edid_log(const uint8_t *e, int nb)
{
    static char line[80];
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    #define PUT(s) do { for (const char *p_ = (s); *p_ && o < 78; ) line[o++] = *p_++; } while (0)
    #define HEX(v) do { if (o < 76) { line[o++] = hex[((v) >> 4) & 15]; line[o++] = hex[(v) & 15]; } } while (0)
    PUT("[AGA] EDID: ");
    if (!nb) { PUT("none"); line[o] = 0; aga_diag_log(line); return; }
    char name[14] = "?";
    for (int d = 54; d <= 108; d += 18)
        if (!e[d] && !e[d + 1] && !e[d + 2] && e[d + 3] == 0xFC) {
            int i = 0;
            for (; i < 13 && e[d + 5 + i] != 0x0A && e[d + 5 + i] >= 32 && e[d + 5 + i] < 127; i++) name[i] = (char)e[d + 5 + i];
            name[i] = 0;
        }
    PUT(name);
    PUT(", blocks ");
    HEX(nb);
    PUT(" of ");
    HEX(e[126] + 1);
    PUT(", tags");
    for (int x = 1; x < nb; x++) { PUT(" "); HEX(e[128 * x]); }
    line[o] = 0;
    aga_diag_log(line);
    for (int x = 1; x < nb; x++) {
        const uint8_t *b = e + 128 * x;
        if (b[0] != 0x02) continue;
        int end = b[2] < 4 || b[2] > 127 ? 4 : b[2];
        for (int i = 4; i < end; i += 1 + (b[i] & 31)) {
            if ((b[i] >> 5) != 1) continue;
            o = 0;
            PUT("[AGA] EDID: audio, byte 3 ");
            HEX(b[3]);
            PUT(", SADs");
            for (int k = i + 1; k + 3 <= i + 1 + (b[i] & 31) && k + 3 <= 128; k += 3) {
                PUT(" ");
                HEX(b[k]); HEX(b[k + 1]); HEX(b[k + 2]);
            }
            line[o] = 0;
            aga_diag_log(line);
        }
    }
    #undef PUT
    #undef HEX
}

/* ---- boot, core 0, before the 68k: the clock and a DMA channel; no HDMI register */

static void ahi_report(uint32_t fill);     /* the AHI ring's counter (below) */

void aga_hdmi_boot(uint8_t *fb_va, uintptr_t fb_phys)
{
    if (aga_hdmi_audio_off) { set_state(H_SWITCHED_OFF); return; }
    ahi_report(0);                       /* "understood": aga-hdmi.audio looks for it before it asks for a ring */
    pi3 = aga_is_pi3();
    hw = pi3 ? &hw_pi3 : &hw_pi4;
    _Static_assert(RING_MAX * 8 + 32 <= AGA_HDMI_AUDIO_SIZE, "the ring and its control block fit AGA_HDMI_AUDIO_OFF's room");
    _Static_assert(AGA_D_HDMI_MAI_ERRS == 499 && AGA_D_HDMI_SCAN_MOVES == 501, "agastat names these by index");
    if (pi3) {                                           /* see the ring's note above */
        base_ring   = 30 * 192;                          /* 120 ms */
        base_lead   = 2304;                              /* 48 ms */
        base_high   = 4800;
        idle_ms     = 60;                                /* 48 + 32 ms of lead and silence stay ahead of it */
    }
    rate_params();
    ring = (volatile uint32_t *)(fb_va + AGA_HDMI_AUDIO_OFF);
    ring_bus = 0xC0000000u | (uint32_t)(fb_phys + AGA_HDMI_AUDIO_OFF);
    cb_bus = ring_bus + RING_MAX * 8;                    /* the control block's place at every rate */

    /* What the screen takes (aga_edid.h): its EDID, which only the mailbox
       gives - mine alone now, at boot. Block 0, then the extensions it
       announces (at most 3). A screen that says nothing about audio, or no
       EDID at all, gets 44.1 and 48 kHz in 16 bits, as before. */
    {
        static uint8_t ed[4 * 128];
        int nb = 0;
        if (get_edid_block(0, ed) == 0) {
            nb = 1;
            int ext = ed[126] > 3 ? 3 : ed[126];
            while (nb <= ext && get_edid_block((uint32_t)nb, ed + 128 * nb) == 0) nb++;
        }
        uint32_t a = nb ? edid_audio(ed, nb) : 0;
        edid_log(ed, nb);
        if (a & EDID_SEEN) {
            sink_rates = (a & EDID_RATES) | (1u << R_48K);   /* 48 kHz always: what HDMI sound began with */
            sink_24 = (a & EDID_24) != 0;
        }
        kprintf("[AGA] HDMI audio: the screen's EDID %s - %s%s%s%s%s%s kHz, %d bit\n",
                !nb ? "could not be read" : (a & EDID_SEEN) ? "lists LPCM" : "lists no LPCM",
                sink_rates & 1 ? " 44.1" : "", sink_rates & 2 ? " 48" : "", sink_rates & 4 ? " 88.2" : "",
                sink_rates & 8 ? " 96" : "", sink_rates & 16 ? " 176.4" : "", sink_rates & 32 ? " 192" : "",
                sink_24 ? 24 : 16);
    }
    make_tables();
    silence(0, ring_frames);
    ring_silent = 1;
    ahi_report(0);                                       /* with the screen's rates in it */

    /* Is the HDMI state machine clock running, and how fast is the MAI
       clock? Pi 3: CPRMAN (hsm_pi3). Pi 4: the HSM is the firmware's clock
       13, MEASURED - GET_CLOCK_RATE gives the rate the ARM asked for, and
       under Emu68 nobody asked: it read 0 on a CM4 showing a picture
       (2026-10-02). The MAI itself runs from the DVP's fixed 108 MHz. The
       mailbox is mine alone only now, so this is the Pi 4's only look. */
    uint32_t clk;
    if (pi3) {
        clk = hsm_pi3() && hsm_div && hsm_known ? mai_hz : 0;
    } else {
        uint32_t hsm = get_clock_rate_measured(HSM_CLOCK);
        aga_diag_set(AGA_D_HDMI_HSM_SET_KHZ, get_clock_rate(HSM_CLOCK) / 1000);
        aga_diag_set(AGA_D_HDMI_HSM_KHZ, hsm / 1000);
        clk = hsm ? 108000000 : 0;
    }
    if (!clk && !aga_hdmi_force) {                       /* aga.hdmiforce: the user knows better */
        set_state(H_NO_CLOCK);
        kprintf("[AGA] HDMI audio: no HDMI clock (no screen at boot)%s\n",
                pi3 ? " - looked at again while sound is wanted" : " - left alone");
        if (!pi3) return;
    }
    if (!clk) clk = pi3 ? 163682864 : 108000000;        /* forced, or a Pi 3 waiting: the usual rate */
    mai_hz = clk;
    mai_smp = smp_for(clk, rate_hz[rate_i]);

    /* a channel the firmware leaves to the ARM: brcm,dma-channel-mask */
    dma_mask = hw->dma_mask;
    of_node_t *n = dt_find_node("/soc/dma@7e007000");
    if (!n) n = dt_find_node("/soc/dma-controller@7e007000");
    if (n) dma_mask = dt_get_property_value_u32(n, "brcm,dma-channel-mask", dma_mask, 0);
    dma_pick();                                          /* again at every start: one may be taken by then */
    if (!dma_mask) { set_state(H_NO_DMA); return; }
    /* only the addresses now: the values are read at the first start, behind
       the guards - with no clock running the first read would hang the boot */
    const uint32_t regs[S_N] = { hw->mai_thr, hw->mai_fmt, hw->mai_smp, hw->ch_map, hw->mai_cfg, hw->aud_pkt,
                                 hw->crp, hw->cts0, hw->cts1, hw->phy, hw->mai_ctl };
    for (int i = 0; i < S_N; i++) saved_reg[i] = regs[i];
    ready = 1;
    if (state != H_NO_CLOCK) set_state(H_OFF);
    kprintf("[AGA] HDMI audio: %s on %s (off until the Chipset menu's Sound turns it on), "
            "MAI clock %u Hz, SMP %08x, DMA channel %d\n",
            state == H_NO_CLOCK ? "waiting for the HSM" : "ready", hw->name, mai_hz, mai_smp, dma_ch);
}

/* ---- core 3: the mix in --------------------------------------------------- */

static uint32_t wpos;                    /* next ring frame to write */
static uint32_t sil_end;                 /* silence is written up to here */
static int32_t  hist[4][2];              /* the last four input frames */
static uint64_t ph;                      /* 32.32: where I am past hist[1] */
static uint64_t step_nom;                /* 27,928 / 48,000 in 32.32 (step_rate / step_out) */
static uint32_t step_rate, step_out;     /* the input and output rates step_nom was made for */
extern uint32_t aga_audio_rate(void);    /* aga_audio.c: the real Paula's sample rate */
static int32_t  lead_q4;                 /* the lead, smoothed, in 1/16 frames */
static volatile uint64_t last_feed;
/* core 3 and the housekeeper's idle fill, which both write the ring: the
   feed counts itself in and out (odd while it runs), and a piece of the fill
   is written only under fill_busy, after a fenced look at that count - so
   either the fill sees a feed has begun and writes nothing, or the feed waits
   the few microseconds the piece takes. The fill used to look only at
   last_feed, stamped as a feed ENDS, and could lay a piece of silence over
   sound the feed was writing (found by RGB2RTG). */
static volatile uint32_t feed_seq;
static volatile int fill_busy;

static inline uint32_t dma_frame(int c)
{
    uint32_t off = rd(DMA_SRC(c)) - ring_bus;
    return off < (ring_frames * 8) ? off / 8 : 0;
}

/* The A500/A1200 filters (aga_hdmi_filter.h), off unless agaboot SOUND FILTER
   chose one: $5830 off, $5831 A500, $5834 A1200 from the 68k's trap on core 0
   (a plain store there), the power LED from the chipset loop's look at CIA-A
   once a frame. The filters run on core 3 in the feed, at HDMI's rate (hf_k). */
static volatile int filt_mode, filt_led;
static hf_state_t filt[2];
static int filt_seen;

void aga_hdmi_filter(int mode) { filt_mode = (mode == HF_A500 || mode == HF_A1200) ? mode : HF_OFF; }
void aga_hdmi_led(int lit)                 /* core 3, once a frame */
{
    filt_led = lit ? 1 : 0;
    aga_diag_set(AGA_D_HDMI_FILTER, (uint32_t)(filt_mode | (lit ? 16 : 0)));
}

/* Catmull-Rom between b and c; t in 1/65536 */
static inline int32_t cubic(int32_t a, int32_t b, int32_t c, int32_t d, int32_t t)
{
    int64_t t1 = t, t2 = (t1 * t1) >> 16, t3 = (t2 * t1) >> 16;
    int64_t v = 2 * (int64_t)b
              + (((int64_t)(c - a) * t1) >> 16)
              + (((int64_t)(2 * a - 5 * b + 4 * c - d) * t2) >> 16)
              + (((int64_t)(3 * (b - c) + d - a) * t3) >> 16);
    return (int32_t)(v >> 1);
}

/* The Sound window's HDMI volume (agaboot SOUND VOLUME, $584n from the 68k's
   trap on core 0: a plain store): 100, 150, 200, 300 or 400 %. 200 is
   1.112's and 1.113's GAIN 2; above 100 % the peaks are rounded off by
   aga_mix.h's soft knee instead of clipped. */
static volatile int32_t hdmi_gain16 = 2 << 16;
void aga_hdmi_volume(int i) { if (i >= 0 && i < 5) hdmi_gain16 = (int32_t)((mix_vol_pc(i) << 16) / 100); }

/* ---- AHI (1.114): the aga-hdmi.audio driver's stereo, mixed in -----------------
   An AHI program's sound, mixed by AHI itself into stereo - 16 bits, or 32 in
   its HiFi modes - goes into a ring in the 68k's fast RAM
   (tools/ahi/aga-hdmi.c) - Pi memory, identity mapped, cached and coherent,
   so it is read here where it lies. HDMI runs at the ring's rate (set_rate),
   and the feed takes one ring frame for each HDMI frame it writes, added to
   the Paula mix after its volume and filters (AHI's sound is mastered at
   full scale, and the A500/A1200 filters are the Amiga's analog output, not
   this), and outside the sandbox core 3 runs the feed for it with a silent
   Paula (aga_hdmi_pump). The ring's header, big-endian like everything the
   68k writes: magic, frames (a power of two), the frames written and the
   frames read since it began - the driver writes the first, I the second,
   each counting on without wrapping at the ring - the rate, and flags: bit 0
   the frames are two 32-bit samples (else two 16-bit). The driver publishes
   its count with a CAS, which the JIT closes with a barrier, after the
   samples; I read the count acquiring, so the samples before it are there.
   Words: $58A0 + E4(n) four more bits of the ring's address, highest first;
   $5895 the ring from that address, $5894 none (both from core 0's trap).

   Counter AGA_D_AHI (511), which the driver reads: bit 0 the kernel takes
   rings, bit 1 one is on, bit 2 HDMI sound plays, bit 3 the screen takes 24
   bits, bits 4-9 the rates it takes (rate_hz's order; the driver offers
   those), bits 10-12 the rate HDMI runs at, bit 13 the ring's frames are 32
   bits, bit 14 Paula band-limited, bit 15 it knows AHI_LOWLAT (1.116);
   bits 16-23 the frames waiting / 64, bits
   24-31 the times the ring ran dry (at most 255). */
extern uintptr_t mmu_virt2phys(uintptr_t virt);
#define AHI_MAGIC 0x41484931u                    /* "AHI1" */
#define AHI_S32   1u                             /* flags: 32-bit samples */
#define AHI_LOWLAT 2u                            /* ... low latency: a 10 ms start buffer (1.116, driver 6.2) */
struct ahi_hdr { uint32_t magic, frames, wr, rd, rate, flags, res[2]; };
static struct ahi_hdr *volatile ahi_live __attribute__((aligned(64)));   /* its own line: core 3 polls it parked */
static uint32_t ahi_latch;
static struct ahi_hdr *ahi_cur;                  /* core 3's: the ring the feed reads */
static uint32_t ahi_rd, ahi_mask, ahi_under;
static const int16_t *ahi_pcm;
static const int32_t *ahi_pcm32;                 /* ... when its frames are 32-bit */
static int ahi_seen;                             /* the driver has written something */
/* The start buffer (1.116, RGB2RTG's 0.75-dev53): a ring is played once the
   driver is 40 ms ahead, or 100 ms after its first frame came - new, or
   after it ran dry. Read at once, a player that stalled right after its
   first buffer (one opening a file on the SD card, whose driver spins
   while it reads) left holes, heard as shitty crackling; held, its start comes
   40 ms later, whole. A ring that runs dry waits the same way: one gap,
   not a crackle of many. 40 ms: driver 6.2 keeps 42 ms more than its
   buffers; an older one, never that far ahead with small buffers, starts
   after the 100 ms. A low latency ring (AHI_LOWLAT, driver 6.2's low
   latency modes, about a buffer and 12 ms) starts at 10 ms: a game's
   sound effects heard sooner, a stall of the 68k past that margin a gap. */
static int ahi_primed;                           /* the feed plays the ring */
static uint64_t ahi_wait_t0;                     /* the first frame of the wait came (0 none yet) */
static int ahi_lowlat;                           /* the ring has AHI_LOWLAT */
static volatile int paula_bl;                    /* Paula on HDMI band-limited (below) */

static void ahi_report(uint32_t fill)
{
    struct ahi_hdr *h = ahi_live;
    uint32_t u = ahi_under > 255 ? 255 : ahi_under;
    aga_diag_set(AGA_D_AHI, 1u | (h ? 2u : 0u) | (state == H_RUNNING ? 4u : 0u) | (out_24() ? 8u : 0u)
                            | (offer_rates() & 0x3F) << 4 | (uint32_t)rate_i << 10
                            | (h && (h->flags & AHI_S32) ? 1u << 13 : 0u) | (paula_bl ? 1u << 14 : 0u)
                            | 1u << 15                    /* 1.116: it knows AHI_LOWLAT */
                            | (fill / 64 > 255 ? 255u : fill / 64) << 16 | u << 24);
}

/* Another rate (core 0: an AHI ring that asks for one, the sandbox's start).
   Sound that plays stops as Off stops it - the state first, which the feed
   and the housekeeper's fills look at, then the DMA - and a feed already
   under way is let finish: it counts with the old ring. Then the ring, the
   tables and the MAI's clock for the new rate, the ring all silence, and the
   start: the MAI programmed afresh. The screen locks on to the new rate, a
   short gap. A start the guard does not let through now is the
   housekeeper's, a second later, as after a lost link. force: the same
   way at the same rate, for new tables (16 or 24 bits). */
static void set_rate(int i, int force)
{
    if (!ready || (i == rate_i && !force)) return;
    lock_regs();
    int was = state == H_RUNNING;
    if (was) {
        set_state(H_OFF);
        uint64_t t = cntvct() + cntfrq() / 10000;
        while (cntvct() < t) ;
        t = cntvct() + cntfrq() / 50;                    /* a feed is well under a millisecond: 20 ms at most */
        while ((__atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST) & 1) && cntvct() < t) __asm__ volatile("yield");
        dma_stop();
    }
    __atomic_store_n(&rate_i, i, __ATOMIC_RELEASE);
    rate_params();
    make_tables();
    mai_smp = smp_for(mai_hz, rate_hz[i]);
    prep_left = 0;
    silence(0, ring_frames);
    ring_silent = 1;
    if (was) {
        if (guard_now()) start(1);
        else set_state(H_NO_LINK);
    } else {
        kprintf("[AGA] HDMI audio: %u Hz from its next start\n", rate_hz[i]);
    }
    unlock_regs();
}

/* the sandbox starts: 48 kHz again, unless an AHI program plays on - or
   the fixed rate the Sound window chose */
void aga_hdmi_rate_default(void)
{
    int f = fixed_ri[rate_mode];
    if (f >= 0) set_rate(f, 0);
    else if (!__atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE)) set_rate(R_48K, 0);
    ahi_report(0);
}

/* $5868 + E2(m), core 0: the Sound window's HDMI sound rate (rate_mode).
   HDMI goes to the fixed rate, or stays at one still offered, else to 48
   kHz; an AHI ring at a rate no longer offered is let go - its driver then
   keeps time by itself, silent, until the program starts again. */
void aga_hdmi_rate_mode(int m)
{
    if (m < 0 || m > 3 || m == rate_mode) return;
    int was24 = out_24();
    rate_mode = m;
    struct ahi_hdr *h = __atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE);
    int keep = -1;
    for (int i = 0; h && i < N_RATES; i++)
        if (h->rate == rate_hz[i] && (offer_rates() & (1u << i))) keep = i;
    if (h && keep < 0) {
        __atomic_store_n(&ahi_live, (struct ahi_hdr *)0, __ATOMIC_RELEASE);
        aga_diag_log("[AGA] AHI: ring let go - its rate is no longer offered");
    }
    int f = fixed_ri[m];
    int want = f >= 0 ? f : keep >= 0 ? keep : (offer_rates() >> rate_i) & 1 ? rate_i : R_48K;
    set_rate(want, out_24() != was24);
    ahi_report(0);
    aga_diag_log(m == 0 ? "[AGA] HDMI audio: rate follows the program, within the EDID"
               : m == 1 ? "[AGA] HDMI audio: rate follows the program, any rate"
               : m == 2 ? "[AGA] HDMI audio: rate fixed at 96 kHz" : "[AGA] HDMI audio: rate fixed at 192 kHz");
}

void aga_hdmi_ahi_nibble(int d) { ahi_latch = (ahi_latch << 4) | ((uint32_t)d & 15u); }

/* the whole range [a, a + n) identity-mapped Pi memory (68k fast RAM) */
static int ahi_mapped(uint32_t a, uint32_t n)
{
    for (uint32_t p = a & ~4095u; p < a + n; p += 4096)
        if (mmu_virt2phys((uintptr_t)p) != (uintptr_t)p) return 0;
    return 1;
}

void aga_hdmi_ahi(int on)
{
    if (!on) {
        __atomic_store_n(&ahi_live, (struct ahi_hdr *)0, __ATOMIC_RELEASE);
        ahi_report(0);
        return;
    }
    uint32_t a = ahi_latch;
    /* longword-aligned is all the header needs: AllocVec hands out blocks
       four bytes past an eight-byte boundary (found on the A500, 2026-10-11) */
    int why = a < 0x200000u ? 1 : (a & 3) ? 2 : a > 0xFFF00000u ? 3 : !ahi_mapped(a, sizeof(struct ahi_hdr)) ? 4 : 0;
    struct ahi_hdr *h = (struct ahi_hdr *)(uintptr_t)a;
    uint32_t n = why ? 0 : h->frames;
    int ri = -1;                                  /* the ring's rate, one the screen takes */
    for (int i = 0; !why && i < N_RATES; i++)
        if (h->rate == rate_hz[i] && (offer_rates() & (1u << i))) ri = i;
    uint32_t fsize = !why && (h->flags & AHI_S32) ? 8 : 4;
    if (!why) why = h->magic != AHI_MAGIC ? 5 : ri < 0 ? 6 : (n < 256 || n > 65536 || (n & (n - 1))) ? 7
                  : !ahi_mapped(a, sizeof(struct ahi_hdr) + n * fsize) ? 8 : (h->flags & ~(AHI_S32 | AHI_LOWLAT)) ? 9 : 0;
    if (why) {
        /* "[AGA] AHI: ring at XXXXXXXX refused (N)": 1 chip RAM, 2 alignment, 3 range,
           4 not mapped, 5 magic, 6 a rate the screen does not take, 7 size, 8 the
           frames not mapped, 9 flags this kernel does not know */
        static char line[] = "[AGA] AHI: ring at XXXXXXXX refused (N)";
        for (int i = 0; i < 8; i++) line[19 + i] = "0123456789abcdef"[(a >> (28 - 4 * i)) & 15];
        line[37] = (char)('0' + why);
        aga_diag_log(line);
        return;
    }
    set_rate(ri, 0);                              /* HDMI at the ring's rate - before the feed reads it */
    __atomic_store_n(&ahi_live, h, __ATOMIC_RELEASE);
    ahi_report(0);
    __asm__ volatile("sev");                      /* core 3, parked: run the feed for it */
    aga_diag_log("[AGA] AHI: ring on");
}

int aga_hdmi_ahi_live(void) { return __atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE) != 0; }

/* ---- the feed: what both of Paula's paths share ----------------------------- */

struct feed {
    int ri, fm, fl, resynced;
    const int32_t *fk;
    int32_t gain16, ppm;
    struct ahi_hdr *ah;
    uint32_t ahi_wr;
    int ahi_hold;                        /* the ring ran dry and waits: these frames are holes */
};

/* A feed's start: 0 when it may not run now. Counts itself in, resyncs on a
   pause or a lead out of its range (f->resynced: the caller's resampler
   starts afresh too), works out the rate trim, and takes the filters, the
   volume and the AHI ring as they are now. */
static int feed_begin(struct feed *f)
{
    /* acquiring: the ring's counts set_rate wrote before the start are seen */
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != H_RUNNING) return 0;
    int c = dma_ch;                      /* the housekeeper may drop a channel taken from me */
    if (c < 0) return 0;
    __atomic_add_fetch(&feed_seq, 1, __ATOMIC_SEQ_CST);  /* odd: a feed runs */
    __asm__ volatile("dmb ish" ::: "memory");
    while (__atomic_load_n(&fill_busy, __ATOMIC_SEQ_CST)) __asm__ volatile("yield");
    f->ri = rate_i;
    f->resynced = 0;
    uint32_t rpos = dma_frame(c);
    uint32_t lead = (wpos + ring_frames - rpos) % ring_frames;
    /* The pause is judged here as well as by the housekeeper's flag: a feed
       that began just as the housekeeper decided on the fill could read the
       flag before it was set (found by review). Signed: last_feed is mine. */
    int stale = (int64_t)(cntvct() - last_feed) > (int64_t)(cntfrq() * idle_ms / 1000);
    int was_idle = __atomic_load_n(&idle, __ATOMIC_SEQ_CST) || stale;
    if (was_idle || lead < lead_low || lead > lead_high) {
        /* (re)start a lead ahead of the DMA, silence in between and beyond */
        if (!was_idle) aga_diag_inc(AGA_D_HDMI_RESYNCS);
        wpos = (rpos + lead_target) % ring_frames;
        silence((rpos + 8) % ring_frames, lead_target - 8 + horizon);
        sil_end = (wpos + horizon) % ring_frames;
        lead = lead_target;
        lead_q4 = lead_target << 4;
        /* the filters too: the ring before wpos is silence, so they go on
           from silence, not from the sound before the pause (found by review) */
        for (int c2 = 0; c2 < 2; c2++)
            for (int k = 0; k < 5; k++) filt[c2].rc[k] = 0;
        f->resynced = 1;
        __atomic_store_n(&idle, 0, __ATOMIC_RELEASE);
    }
    /* the rate: Paula's clock against HDMI's, trimmed by the lead - 4 ppm per
       frame of error, at most 0.3% (5 cents, inaudible) */
    lead_q4 += ((int32_t)(lead << 4) - lead_q4) >> 5;
    int32_t ppm = ((lead_q4 >> 4) - lead_target) * 4;
    if (ppm > 3000) ppm = 3000;
    if (ppm < -3000) ppm = -3000;
    f->ppm = ppm;
    aga_diag_set(AGA_D_HDMI_LEAD, lead);
    aga_diag_set(AGA_D_HDMI_PPM, (uint32_t)ppm);
    f->fm = filt_mode;
    f->fl = filt_led;
    f->fk = hf_k[f->ri];
    f->gain16 = hdmi_gain16;
    /* AHI: the ring now, and what the driver has written into it */
    struct ahi_hdr *ah = __atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE);
    if (ah != ahi_cur) {
        ahi_cur = ah;
        if (ah) {
            ahi_rd = ah->rd;
            ahi_mask = ah->frames - 1;
            ahi_pcm = (const int16_t *)(ah + 1);
            ahi_pcm32 = (ah->flags & AHI_S32) ? (const int32_t *)(ah + 1) : 0;
            ahi_seen = 0;
            ahi_under = 0;
            ahi_primed = 0;
            ahi_wait_t0 = 0;
            ahi_lowlat = (ah->flags & AHI_LOWLAT) != 0;
        }
    }
    f->ah = ah;
    f->ahi_wr = ah ? __atomic_load_n(&ah->wr, __ATOMIC_ACQUIRE) : 0;
    if (ah && f->ahi_wr - ahi_rd > ahi_mask + 1) ahi_rd = f->ahi_wr - (ahi_mask + 1) / 2;   /* I fell a lap behind: half a ring */
    f->ahi_hold = 0;
    if (ah && !ahi_primed) {                             /* the start buffer */
        uint64_t at = cntvct();
        uint32_t fill = f->ahi_wr - ahi_rd;
        if (!fill)
            ahi_wait_t0 = 0;
        else if (!ahi_wait_t0)
            ahi_wait_t0 = at;
        if (fill >= at_rate(ahi_lowlat ? 480 : 1920) || (fill && at - ahi_wait_t0 >= cntfrq() / 10)) {
            ahi_primed = 1;
        } else {
            f->ah = 0;                                   /* not yet: Paula alone, the ring kept */
            f->ahi_hold = ahi_seen;                      /* after it ran dry: a hole */
        }
    }
    if (f->fm != filt_seen) {                            /* a new model starts from silence */
        for (int c2 = 0; c2 < 2; c2++)
            for (int k = 0; k < 5; k++) filt[c2].rc[k] = 0;
        filt_seen = f->fm;
    }
    return 1;
}

/* One HDMI sample out: Paula's, at the 16-bit scale and before the
   filters, through the filters and the volume into the top 16 of 24, the
   AHI ring's frame added. */
static inline void feed_emit(struct feed *f, int32_t pl, int32_t pr)
{
    int32_t l = 256 * mix_level(hf_run_k(&filt[0], f->fm, f->fl, pl, f->fk), f->gain16);
    int32_t r = 256 * mix_level(hf_run_k(&filt[1], f->fm, f->fl, pr, f->fk), f->gain16);
    if (f->ah) {                                         /* one AHI frame for each HDMI frame, the same rate */
        if (ahi_rd != f->ahi_wr) {
            uint32_t k = ahi_rd & ahi_mask;
            if (ahi_pcm32) {                             /* 32 bits: the top 24 */
                l += ahi_pcm32[2 * k] >> 8;
                r += ahi_pcm32[2 * k + 1] >> 8;
            } else {
                l += 256 * ahi_pcm[2 * k];
                r += 256 * ahi_pcm[2 * k + 1];
            }
            ahi_rd++;
            ahi_seen = 1;
        } else if (ahi_seen) {
            ahi_under++;
            if (ahi_primed) {                            /* dry: the start buffer again */
                ahi_primed = 0;
                ahi_wait_t0 = 0;
            }
        }
        l = clamp24(l);
        r = clamp24(r);
    } else if (f->ahi_hold) {
        ahi_under++;
    }
    put_frame(wpos, l, r);
    if (++wpos >= ring_frames) wpos = 0;
}

/* A feed's end: silence kept ahead, the AHI ring's read count published,
   and the feed counted out. */
static void feed_end(struct feed *f)
{
    /* keep horizon frames of silence beyond the sound, in case it stops here */
    uint32_t have = (sil_end + ring_frames - wpos) % ring_frames;
    if (have > horizon) have = 0;                        /* I ran past it */
    silence((wpos + have) % ring_frames, horizon - have);
    sil_end = (wpos + horizon) % ring_frames;
    if (f->ah) {
        __atomic_store_n(&f->ah->rd, ahi_rd, __ATOMIC_RELEASE);   /* the driver paces itself by it */
        ahi_report(f->ahi_wr - ahi_rd);
    }
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(&last_feed, cntvct(), __ATOMIC_RELEASE);
    __atomic_add_fetch(&feed_seq, 1, __ATOMIC_SEQ_CST);  /* even: done */
}

/* Classic: the same frames aga_audio_stream gets - stereo, at the real
   Paula's rate - resampled to HDMI's with a Catmull-Rom */
void aga_hdmi_feed(const int16_t *lr, int frames)
{
    struct feed f;
    if (frames <= 0 || !feed_begin(&f)) return;
    /* the rate the frames come at (aga_audio_rate): the real Paula's,
       27,928 a second on a PAL crystal - kept at 1.112's 3546895 / 127 to
       the bit - and 27,965 on an NTSC one, where the glue now asks the mix
       for that many - and the rate HDMI runs at */
    uint32_t rate = aga_audio_rate(), out = rate_hz[f.ri];
    if (rate != step_rate || out != step_out) {
        step_rate = rate;
        step_out = out;
        step_nom = rate == 3546895u / 127u ? ((uint64_t)(3546895u) << 32) / (127u * (uint64_t)out)
                                          : ((uint64_t)rate << 32) / out;
    }
    if (f.resynced) {
        ph = 0;
        for (int i = 0; i < 4; i++) hist[i][0] = hist[i][1] = 0;
    }
    uint64_t step = step_nom + (uint64_t)(((int64_t)step_nom * f.ppm) / 1000000);
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            hist[0][ch] = hist[1][ch]; hist[1][ch] = hist[2][ch]; hist[2][ch] = hist[3][ch];
            hist[3][ch] = lr[2 * i + ch];
        }
        while (ph < (1ULL << 32)) {
            int32_t t = (int32_t)(ph >> 16);
            feed_emit(&f, cubic(hist[0][0], hist[1][0], hist[2][0], hist[3][0], t),
                          cubic(hist[0][1], hist[1][1], hist[2][1], hist[3][1], t));
            ph += step;
        }
        ph -= 1ULL << 32;
    }
    feed_end(&f);
}

/* ---- Paula band-limited (aga_blep.h): the Sound window's "Paula on HDMI" ----
   $5899 band-limited, $5898 classic (from core 0's trap: a plain store); the
   glue asks the core for Paula's steps while it is on and hands them here
   instead of the classic frames. */
static blep_t blep;
static int blep_inited, blep_live;
static uint64_t blep_now;                /* the colour clock the last feed went up to */

void aga_hdmi_paula(int bl)
{
    paula_bl = bl ? 1 : 0;
    ahi_report(0);
    aga_diag_log(bl ? "[AGA] HDMI audio: Paula band-limited" : "[AGA] HDMI audio: Paula classic");
}
int aga_hdmi_paula_bl(void) { return paula_bl; }

/* Paula's steps since the last call (aga_audio_steps), every step before
   colour clock now among them; cck_hz the colour clocks the emulation runs
   a second */
void aga_hdmi_feed_steps(const aga_step_t *s, int n, uint64_t now, uint32_t cck_hz)
{
    struct feed f;
    if (!feed_begin(&f)) { blep_live = 0; return; }
    if (!blep_inited) { blep_init(&blep, f.ri, now, 0); blep_inited = 1; }
    /* HDMI samples per colour clock, 32.32: fewer while the lead is long */
    uint64_t rnom = ((uint64_t)rate_hz[f.ri] << 32) / cck_hz;
    uint64_t r = rnom - (uint64_t)(((int64_t)rnom * f.ppm) / 1000000);
    /* Afresh after a resync (silence before wpos), at a new rate, or when
       the colour clock jumped: from the last feed's end when it follows on
       from it, else from the first step or now. Otherwise on from the last
       feed's end, at the new trim. */
    int follows = blep_live && now >= blep_now && now - blep_now <= cck_hz / 10;
    if (f.resynced || !follows || f.ri != blep.ri) {
        uint64_t t = now;
        if (follows) t = blep_now;
        else for (int i = 0; i < n; i++) if (s[i].t < t) t = s[i].t;
        blep_reset(&blep, f.ri, t, r);
        blep_live = 1;
    } else {
        blep_retime(&blep, blep_now, r);
    }
    for (int i = 0; i < n; i++) blep_step(&blep, s[i].t, s[i].ch, s[i].level);
    int32_t w16;
    int sw;
    aga_audio_width_swap(&w16, &sw);
    for (uint32_t k = blep_ready(&blep, now); k; k--) {
        int32_t l, r2;
        blep_out(&blep, &l, &r2);
        l *= 2;                                          /* two channels a side, as the core's mix: 32,512 full */
        r2 *= 2;
        if (w16 != 0x10000 || sw) {                      /* the Sound window's stereo, as aga_audio_stream does it */
            int16_t a = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            int16_t b = (int16_t)(r2 > 32767 ? 32767 : r2 < -32768 ? -32768 : r2);
            mix_stereo(&a, &b, w16, sw);
            l = a;
            r2 = b;
        }
        feed_emit(&f, l, r2);
    }
    blep_now = now;
    feed_end(&f);
}

/* Core 3, parked (the sandbox down) while an AHI ring is on: the feed with a
   silent Paula, every 2 ms as many frames as the real Paula's rate gives, so
   the AHI stream plays on the plain desktop too, through the same lead and
   rate control. The wait reads only the clock and two flags on lines of
   their own (aga_glue.c's park explains why a polled line matters). Back to
   the park as soon as the chipset is wanted or the ring is off. */
void aga_hdmi_pump(void)
{
    static int16_t zeros[2 * 64];
    static uint64_t last, frac;
    uint64_t f = cntfrq(), now = cntvct();
    if (!last || now - last > f / 20) { last = now; frac = 0; }     /* (re)start after a pause */
    while (cntvct() - last < f / 500) {
        if (aga_enabled || !aga_hdmi_ahi_live()) return;
        aga_nap_before(last + f / 500); /* with aga.sleep: asleep between the 2 ms steps (aga_glue.h) */
    }
    now = cntvct();
    uint64_t due = (now - last) * (uint64_t)aga_audio_rate() + frac;
    uint32_t n = (uint32_t)(due / f);
    frac = due % f;
    last = now;
    while (n) {
        int k = n > 64 ? 64 : (int)n;
        aga_hdmi_feed(zeros, k);
        n -= (uint32_t)k;
    }
}

/* ---- the housekeeper: silence when the mix stops, and a look once a second - */

/* While it plays, once a second, behind the guards: is the DMA still mine
   and running, the MAI programmed (a hotplug lets the firmware set the link
   up afresh), my infoframe sent? The MAI's and the DMA's error flags are
   cleared, and a new HSM rate written. With reg_lock held. */
static void check(void)
{
    int c = dma_ch;
    uint32_t cb = rd(DMA_CB(c)), cs = rd(DMA_CS(c));
    if (cb != cb_bus) {
        /* Someone else's control block (a 68k driver took the channel after
           I did): theirs now, not reset under it (dma_lost). CONBLK_AD 0
           and not running is only an idle channel: someone reset it and
           nobody runs it - mine again in place, from the ring's start, as
           1.112 restarted a stopped channel. */
        if (cb || (cs & CS_ACTIVE) || !start_dma()) { dma_lost(c); return; }
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        aga_diag_log("[AGA] HDMI audio: its DMA channel was reset - started again");
        cs = rd(DMA_CS(c));
    }
    /* A DMA error (an AXI read error, its FIFO) is cleared in DEBUG. A
       channel still running goes on where it is; only one that stopped is
       started again, from the ring's start - a jump, and core 3 resyncs. */
    if (cs & CS_ERROR) {
        wr(DMA_DEBUG(c), rd(DMA_DEBUG(c)) & DBG_ERRORS);
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
    }
    if (!(cs & CS_ACTIVE)) {
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        if (!start_dma()) { dma_lost(c); return; }
    }
    uint32_t ctl = rd(hw->mai_ctl), err = ctl & M_FLAGS, n = rd(hw->crp) & 0xFFFFF, our_n = rate_n[rate_i];
    if ((ctl & M_ENABLE) && n && n != our_n) {
        /* Enabled with an N of its own: another driver reset the MAI and
           started it under mine. Theirs now (state 8, as at a start): my
           DMA stops, its registers stay as that driver set them, and only
           my slot 7 waits for Sound Off or my next start. A hotplug looks
           different - the firmware leaves the MAI off, or N 0. */
        set_state(H_BUSY_OTHER);
        dma_stop();
        mai_ours = 0;
        info_pending = 0;
        aga_diag_log("[AGA] HDMI audio: another driver took the MAI over - waiting for it to stop");
        return;
    }
    if (err) aga_diag_inc(AGA_D_HDMI_MAI_ERRS);          /* program_mai's reset clears them too */
    if (n != our_n || !(ctl & M_ENABLE)) {
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        program_mai();
    } else {
        if (err)
            wr(hw->mai_ctl, M_RUN | err);                /* cleared, and still enabled */
        if (smp_new) {
            aga_diag_inc(AGA_D_HDMI_FIXUPS);
            wr(hw->mai_smp, mai_smp);                    /* Pi 3: the HSM has a new rate */
            smp_new = 0;
        }
    }
    if (info_pending || !(rd(hw->pkt_cfg) & (1u << SLOT)) || !(rd(hw->pkt_stat) & (1u << SLOT))) {
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        infoframe();
    }
}

/* the states a wish for sound leaves by itself, a second later */
static inline int retryable(int s)
{
    return s == H_OFF || s == H_NO_LINK || s == H_SLOT_BUSY || (s == H_NO_DMA && dma_mask) ||
           s == H_BUSY_OTHER || (s == H_NO_CLOCK && pi3);
}

/* The silence goes in a piece per visit, starting where the DMA would first
   meet old sound: this core also samples the Amiga's interrupt lines, and a
   whole ring at once would hold that up. A piece is about a microsecond; the
   DMA is still 13 ms from the old sound when the fill starts. The ring a
   start on this core needs (prep_*) goes the same way. */
static uint32_t fill_at, fill_left;
static uint32_t fill_seq;                /* feed_seq when the fill began (even: no feed running) */
static int start_soon;                   /* the ring is ready: try the start at the next visit */

void aga_hdmi_service(void)
{
    static uint64_t info_next;
    int s = state;
    if (s != H_RUNNING && !ours() && !prep_left && !(wanted && retryable(s))) return;
    uint64_t now = cntvct(), hz = cntfrq();
    scan_sample(now, hz);
    if (s == H_RUNNING && fill_left) {
        /* a piece only under fill_busy, after a fenced look at the feed's
           count (see feed_seq) */
        __atomic_store_n(&fill_busy, 1, __ATOMIC_SEQ_CST);
        __asm__ volatile("dmb ish" ::: "memory");
        if (__atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST) != fill_seq) {
            fill_left = 0;                               /* the mix is back: core 3 resyncs */
        } else {
            uint32_t n = fill_left < 256 ? fill_left : 256;
            silence(fill_at, n);
            fill_at = (fill_at + n) % ring_frames;
            fill_left -= n;
        }
        __asm__ volatile("dmb ish" ::: "memory");
        __atomic_store_n(&fill_busy, 0, __ATOMIC_SEQ_CST);
        return;
    }
    if (s == H_RUNNING && !idle) {
        /* the count first, then the time: a feed that starts after this
           look changes the count, and one running now makes it odd. The
           clock is read after both, and compared signed: read before them,
           a feed that came and went in between left last_feed AFTER it, and
           the unsigned difference wrapped to "idle for ages" (found by
           review). */
        uint32_t seq = __atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST);
        uint64_t seen = __atomic_load_n(&last_feed, __ATOMIC_ACQUIRE);
        uint64_t t = cntvct();
        if (!(seq & 1) && (int64_t)(t - seen) > (int64_t)(hz * idle_ms / 1000)) {
            fill_seq = seq;
            fill_at = sil_end;
            fill_left = ring_frames;
            /* idle from now: the next feed resyncs whether the fill gets to
               the end or is cut short by it. Set only at the end, a feed
               that cut it short found the writer 40 ms behind the DMA and,
               on a Pi 4, a lead still inside its limits - and played a lap
               of old ring (found by review).

               Then the count once more, both sequentially consistent: a
               feed that began after the look above either sees this flag,
               or is seen here. Seen, it found the pause by itself (its own
               clock, or a lead the pause put out of range - see lead_high)
               and resynced; the flag would only make the feed after it
               resync again, in the middle of sound, so it goes again
               (found by review). */
            __atomic_store_n(&idle, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST) != seq) {
                __atomic_store_n(&idle, 0, __ATOMIC_SEQ_CST);
                fill_left = 0;
                return;
            }
            aga_diag_inc(AGA_D_HDMI_IDLES);
            return;
        }
    }
    if (prep_left) {
        /* with the lock: core 0 may start it meanwhile, and then the ring is its */
        if (!try_regs()) return;
        if (prep_left && state != H_RUNNING) {
            uint32_t n = prep_left < 256 ? prep_left : 256;
            silence(prep_at, n);
            prep_at += n;
            prep_left -= n;
            if (!prep_left) { ring_silent = 1; start_soon = 1; }
        } else {
            prep_left = 0;
        }
        unlock_regs();
        return;
    }
    int second = now - last_check >= hz;
    int info = s == H_RUNNING && info_pending && now >= info_next;
    if (!second && !start_soon && !info) return;
    if (!try_regs()) return;                             /* core 0 is switching it: the next visit */
    int g = guard_ok(now, hz);
    if (g < 0) { unlock_regs(); return; }                /* the look waits for the picture to be seen moving */
    if (second) last_check = now;
    if (info) info_next = now + hz / 100;                /* a pending infoframe: every 10 ms */
    start_soon = 0;
    s = state;
    if (s == H_RUNNING) {
        if (!g) {
            /* no picture on HDMI for 200 ms, or a Pi 3's HSM stopped: no
               HDMI register may be read now. My DMA stops (its registers are
               always clocked); the MAI's wait for the clock (mai_ours), and
               the retry starts it again. */
            set_state(H_NO_LINK);
            dma_stop();
            aga_diag_log("[AGA] HDMI audio: the HDMI picture stopped - waiting for it");
        } else if (second && pi3 && !hsm_known) {
            /* the Pi 3's HSM runs, but no longer from PLLD: its rate is
               unknown, so the MAI's divider may be wrong. Stopped and put
               back (the clock runs, so it may be), state 3, tried again. */
            set_state(H_NO_CLOCK);
            dma_stop();
            restore();
            aga_diag_log("[AGA] HDMI audio: the HSM left PLLD - waiting for it");
        } else if (second) {
            check();
        } else if (info_pending) {
            infoframe();
        }
    } else if (wanted && retryable(s)) {
        if (!g) {
            if (s != H_NO_CLOCK) set_state(H_NO_LINK);
        } else if (!ring_silent) {
            prep_at = 0;                                 /* silence first, then the start */
            prep_left = ring_frames;
        } else {
            start(0);
        }
    } else if (!wanted && ours() && g) {
        restore();                                       /* what a lost clock or another driver left behind */
    }
    unlock_regs();
}
