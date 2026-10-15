/* AGA-PISTORM - Paula audio through the real Paula, used as a DAC.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Everything audio is emulated (the games and AHI's paula.audio both talk to
 * the virtual Paula). The emulated mix (stereo, 8 bit, ~27.9 kHz) is streamed
 * into two small loop buffers in the REAL chip RAM (otherwise unused in AGA
 * mode) which real Paula channels 0 (left) and 1 (right) play back forever.
 * The stream is written a little ahead of the DMA read position estimated
 * from the Pi timer; when the two clocks have drifted apart the loop is
 * restarted (an inaudible hiccup every few minutes).
 *
 * All bus writes here come from the chipset core (core 3) and go through the
 * bus mutex added to Emu68's ps_protocol (see emu68-aga.patch), so they never
 * interleave with the 68k's own bus transactions on core 0.
 */
#include <stdint.h>
#include "aga/chipset.h"
#include "aga_glue.h"
#include "aga_mix.h"

extern void kprintf(const char *fmt, ...);
#include "ps_protocol.h"    /* the bus API differs per board; do not re-declare it */

#define PAULA_PERIOD     127                       /* 3546895 / 127 = 27928 Hz */
#define RING_SAMPLES     2048                      /* per channel, bytes */
/* The real Paula plays my rings on the Amiga's own crystal. The writer
   makes 27,928 samples a second of real time whatever the machine, so on a
   PAL crystal period 127 matches it; an NTSC crystal (3,579,545 Hz) played
   them 0.92% fast at 127 - 28,185 Hz, 257 samples a second more than
   written - while the read-position model below still counted 27,928, so it
   never saw the DMA lap the writer and play ring data 73 ms old (found by
   RGB2RTG). There period 128 plays 27,965 Hz, 0.13% over, which the
   one-sample nudges absorb, and the model counts that. The crystal is
   measured from the real beam (crystal_measure), not taken from the Agnus's
   PAL/NTSC strap: an A500 can run its Agnus jumpered NTSC on a PAL crystal. */
static volatile uint32_t paula_period = PAULA_PERIOD;
static volatile uint32_t sample_hz = 3546895u / PAULA_PERIOD;
/* Real chip RAM addresses for the Paula DMA rings. Paula can only fetch from
   chip RAM, so these must live there - fast RAM is not an option however
   tempting.

   The ring is OWNED, not assumed: agaboot does AllocMem(MEMF_CHIP) before
   sandbox entry and passes the address in through the control register (see
   aga_audio_set_ring). Two earlier hard-coded addresses each corrupted memory
   that belonged to somebody else:

     0x00000/0x01000  straight over the 68k exception vector table. Harmless in
       MACHINE mode, where the real chip RAM is dead space, and fatal in SANDBOX
       mode, where AmigaOS is live and its vectors are down there. Measured
       2026-09-07: rungs 3, 4 and 5 all sustain 50 Hz because none of them call
       aga_audio_stream; rung 0 calls it and the machine dies at once.

     0x0FE000/0x0FF000  the last 8 KB of the first megabyte. Survives a bare
       Workbench, which is why a 90-second rung-0 run looked clean, and dies the
       moment anything actually allocates chip RAM - `WHDLoad ... PRELOAD` takes
       what it can get, is handed that region, and then I write 8 KB of audio
       over the game's own data 437 times a second.

   No fixed address can be safe in sandbox mode, because the OS owns all of chip
   RAM. AGA_RING_FALLBACK is used only when agaboot is too old to pass one, and
   carries the second bug knowingly; it stays inside the first megabyte so it
   behaves the same on an A500 with 1 MB of chip as on a 2 MB machine. */
static volatile int running;

#define AGA_RING_FALLBACK 0x0FE000
/* Four loops, not two: left high, left low, right high, right low.
 *
 * One 8-bit Paula channel per side threw away most of the mix. A channel at
 * sample 128 and volume 44 gives 5632, which my mixer turned into 44 out of
 * 127, where real hardware produces 127*44/64 = 87 - I was 6 dB down, because
 * hardware sums two channels per side in the ANALOG domain and can swing to
 * twice one DAC's range while I had to fit that sum into a single byte. And
 * everything was then quantised to 8 bits, so a channel at volume 10 came out
 * with about THREE bits of resolution. That is what "some channels are really
 * quiet" is, and why low-volume sound effects vanished entirely.
 *
 * All four Paula channels are mine - the game's audio is fully emulated and
 * none of its AUDxx writes reach the real chip - so pair them. Channels 0 and 3
 * are both LEFT on Amiga hardware and 1 and 2 are both RIGHT, so each side gets
 * a high byte at volume 64 and a low byte at volume 1. Paula sums them itself:
 *
 *     out = hi * (64/64) + lo * (1/64) = (hi * 64 + lo) / 64
 *
 * which is a 14-bit value played through 8-bit DACs, and twice the level I
 * had. 64 times the resolution for the quiet channels that were disappearing. */
#define RING_BYTES        (4u * RING_SAMPLES)      /* lhi, llo, rhi, rlo */

/* Who writes the real chip RAM: NOT core 3 any more.

   Every group of four samples used to go out as four ps_write_32 calls
   from the chipset loop, and on a Pi 3 one such push stalled the loop for
   5 ms where it normally costs 0.15 ms (2026-09-14) - two thirds of that
   board's resyncs, and 1.2 ms of every frame on a Pi 4. So core 3 now only
   fills the local byte rings and publishes how far it got (pub_wr, released
   after the bytes); the housekeeper core, which already owns bus traffic
   for the IPL line, drains them from its own loop, one group per visit so
   the interrupt sampling never waits behind more than four bus writes.
   Restarts and the once-a-second register refresh go the same way: core 3
   asks, the housekeeper does, and sets start_tick when the DMA really
   started so the read-position model is not a guess. flush_pos, the
   ring clear and start_tick's write are the housekeeper's; core 3 reads
   start_tick only while no restart is pending. */
static uint32_t flush_pos;          /* housekeeper: first sample not yet pushed to chip RAM */
static volatile uint32_t pub_wr;    /* core 3 -> housekeeper: samples up to here are in the local rings */
static volatile int restart_req;    /* core 3 -> housekeeper: (re)start the DMA loops */
static volatile int reinit_req;     /* core 3 -> housekeeper: refresh the Paula registers */
static volatile int svc_busy;       /* housekeeper is inside the service right now */
static uint32_t clear_pos;          /* housekeeper: ring clear progress on a fresh start */
static int      svc_state;          /* 1 = clearing the rings, 2 = program Paula, 3 = running */
          /* first sample not yet pushed to chip RAM */
static uint32_t left_ring  = AGA_RING_FALLBACK;                     /* ch0: left  high */
static uint32_t right_ring = AGA_RING_FALLBACK + RING_SAMPLES;      /* ch1: right high */
static uint32_t llo_ring   = AGA_RING_FALLBACK + 2 * RING_SAMPLES;  /* ch3: left  low  */
static uint32_t rlo_ring   = AGA_RING_FALLBACK + 3 * RING_SAMPLES;  /* ch2: right low  */
static int      ring_owned;                        /* agaboot allocated it */

/* Called from the control-register trap before sandbox entry. base is a 68k
   chip RAM address of RING_BYTES that agaboot has allocated and will free. */
void aga_audio_set_ring(uint32_t base)
{
    if (base < 0x400 || (base & 3) || base + RING_BYTES > 0x200000) {
        kprintf("[AGA] audio: rejecting ring at %08x (not usable chip RAM)\n", base);
        return;
    }
    left_ring  = base;
    right_ring = base + RING_SAMPLES;
    llo_ring   = base + 2 * RING_SAMPLES;
    rlo_ring   = base + 3 * RING_SAMPLES;
    ring_owned = 1;
    running    = 0;                 /* re-program Paula against the new address */
    kprintf("[AGA] audio: ring at %08x/%08x (allocated by agaboot)\n", left_ring, right_ring);
}

/* Sandbox leave: agaboot is about to FreeMem the buffer, so stop trusting it. */
static void ring_release(void)
{
    left_ring  = AGA_RING_FALLBACK;
    right_ring = AGA_RING_FALLBACK + RING_SAMPLES;
    llo_ring   = AGA_RING_FALLBACK + 2 * RING_SAMPLES;
    rlo_ring   = AGA_RING_FALLBACK + 3 * RING_SAMPLES;
    ring_owned = 0;
}

/* How far ahead of the DMA I write. 512 samples is 18 ms on a Pi 4, whose
   frames never take longer than that. The Pi 3's worst frame with video on
   measured 31 ms (2026-09-14): Paula ate through an 18 ms lead on every
   such spike and the ring restarted, 70 times in 33 seconds - the wobble
   in the music. 1024 samples is 37 ms, which covers it, at the price of
   19 ms more latency on that board only. */
#define LEAD_PI4         512
#define LEAD_PI3         1024
static uint32_t lead_target;                   /* set at the first stream call */
#define LEAD_SAMPLES     lead_target
/* How far the lead may wander before it is worth correcting.
 *
 * It used to correct whenever the lead was more than ONE sample from target,
 * and the lead is not a measurement - it is an ESTIMATE, from a timer, of where
 * Paula has got to, against a producer that runs in bursts around each frame
 * present. So the corrector never settled: it dithered, dropping a sample and
 * then repeating one, about 92 times a second. Measured over one game:
 *   aud_drops 5652, aud_repeats 5551
 * - within 2% of each other, i.e. no net drift at all, just 92 discontinuities
 * a second chasing noise. That is what "catching other tones, some kind of lag"
 * sounds like, and it damages short sound effects worst, since a 60 ms effect
 * spans several of them.
 *
 * A deadband of 64 samples is 2.3 ms - far larger than the burst jitter, far
 * smaller than the 512-sample lead or the resync window either side of it. Real
 * crystal drift is about 0.005%, or 1.4 samples a second, so genuine drift
 * still gets corrected, roughly once every 45 seconds instead of 92 times a
 * second. */
#define LEAD_DEADBAND    64
/* Applied in the 14-bit domain, where it costs no resolution. 2 is safe against
   a measured gameplay peak of 45/127; raise it only with a fresh measurement. */
#define AUD_GAIN         2
/* The window outside which the lead is declared lost. 1024 + a frame's
   burst stays well under 1900; the ring itself is 2048 (73 ms). */
#define RESYNC_LOW       64
#define RESYNC_HIGH      1900

static uint32_t wr_pos;             /* next sample index to write (0..RING_SAMPLES-1) */
static uint64_t start_tick;         /* timer value when the DMA loop was (re)started */
/* The register refresh, once a second of real time. It was counted in calls
   of aga_audio_stream - about 437 a second, not 50 - so it ran 8.7 times a
   second, eleven bus writes in one housekeeper visit (found by RGB2RTG). */
static uint64_t refresh_at;         /* core 3: when it was last asked for */
static int      refresh_step;       /* housekeeper: how far the refresh has got */
static volatile int paula_reset;    /* housekeeper -> core 3: a reset switched the real channels off */
static int16_t  pcm[4096];          /* scratch: interleaved stereo from the core */
static uint8_t  lbuf[RING_SAMPLES], rbuf[RING_SAMPLES];     /* high bytes */
static uint8_t  llob[RING_SAMPLES], rlob[RING_SAMPLES];     /* low bytes  */
static int      have;               /* samples pending in lbuf/rbuf */

static inline uint64_t cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}
static inline uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v;
}

void aga_audio_init(const uint8_t *chipram, uint32_t chipram_size)
{
    (void)chipram; (void)chipram_size;
    kprintf("[AGA] audio: real Paula DAC, %d Hz stereo (period 127; 128 on an NTSC crystal), %d-sample loop in real chip RAM\n",
            3546895 / PAULA_PERIOD, RING_SAMPLES);
}

/* No register mirroring: the virtual Paula owns the audio registers. */
int aga_audio_reg_write(uint32_t reg, uint16_t v)
{
    (void)v;
    if (reg == DMACON) return 1;                              /* audio DMA bits stay virtual */
    return (reg >= AUD0LCH && reg < AUD0LCH + 4 * 16) ? 1 : 0;
}

/* channel n's ring, volume and register base: 0 = left high, 1 = right high,
   2 = right low, 3 = left low */
static uint32_t chan_ring(int n) { return n == 0 ? left_ring : n == 1 ? right_ring : n == 2 ? rlo_ring : llo_ring; }
static uint16_t chan_vol(int n)  { return n < 2 ? 64 : 1; }

static void program_paula(void)
{
    /* 0 = left high, 3 = left low, 1 = right high, 2 = right low.
       On Amiga hardware 0 and 3 feed LEFT, 1 and 2 feed RIGHT, so each pair is
       already summed on the right side of the output. Volume 64 against 1 makes
       the second channel the bottom six bits of a 14-bit sample. */
    ps_write_16(0xDFF096, 0x000F);                /* stop all four first */
    for (int n = 0; n < 4; n++) {
        uint32_t r = 0xDFF0A0 + 16 * (uint32_t)n, ring = chan_ring(n);
        ps_write_16(r, ring >> 16);              ps_write_16(r + 2, ring & 0xFFFF);
        ps_write_16(r + 4, RING_SAMPLES / 2);    ps_write_16(r + 6, (uint16_t)paula_period);
        ps_write_16(r + 8, chan_vol(n));
    }
    ps_write_16(0xDFF09E, 0x00FF);                /* ADKCON: clear audio modulation bits */
    ps_write_16(0xDFF096, 0x820F);                /* DMAEN | AUD0..AUD3 */
    start_tick = cntvct();
    refresh_step = 0;
}

/* The Amiga's crystal, from the real beam: how many colour clocks pass in
   8 ms of the Pi's timer, taken as 227 a line - an NTSC Agnus's 227.5 costs
   0.2%, against the 0.92% between the crystals (half way: 3,563,000 Hz).
   From the 68k's start, on the housekeeper, a look at a time, so it is
   known long before the first game and nothing ever waits for it; until
   then the rings
   play at the PAL period. A measurement is a look near the top of a frame
   (line < 64) and another 8 ms on, inside the same frame on either
   standard. Each read is bracketed by the timer and dropped when the bus
   held it up over 4 us - the bus lock is shared with the 68k's CIA cycles,
   and a wait on one end skewed a 2 ms window enough to call a US A500 PAL
   (found by review). Two measurements must agree. No answer in 2 s: the
   rings go on at the PAL period, as before, and the measuring goes on too,
   a try every quarter second - one bad window (a held bus, a reset) must
   not fix PAL for the whole boot. NTSC found while the rings already play
   at 127: the stream restarts at 128 (paula_reset).

   Not before Emu68 hands the machine to the 68k (housekeeper_enabled):
   until then core 0 drives the same GPIOs without the bus lock - the
   CPLD's JTAG check or flash, the reset pulse, the Gayle probe - and a
   read from here took their pins away in the middle of it (found by
   review). For the same reason no look while a reset is being sent
   (ignore_reset): that path takes no lock either. The check narrows that
   window to the instant the flag is raised; closing it needs the lock in
   Emu68's ps_send_reset, which the ring's own writes would want as well. */
static volatile int crystal_known;
extern volatile int housekeeper_enabled;      /* ps_*_protocol.c: Emu68 has handed over */
extern volatile int ignore_reset;             /* ps_*_protocol.c: core 0 is sending a RESET */
static void crystal_measure(void)
{
    static uint64_t t0, began, next;
    static uint32_t pos0;
    static int last = -1;                         /* the measurement before: 0 PAL, 1 NTSC */
    static int late;                              /* 2 s went by without an answer */
    uint64_t now = cntvct(), f = cntfrq();
    if (!began) began = now;
    if (!late && now - began > 2 * f) {           /* no answer yet: PAL meanwhile, as before */
        late = 1;
        aga_diag_log("[AGA] audio: the real beam could not be timed yet - PAL period 127 until it is");
    }
    if (now < next) return;                       /* between looks: no bus traffic */
    if (ignore_reset) { t0 = 0; next = now + f / 20; return; }   /* a reset on its way: in 50 ms */
    uint64_t ta = cntvct();
    uint32_t beam = ps_read_32(0xDFF004);         /* VPOSR, VHPOSR */
    uint64_t tb = cntvct();
    if (tb - ta > f / 250000) { next = tb + f / 20000; return; }   /* held up: again in 50 us */
    uint64_t t = ta + (tb - ta) / 2;
    uint32_t v = ((beam >> 16) & 7) << 8 | ((beam >> 8) & 0xFF), pos = v * 227 + (beam & 0xFF);
    if (!t0) {
        if (v < 64) { t0 = t; pos0 = pos; next = t + f / 125; }   /* the second look, 8 ms on */
        else next = t + f / 2000;                 /* the top of the next frame: look every 0.5 ms */
        return;
    }
    uint64_t hz = pos > pos0 ? (uint64_t)(pos - pos0) * f / (t - t0) : 0;
    t0 = 0;
    next = t + (late ? f / 4 : f / 2000);         /* late: the next try a quarter second on */
    if (hz < 3400000 || hz > 3700000) return;     /* a frame's end, or nonsense: again */
    int ntsc = hz > 3563000;
    if (ntsc != last) { last = ntsc; return; }    /* once more, to be sure */
    /* the period first, then the restart, then the rate (released): a core 3
       that models the read position with the new rate sees the restart too,
       so the lead it gets from the old origin only ever ends in that one
       restart - which programs the new period and clears paula_reset */
    paula_period = ntsc ? 128 : PAULA_PERIOD;
    if (ntsc && __atomic_load_n(&running, __ATOMIC_ACQUIRE))
        __atomic_store_n(&paula_reset, 1, __ATOMIC_RELEASE);   /* playing at 127: restart at 128 */
    __atomic_store_n(&sample_hz, (ntsc ? 3579545u : 3546895u) / paula_period, __ATOMIC_RELEASE);
    __atomic_store_n(&crystal_known, 1, __ATOMIC_RELEASE);
    aga_diag_log(ntsc ? "[AGA] audio: NTSC crystal - the real Paula plays the rings at period 128"
                      : "[AGA] audio: PAL crystal - the real Paula plays the rings at period 127");
}

/* The rate the writer makes samples at, per second of real time: the one
   the real Paula plays them at (sample_hz). 27,928 on a PAL crystal, as
   always; 27,965 on an NTSC one, where 27,928 left 37 samples a second for
   the one-sample repeats to fill - a hold every 27 ms for as long as sound
   played (found by review). The glue asks the core's mix for this rate,
   and HDMI's resampler starts from it. */
uint32_t aga_audio_rate(void)
{
    return __atomic_load_n(&sample_hz, __ATOMIC_ACQUIRE);
}

/* The Sound window's Stereo and Left and right (agaboot SOUND STEREO / SWAP,
   $585n and $5860/$5861 from the 68k's trap on core 0: plain stores). They
   work on the mix both outputs take - the real Paula's rings and HDMI - so
   System wide's Workbench, AHI on paula.audio included, gets them too. */
static volatile int32_t stereo_width16 = 0x10000;     /* the Amiga's own separation */
static volatile int stereo_swap;
void aga_audio_stereo(int i) { if (i >= 0 && i < 5) stereo_width16 = (int32_t)((mix_width_pc(i) << 16) / 100); }
void aga_audio_swap(int on)  { stereo_swap = on ? 1 : 0; }
void aga_audio_width_swap(int32_t *width16, int *swap) { *width16 = stereo_width16; *swap = stereo_swap; }

/* Called from the chipset loop (core 3, outside the chipset lock) with the
   samples pulled from the core since the last call. */
void aga_audio_stream(const int16_t *lr, int frames)
{
    /* width and swap first, in place: lr is aga_audio_scratch's buffer, the
       glue's own. At the Amiga's width and not swapped nothing is touched. */
    int32_t w16 = stereo_width16;
    int sw = stereo_swap;
    if (w16 != 0x10000 || sw) {
        int16_t *m = (int16_t *)lr;
        for (int i = 0; i < frames; i++) mix_stereo(&m[2 * i], &m[2 * i + 1], w16, sw);
    }
    aga_diag_set(AGA_D_AUDIO_MIX, (uint32_t)((w16 * 100 + 0x8000) >> 16) | (uint32_t)sw << 8);
    /* the same mix to HDMI first: it needs no ring in real chip RAM, so it
       plays even where the real Paula below has to stay silent. Unless HDMI
       takes Paula band-limited: then the glue hands it Paula's steps. */
    if (!aga_hdmi_paula_bl()) aga_hdmi_feed(lr, frames);
#ifdef FRF_VIRTUAL_AGA
    /* There is no physical Amiga/Paula in the standalone machine.  HDMI has
       consumed the virtual Paula mix; do not create or service the donor's
       real-chip Paula rings, and therefore never issue ps_write_* audio bus
       traffic. */
    aga_diag_inc(AGA_D_AUDIO_STREAMS);
    return;
#endif
    if (!lead_target) {
        lead_target = aga_pi3_mode() ? LEAD_PI3 : LEAD_PI4;
        /* says in the log which tolerances this board got, so a run on a
           kernel that was never loaded cannot pass for a test of it */
        aga_diag_log(lead_target == LEAD_PI3
            ? "[AGA] audio: Pi 3 - lead 1024 samples, loop catches up to two frames"
            : "[AGA] audio: lead 512 samples, loop catches up to one frame");
    }
    /* Sandbox mode: AmigaOS owns every byte of chip RAM, so without a ring
       agaboot allocated for me there is nowhere safe to put the samples.
       Silence is the correct failure - the alternative is writing 8 KB over
       whatever the running game was given, which is how this cost a day. In
       machine mode the real chip RAM is dead space and the fallback is fine. */
    if (!ring_owned && aga_sandbox_active) {
        static int moaned;
        if (!moaned) { moaned = 1; kprintf("[AGA] audio: no allocated ring in sandbox mode, staying silent\n"); }
        return;
    }
    if (!running) {
        /* start with a lead of silence so the DMA never overtakes the writer;
           the housekeeper clears the real rings and programs Paula */
        for (int i = 0; i < RING_SAMPLES; i++) { lbuf[i] = rbuf[i] = llob[i] = rlob[i] = 0; }
        wr_pos = LEAD_SAMPLES;
        start_tick = cntvct();              /* provisional; the housekeeper sets the real one */
        refresh_at = start_tick;
        __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
        clear_pos = 0;
        svc_state = 1;
        __atomic_store_n(&pub_wr, wr_pos, __ATOMIC_RELEASE);
        __atomic_store_n(&restart_req, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
    }

    /* Estimated DMA read position.
     *
     * NOT `(elapsed << 32) / ticks_per_sample_fp`: elapsed is a tick count, and
     * shifting it left by 32 overflows as soon as it passes 2^32 - about 165
     * seconds at the Pi's ~26 MHz. Multiplying by the sample rate first keeps
     * every intermediate inside 64 bits for hours. restart_req first, with
     * acquire: once it reads 0, the start_tick the housekeeper wrote before
     * clearing it is the one read here, not the origin before the restart.
     * The rate with acquire too: a new one comes with its paula_reset. */
    int pending = __atomic_load_n(&restart_req, __ATOMIC_ACQUIRE);
    uint64_t elapsed = cntvct() - start_tick;
    uint32_t rate = __atomic_load_n(&sample_hz, __ATOMIC_ACQUIRE);
    uint32_t rd_pos = (uint32_t)((elapsed * rate) / cntfrq()) % RING_SAMPLES;
    uint32_t lead = (wr_pos - rd_pos) & (RING_SAMPLES - 1);

    if (pending) {
        /* the housekeeper has not restarted the DMA yet: the model has no
           origin to regulate against, so just keep buffering */
    } else if (lead < RESYNC_LOW || lead > RESYNC_HIGH || __atomic_load_n(&paula_reset, __ATOMIC_ACQUIRE)) {
        /* Genuinely lost: restart the loop. Audible, so it must be rare. A
           reset that switched the real channels off (the housekeeper's
           refresh saw it) is restarted the same way: the hardware's read
           position went back to the start, and the model must too. */
        __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
        aga_diag_inc(AGA_D_AUDIO_RESYNCS);
        rd_pos = 0;
        wr_pos = LEAD_SAMPLES;
        svc_state = 2;
        __atomic_store_n(&restart_req, 1, __ATOMIC_RELEASE);
    } else if (lead > LEAD_SAMPLES + LEAD_DEADBAND) {
        /* Paula plays on the Amiga's crystal and I write on the Pi's, so the
         * two clocks drift apart for ever - that is not a fault to recover
         * from, it is the normal condition, and the only question is how it is
         * absorbed.
         *
         * It used to be absorbed by program_paula(), which restarts the DMA
         * loop outright. That is a hard discontinuity in the middle of the
         * music, and the drift is far larger than the design assumed: 31
         * restarts in 110 seconds, one every three and a half seconds, where
         * the header promises "an inaudible hiccup every few minutes". Beats
         * landing in the wrong place is exactly what that sounds like.
         *
         * Absorb it a single sample at a time instead. This runs about 437
         * times a second, so nudging by one sample per call corrects far more
         * drift than the clocks can produce, and one sample at 27.9 kHz is 36
         * microseconds - inaudible, where a loop restart is not. */
        wr_pos = (wr_pos - 1) & (RING_SAMPLES - 1);   /* running ahead: drop one */
        aga_diag_inc(AGA_D_AUD_NUDGE);
        aga_diag_inc(AGA_D_AUD_DROP);
    } else if (lead + LEAD_DEADBAND < LEAD_SAMPLES) {
        /* Running behind: repeat the last sample. The slot must be FILLED, not
           merely stepped over - advancing wr_pos alone leaves this slot holding
           whatever was written a full ring lap (73 ms) ago, and the packer
           below duly ships that stale byte to the DAC. A one-sample hold is
           inaudible; a 73 ms-old sample is a broadband click. */
        uint32_t prev = (wr_pos - 1) & (RING_SAMPLES - 1);
        lbuf[wr_pos] = lbuf[prev];  llob[wr_pos] = llob[prev];
        rbuf[wr_pos] = rbuf[prev];  rlob[wr_pos] = rlob[prev];
        wr_pos = (wr_pos + 1) & (RING_SAMPLES - 1);
        aga_diag_inc(AGA_D_AUD_NUDGE);
        aga_diag_inc(AGA_D_AUD_REPEAT);
    }

    aga_diag_inc(AGA_D_AUDIO_STREAMS);
    /* Convert to 8 bit, then push every COMPLETE four-sample group that has not
       been pushed yet - tracked by its own pointer.
       It used to flush on `(wr_pos & 3) == 0` alone, which assumes wr_pos only
       ever advances by one. The drift nudges above move it by +1 or -1, and
       that changes its ALIGNMENT: a group boundary can be stepped straight
       over, so those four samples are never written to chip RAM and the DAC
       plays whatever the previous lap left there. Four stale samples is a
       click; land one on a short sound effect and the effect is gone. A
       separate flush pointer cannot skip a group however wr_pos moves. */
    for (int i = 0; i < frames; i++) {
        /* 16-bit mix -> a 14-bit value played as high byte (volume 64) plus low
           byte (volume 1), which Paula sums for me on each side.
           AUD_GAIN is applied here, where there is room for it: the measured
           peak of a whole gameplay run was 45 out of 127, barely a third of the
           range, so doubling cannot clip anything that was not already loud -
           and with 14 bits the gain costs no resolution, where at 8 bits it
           would have cost a whole bit. */
        for (int ch = 0; ch < 2; ch++) {
            int32_t v14 = ((int32_t)lr[i * 2 + ch] * AUD_GAIN) >> 2;
            if (v14 >  8128) v14 =  8128;
            if (v14 < -8128) v14 = -8128;
            int32_t hi = v14 >> 6;                 /* arithmetic: -127..127 */
            int32_t lo = v14 - (hi << 6);          /* 0..63 */
            if (ch == 0) { lbuf[wr_pos] = (uint8_t)(int8_t)hi; llob[wr_pos] = (uint8_t)lo; }
            else         { rbuf[wr_pos] = (uint8_t)(int8_t)hi; rlob[wr_pos] = (uint8_t)lo; }
        }
        if (AGA_PROBES_ON) aga_diag_wave(lbuf[wr_pos], rbuf[wr_pos]);
        wr_pos = (wr_pos + 1) & (RING_SAMPLES - 1);
    }
    /* the bytes are in the local rings: tell the housekeeper how far */
    __atomic_store_n(&pub_wr, wr_pos, __ATOMIC_RELEASE);

    /* Refresh the real registers now and then, because a 68k reset also resets
       the real Paula. SET-ONLY: this must not restart the DMA.

       It used to call program_paula(), which writes DMACON=$0003 to stop the
       channels before writing $8203 to start them. That falling edge on
       AUD0EN/AUD1EN puts Paula's audio state machine back through its start
       state, which reloads the pointer from AUDxLC - the hardware read position
       snaps to ring offset 0. My own emulated Paula does exactly the same on a
       0->1 edge (aud_dmacon_changed), so the behaviour is not in doubt. The
       line after it then restored start_tick "to keep the phase", so the MODEL
       of the read position carried on from the old origin while the hardware's
       had been reset: every lead, every resync test and every nudge below was
       regulating a fiction from the first refresh onwards.

       At REINIT_INTERVAL that happened 8.66 times a second - a splice to an
       arbitrary point in the last 73 ms of ring history, continuously, under
       all the music and effects. It was never counted, unlike the 3.5-second
       drift restarts, which is why silencing those did not make the sound
       right.

       Writing $8203 when the bits are already set produces no edge and no
       restart, yet still re-enables the channels if a reset cleared them, which
       is the only reason this refresh exists. AUDxLC/AUDxLEN are latched at the
       loop wrap and AUDxPER/AUDxVOL take effect live, so rewriting them
       mid-stream is harmless.

       Since then: the refresh reads the real DMACON first. All four channels
       on, it rewrites all four channels' registers (it rewrote two, while the
       output has been four channels since the 14-bit pairs); one off, a reset
       took them, and the stream restarts as a lost one does (paula_reset) -
       set-only, two channels restarted from AUDxLC under a model that still
       ran from the old origin, and the low bytes stayed off. Once a second of
       real time (refresh_at). */
    uint64_t now = cntvct();
    if (now - refresh_at >= cntfrq()) {
        refresh_at = now;
        __atomic_store_n(&reinit_req, 1, __ATOMIC_RELEASE);
    }
}

/* The housekeeper core's share, called from its loop on every visit (about a
   million times a second): at most one group of four samples per visit, so
   the IPL sampling it exists for never waits behind more than four bus
   writes - plus, while the RGB port's HAM6 is on, four words of the frame
   the RGB port is sending (aga_rgbout.c, a classic PiStorm) - which is
   exactly why these writes were moved off core 3. A fresh start clears the
   four real rings sixteen words per visit, then programs Paula and stamps
   the moment the DMA really began. */
extern void aga_hvs_service(void);      /* aga_hvs.c: the post-hand-back HVS watch */
void aga_audio_bus_service(void)
{
    aga_hvs_service();                  /* cheap: one timer read unless a watch is armed */
    aga_hdmi_service();                 /* cheap too: one timer read unless the mix stopped */
    aga_rgbout_bus_service();           /* cheap: one load unless core 1 handed it a frame */
    /* from the hand-over to the 68k, until it knows: a look every 0.5 ms */
    if (!crystal_known && housekeeper_enabled) crystal_measure();
    if (!__atomic_load_n(&running, __ATOMIC_ACQUIRE)) return;
    svc_busy = 1;
    __asm__ volatile("dmb ish" ::: "memory");
    if (!running) { svc_busy = 0; return; }
    if (__atomic_load_n(&restart_req, __ATOMIC_ACQUIRE)) {
        if (svc_state == 1) {
            for (int i = 0; i < 4 && clear_pos < RING_SAMPLES; i++, clear_pos += 4) {
                ps_write_32(left_ring  + clear_pos, 0);
                ps_write_32(right_ring + clear_pos, 0);
                ps_write_32(llo_ring   + clear_pos, 0);
                ps_write_32(rlo_ring   + clear_pos, 0);
            }
            if (clear_pos >= RING_SAMPLES) svc_state = 2;
        }
        /* the period follows the crystal (crystal_measure). A
           reset or a new period seen before this point is covered by this
           programming, so its paula_reset goes now - left standing, core 3
           restarted a second time as soon as this one was done (found by
           review). One raised after it, on this same core, still gets its
           own restart. */
        if (svc_state == 2) {
            __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
            program_paula();
            flush_pos = LEAD_SAMPLES & ~3u;
            start_tick = cntvct();
            svc_state = 3;
            __atomic_store_n(&restart_req, 0, __ATOMIC_RELEASE);
        }
        svc_busy = 0;
        return;
    }
    if (__atomic_load_n(&reinit_req, __ATOMIC_ACQUIRE)) {
        /* SET-ONLY, no DMACON clear: a falling edge on AUDxEN restarts the
           channel from AUDxLC and the read-position model would be regulating
           a fiction from then on (see the history in git for the full story).
           A step a visit, at most four bus writes each, as the samples go. */
        if (refresh_step == 0) {
            /* DMAEN and AUD0-3 all still on? One off: a reset took the real
               channels - core 3 restarts the stream (paula_reset) */
            if ((ps_read_16(0xDFF002) & 0x020F) != 0x020F) {
                __atomic_store_n(&paula_reset, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&reinit_req, 0, __ATOMIC_RELEASE);
            } else {
                refresh_step = 1;
            }
        } else {
            int n = (refresh_step - 1) >> 1;       /* steps 1-8: two a channel */
            uint32_t r = 0xDFF0A0 + 16 * (uint32_t)n, ring = chan_ring(n);
            if (refresh_step & 1) {
                ps_write_16(r, ring >> 16);
                ps_write_16(r + 2, ring & 0xFFFF);
            } else {
                ps_write_16(r + 4, RING_SAMPLES / 2);
                ps_write_16(r + 6, (uint16_t)paula_period);
                ps_write_16(r + 8, chan_vol(n));
            }
            if (++refresh_step > 8) {
                refresh_step = 0;
                __atomic_store_n(&reinit_req, 0, __ATOMIC_RELEASE);
            }
        }
        svc_busy = 0;
        return;
    }
    /* Distance forward from flush_pos, modulo the ring. A -1 nudge on core 3
       can put the target BEHIND me; half a ring of "distance" means exactly
       that, and the answer is to give up the gap, not to walk the whole ring
       writing stale samples. */
    uint32_t target = __atomic_load_n(&pub_wr, __ATOMIC_ACQUIRE) & ~3u;
    uint32_t dist = (target - flush_pos) & (RING_SAMPLES - 1);
    if (dist > RING_SAMPLES / 2) { flush_pos = target; dist = 0; }
    if (dist >= 4) {
        uint32_t p = flush_pos;
        uint32_t lw  = ((uint32_t)lbuf[p] << 24) | ((uint32_t)lbuf[p + 1] << 16) | ((uint32_t)lbuf[p + 2] << 8) | lbuf[p + 3];
        uint32_t rw  = ((uint32_t)rbuf[p] << 24) | ((uint32_t)rbuf[p + 1] << 16) | ((uint32_t)rbuf[p + 2] << 8) | rbuf[p + 3];
        uint32_t llw = ((uint32_t)llob[p] << 24) | ((uint32_t)llob[p + 1] << 16) | ((uint32_t)llob[p + 2] << 8) | llob[p + 3];
        uint32_t rlw = ((uint32_t)rlob[p] << 24) | ((uint32_t)rlob[p + 1] << 16) | ((uint32_t)rlob[p + 2] << 8) | rlob[p + 3];
        ps_write_32(left_ring  + p, lw);
        ps_write_32(right_ring + p, rw);
        ps_write_32(llo_ring   + p, llw);
        ps_write_32(rlo_ring   + p, rlw);
        flush_pos = (flush_pos + 4) & (RING_SAMPLES - 1);
    }
    svc_busy = 0;
}

int16_t *aga_audio_scratch(int *max_frames)
{
    *max_frames = (int)(sizeof pcm / sizeof pcm[0]) / 2;
    return pcm;
}

/* Sandbox leave: stop the DAC loops and forget the stream state. */
void aga_audio_stop(void)
{
    int was = running;
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    /* let the housekeeper finish its group. An acquire load: after the
       store-release of running above it cannot be hoisted before it (a
       plain load could, and a program_paula then ran after the $000F
       below and left the DMA on - found by review) */
    while (__atomic_load_n(&svc_busy, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    __atomic_store_n(&restart_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&reinit_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
    refresh_step = 0;                            /* the next session's refresh starts afresh */
    if (was) ps_write_16(0xDFF096, 0x000F);      /* clear AUD0..AUD3 */
    have = 0;
    ring_release();
}
