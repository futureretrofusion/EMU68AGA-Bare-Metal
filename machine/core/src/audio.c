/* AGA-PISTORM - Paula audio.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Each channel is advanced once per scanline at colour-clock precision; the
 * output is box-filtered into a stereo ring buffer at `audio_rate` Hz.
 *
 * My own simplified Paula; no UAE/WinUAE code or data is copied. Four rules
 * come from WinUAE audio.cpp (audio_state_channel2), rewritten here: a sample
 * the CPU starts needs its interrupt not pending, and an ignored AUDxDAT write
 * is dropped; the first DMA word is requested at once and raises its interrupt
 * when it arrives; with DMA switched off a channel still waiting for its first
 * word stops while a playing one plays out; with DMA off the interrupt is
 * raised for each word and the channel stops only if the previous one is
 * still pending. The field names dat_written and intreq2 are WinUAE's.
 *   WinUAE audio.cpp: Copyright 1995, 1996, 1997 Bernd Schmidt,
 *     Copyright 1996 Marcus Sundberg, Copyright 1996 Manfred Thole,
 *     Copyright 2006 Toni Wilen
 * UAE and WinUAE are GPL, like this file.
 */
#include "internal.h"

#define AUD_RING 32768      /* stereo frames */
static int16_t ring[AUD_RING * 2];
static uint32_t ring_wr, ring_rd;

/* box filter accumulators (cck-weighted sums) */
static int64_t acc_l, acc_r;
static int32_t acc_cck;          /* cck accumulated in current output sample, fixed 16.16 */
static int32_t cck_per_sample;   /* 16.16 */

/* Paula's steps for HDMI's band-limited Paula (aga_audio_steps). The colour
   clock runs on from boot - a reset does not set it back - so its times only
   ever grow. */
static uint64_t aud_t;                           /* colour clocks mixed since boot */
static aga_step_t steps[AGA_STEP_RING];
static uint32_t steps_wr, steps_rd;
static int steps_on;
static int32_t step_level[4];                    /* the level last logged; NOT_LOGGED: none yet */
#define NOT_LOGGED 0x7FFFFFFF

static void step_log(int n, uint64_t t, int32_t level)
{
    if (step_level[n] == level) return;
    if (steps_wr - steps_rd >= AGA_STEP_RING) return;   /* full: tried again at its next look */
    step_level[n] = level;
    aga_step_t *s = &steps[steps_wr & (AGA_STEP_RING - 1)];
    s->t = t;
    s->level = (int16_t)level;
    s->ch = (uint8_t)n;
    steps_wr++;
}

void aga_audio_steps_on(aga_t *a, int on)
{
    (void)a;
    steps_on = on;
    steps_rd = steps_wr;
    for (int n = 0; n < 4; n++) step_level[n] = NOT_LOGGED;
}

int aga_audio_steps(aga_t *a, aga_step_t *out, int max, uint64_t *now)
{
    (void)a;
    int n = 0;
    while (n < max && steps_rd != steps_wr) out[n++] = steps[steps_rd++ & (AGA_STEP_RING - 1)];
    *now = aud_t;
    return n;
}

void aud_reset(aga_t *a)
{
    memset(a->aud, 0, sizeof a->aud);
    a->audio_rate = 48000;
    a->aud_mask = 15;
    ring_wr = ring_rd = 0;
    acc_l = acc_r = 0;
    acc_cck = 0;
    aud_clock(a);
}

/* colour clocks per output sample, from the standard in force: NTSC's colour
   clock is 0.92% faster, and the samples are made from emulated time */
void aud_clock(aga_t *a)
{
    double cck_hz = a->ntsc ? 3579545.0 : 3546895.0;
    cck_per_sample = (int32_t)(cck_hz * 65536.0 / a->audio_rate);
}

static void set_rate(aga_t *a, int rate)
{
    if (rate <= 0) rate = 48000;
    if (a->audio_rate != rate) {
        a->audio_rate = rate;
        aud_clock(a);
    }
}

void aud_write(aga_t *a, uint32_t reg, uint16_t v)
{
    int n = (reg - AUD0LCH) >> 4;
    audio_channel_t *c = &a->aud[n];
    /* the glue may mirror audio registers to a real Paula */
    if (a->hooks.ext_write) a->hooks.ext_write(a->hooks.user, reg, v);
    switch (reg & 0xE) {
    case 0x0: c->lc = (c->lc & 0xFFFF) | ((uint32_t)(v & 0x01FF) << 16); break;
    case 0x2: c->lc = (c->lc & 0xFFFF0000u) | (v & 0xFFFE); break;
    case 0x4: c->len = v; break;
    case 0x6: c->per = v; break;
    case 0x8:
        c->vol = v & 0x7F; if (c->vol > 64) c->vol = 64;
        if ((uint32_t)c->vol > a->aud_volmax[n]) a->aud_volmax[n] = (uint32_t)c->vol;
        a->aud_volwr[n]++;
        if (c->vol) a->aud_volnz[n]++;
        break;
    case 0xA:
        c->dat = v;
        c->dat_written = 1;
        /* only while the channel's last request has been answered: WinUAE
           audio_state_channel2, state 0 - `dat_written && !isirq(nr)` - leaves
           an idle channel idle when its interrupt is still pending */
        if (!c->dmaen && c->state == 0 && !(a->intreq & (INTF_AUD0 << n))) {
            /* manual mode: start output, request next word */
            c->state = 2;
            c->percnt = c->per;
            c->cur_sample = (int8_t)(v >> 8);
            c->dat_written = 0;
            c->intreq2 = 1;
            aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[0]++;
        } else if (!c->dmaen && c->state == 0) {
            c->dat_written = 0;          /* ignored, and forgotten - as WinUAE does */
        }
        break;
    default: break;
    }
}

void aud_dmacon_changed(aga_t *a, uint16_t old, uint16_t new_)
{
    for (int n = 0; n < 4; n++) {
        audio_channel_t *c = &a->aud[n];
        int was = (old & DMAF_DMAEN) && (old & (DMAF_AUD0EN << n));
        int now = (new_ & DMAF_DMAEN) && (new_ & (DMAF_AUD0EN << n));
        if (!was && now) {
            {   /* what this channel was handed at the moment it was started */
                uint32_t *t = a->aud_trig[a->aud_trig_head & 31];
                t[0] = (uint32_t)n;
                t[1] = c->lc;
                t[2] = c->len;
                t[3] = c->per;
                t[4] = (uint32_t)c->vol;
                /* Not one word - the loudest sample in the first 256 words.
                   A ProTracker sample begins with a zero word by convention, so
                   the first word says nothing. This says whether the DATA IS
                   THERE: zero means my chip RAM copy has no sound at that
                   address, which would be my fault; large means the data is
                   fine and the silence is the volume's doing. */
                {
                    uint32_t words = c->len && c->len < 256 ? c->len : 256;   /* LEN 0: 65,536 */
                    int peak = 0;
                    for (uint32_t k = 0; k < words; k++) {
                        uint16_t wv = chip_rd16(a, c->lc + k * 2);
                        int hi = (int8_t)(wv >> 8), lo = (int8_t)(wv & 0xFF);
                        if (hi < 0) hi = -hi;
                        if (lo < 0) lo = -lo;
                        if (hi > peak) peak = hi;
                        if (lo > peak) peak = lo;
                    }
                    t[5] = (uint32_t)peak;
                }
                a->aud_trig_head++;
            }
            a->aud_on[n]++;
            c->dmaen = 1;
            c->pt = c->lc;
            c->lencnt = c->len;
            c->state = 1;        /* DMA on: fetch first word */
            /* at the channel's next DMA slot, not a whole period later: WinUAE
               state 0 -> 1 asks for DMA at once and raises the interrupt as the
               word arrives (setirq 10), within the line. Banshee starts a sample,
               enables the channel's interrupt, sets the volume on the first one
               and treats the SECOND as the end of the sample. A channel it had
               just stopped leaves a play-out request pending, so the first
               interrupt it takes is that one - and with my first-word request a
               full period behind (550-906 colour clocks), it arrived after the
               handler had cleared the bit and ended the effect one word in: the
               explosions and bonuses on channel 1, 26 of 28 cut after ~650 cck.
               Within the line it merges with the pending bit, as on the A1200. */
            c->percnt = 1;
            c->dat_written = 0;
        } else if (was && !now) {
            /* The channel does not stop here. Paula plays out the word it
               has, then asks for another (see the end of chan_run). Only a
               channel that never fetched anything has nothing to finish. */
            a->aud_dmaoff[n]++;
            c->dmaen = 0;
            if (c->state == 1) {
                c->state = 0;
                c->cur_sample = 0;
            }
        }
    }
}

void aud_dma_slot(aga_t *a, int hpos)
{
    (void)a; (void)hpos;
    /* DMA fetches are performed on demand in aud_line_tick; this slot exists for future cycle accuracy. */
}

static void chan_next_word(aga_t *a, int n, audio_channel_t *c)
{
    if (c->dmaen) {
        /* audio DMA is done per line rather than per slot, so it is not traced */
        a->aud_words[n]++;
        c->dat = chip_rd16(a, c->pt);
        c->pt += 2;
        /* Paula's length counter counts down through 0: an AUDxLEN of 0 is
           65,536 words, a 128 KB sample (WinUAE: wlen = (wlen - 1) & 0xffff
           unless it is 1). Mine stopped at anything below 2, so LEN 0 looped
           a single word and raised AUDx at every one (found by RGB2RTG). */
        if (c->lencnt != 1) {
            c->lencnt = (uint16_t)(c->lencnt - 1);
        } else {
            /* The sample ran out. Real Paula always loops - a game stops the
               repeat by pointing AUDxLC/AUDxLEN at a two-word silent buffer,
               and it does that FROM THIS INTERRUPT. If the interrupt does not
               reach the game the effect loops for ever, the channel is
               permanently busy, and later effects have nowhere to play. */
            a->aud_wrap[n]++;
            a->aud_irq[n]++;
            c->lencnt = c->len;
            c->pt = c->lc;
            if ((a->aud_wrap[n] & 511) == 0) {
                /* Is the buffer I keep replaying actually SILENT? A game turns
                   a one-shot into silence by pointing the channel at a short
                   quiet loop; if what I loop has signal in it, I am looping a
                   piece of the effect instead, and that is the drone. */
                uint32_t *w = a->aud_wrapinfo[a->aud_wrapinfo_head & 31];
                uint32_t words = c->len && c->len < 64 ? c->len : 64;   /* LEN 0: 65,536 */
                int peak = 0;
                for (uint32_t k = 0; k < words; k++) {
                    uint16_t wv = chip_rd16(a, c->lc + k * 2);
                    int hi = (int8_t)(wv >> 8), lo = (int8_t)(wv & 0xFF);
                    if (hi < 0) hi = -hi;
                    if (lo < 0) lo = -lo;
                    if (hi > peak) peak = hi;
                    if (lo > peak) peak = lo;
                }
                w[0] = (uint32_t)n; w[1] = c->lc; w[2] = c->len; w[3] = (uint32_t)peak;
                a->aud_wrapinfo_head++;
            }
            aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[1]++;
        }
        c->dat_written = 1;
    }
}

/* Advance one channel by `cck` colour clocks from colour clock t0,
   accumulating output - and, while HDMI's band-limited Paula asks for them,
   logging the level it plays at each colour clock it changes at. */
static void chan_run(aga_t *a, int n, audio_channel_t *c, int cck, int64_t *l, int64_t *r, uint64_t t0)
{
    int vol = c->vol;
    int left = (n == 0 || n == 3);
    int t = 0;
    /* A channel that modulates the next one (ADKCON USExVy / USExPy: bits n
       and n + 4; channel 3's modulate nothing) is not heard - its words are
       the next channel's volume or period, and Paula does not output it
       (WinUAE audio_update_adkmasks). Mine mixed the bastards in as 8-bit
       samples: a loud buzz on HDMI and on the real Paula (found by RGB2RTG).
       The modulation itself is still not emulated - the next channel plays at
       its own volume and period. */
    int heard = !((a->adkcon >> n) & 0x11);
    int mixed = heard && (a->aud_mask & (1 << n));
    if (c->state == 0) { if (steps_on) step_log(n, t0, 0); return; }
    while (t < cck) {
        int step = c->percnt;
        if (step <= 0) step = 1;
        if (step > cck - t) step = cck - t;
        if (steps_on) step_log(n, t0 + (uint64_t)t, mixed ? (int32_t)c->cur_sample * vol : 0);
        int32_t s = heard ? (int32_t)c->cur_sample * vol * step : 0;
        /* Accumulate what this channel actually CONTRIBUTES, volume included.
           The previous counter tested cur_sample alone, and a channel left
           looping a sample at AUDxVOL 0 - the standard way to silence one
           without stopping its DMA - counted every silent sample as sound. That
           is how channel 2 came to look as busy as the music channels while
           being inaudible on its own. */
        a->aud_nz[n] += (uint32_t)((s < 0 ? -s : s) >> 12);
        if (c->cur_sample && vol && heard) a->aud_audible[n]++;
        /* Masked channels still RUN - their pointers, interrupts and timing are
           what the game depends on - they simply do not reach the mix. */
        if (a->aud_mask & (1 << n)) { if (left) *l += s; else *r += s; }
        t += step;
        c->percnt -= step;
        if (c->percnt <= 0) {
            int per = c->per < 4 ? 4 : c->per;
            c->percnt += per;
            /* next sample */
            if (c->state == 1) {           /* need first word */
                chan_next_word(a, n, c);
                if (!c->dat_written) {
                    c->state = 0;
                    if (steps_on) step_log(n, t0 + (uint64_t)t, 0);
                    return;
                }
                c->dat_written = 0;
                c->cur_sample = (int8_t)(c->dat >> 8);
                c->state = 2;
                /* Paula raises the channel's interrupt request as soon as the
                   first word of a new DMA start is in - "your pointer and length
                   are taken, queue the next ones" (WinUAE: state 1 -> 5,
                   setirq). I only raised it when a sample ran out and looped.
                   Banshee polls INTREQR for exactly this bit and turns the
                   volume up only once it sees it. It retriggers its gun twice a
                   second with a 0.84 s sample that therefore never reaches its
                   end, so the bit never came: 293 shots started at volume 0 and
                   fucking stayed there, and the right speaker was silent 84% of
                   the time while a real A1200 plays four sounds a second. */
                aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[2]++;
            } else if (c->state == 2) {    /* high byte played, play low byte */
                c->cur_sample = (int8_t)(c->dat & 0xFF);
                c->state = 3;
                if (c->dmaen) chan_next_word(a, n, c);   /* prefetch next word */
            } else {                       /* low byte played: load next word */
                if (c->dmaen) {
                    if (!c->dat_written) chan_next_word(a, n, c);
                    c->dat_written = 0;
                    c->cur_sample = (int8_t)(c->dat >> 8);
                    c->state = 2;
                } else if (c->dat_written) {
                    c->dat_written = 0;
                    c->cur_sample = (int8_t)(c->dat >> 8);
                    c->state = 2;
                    aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[3]++;
                } else if (!(a->intreq & (INTF_AUD0 << n))) {
                    /* DMA is off and nothing was written to AUDxDAT: real
                       Paula asks for the next word anyway - it raises AUDxIR
                       and holds its output - and goes idle only if that
                       request is still unanswered a period later (WinUAE,
                       audio_state_channel2 state 3, setirq 32). I used to
                       stop dead the moment DMA went off. Worms: The
                       Director's Cut stops a channel from inside its CIA-B
                       music interrupt and polls INTREQR for exactly this
                       bit - 2.7 million reads, then the whole game wedged at
                       its menu with the music stuck on one note. */
                    /* Every word, for as long as the program acknowledges
                       each request - exactly WinUAE: at the end of state 3
                       with DMA off it goes idle only when the previous
                       request is still pending (irqcheck = isirq), and
                       otherwise raises again (setirq 32) and plays on. */
                    aga_intreq_set(a, (uint16_t)(INTF_AUD0 << n)); a->dbg_audirq[4]++;
                    c->cur_sample = (int8_t)(c->dat >> 8);
                    c->state = 2;
                } else {
                    c->state = 0;
                    c->cur_sample = 0;
                    if (steps_on) step_log(n, t0 + (uint64_t)t, 0);
                    return;
                }
            }
        }
    }
}

/* Mix a scanline, in chunks that end on output-sample boundaries.
 *
 * This used to advance all four channels across the WHOLE line and then treat
 * the result as one constant level:
 *
 *     for (n = 0..3) chan_run(a, n, &a->aud[n], cck, &l, &r);
 *     lpc = l / cck;                  // per-cck average for the entire line
 *
 * which samples the mix at the LINE rate - 3546895/227, about 15.6 kHz - and
 * then holds it across the 1.79 output samples that fit in a line. That halves
 * the usable bandwidth to ~7.8 kHz and folds everything above it back down as
 * aliasing, even though an output sample is only 127 colour clocks and the
 * resolution was there all along. Music mostly survives it; short bright
 * effects turn to mush or vanish into the mix, which is what a game with music
 * and effects on four channels actually sounds like.
 *
 * Chunking on output-sample boundaries costs one extra loop and gives the real
 * 13.96 kHz of a 27.9 kHz stream. */
void aud_line_tick(aga_t *a)
{
    int cck_left = a->maxhpos;

    while (cck_left > 0) {
        /* whole colour clocks until this output sample closes, at least one */
        int32_t need = cck_per_sample - acc_cck;
        int chunk = (int)((need + 0xFFFF) >> 16);
        if (chunk < 1) chunk = 1;
        if (chunk > cck_left) chunk = cck_left;

        int64_t l = 0, r = 0;
        for (int n = 0; n < 4; n++) chan_run(a, n, &a->aud[n], chunk, &l, &r, aud_t);
        aud_t += (uint64_t)chunk;

        /* l and r are sum(sample*vol*cck); acc is the same in 16.16 cck units */
        acc_l += l << 16;
        acc_r += r << 16;
        acc_cck += chunk << 16;
        cck_left -= chunk;

        if (acc_cck >= cck_per_sample) {
            int64_t sl = acc_l / cck_per_sample;   /* sample*vol, <=8128 per channel */
            int64_t sr = acc_r / cck_per_sample;
            sl *= 2; sr *= 2;                      /* two channels per side: max 32512 */
            if (sl > 32767) sl = 32767;
            if (sl < -32768) sl = -32768;
            if (sr > 32767) sr = 32767;
            if (sr < -32768) sr = -32768;
            uint32_t idx = ring_wr & (AUD_RING - 1);
            ring[idx * 2] = (int16_t)sl;
            ring[idx * 2 + 1] = (int16_t)sr;
            ring_wr++;
            acc_l = acc_r = 0;
            acc_cck -= cck_per_sample;   /* keep the remainder: do not drift */
        }
    }
}

int aga_audio_pull(aga_t *a, int16_t *lr, int frames, int rate)
{
    set_rate(a, rate);
    int n = 0;
    while (n < frames && ring_rd != ring_wr) {
        uint32_t idx = ring_rd & (AUD_RING - 1);
        lr[n * 2] = ring[idx * 2];
        lr[n * 2 + 1] = ring[idx * 2 + 1];
        ring_rd++;
        n++;
    }
    return n;
}

/* Per-channel diagnostics: how often each channel's DMA was switched on, and
   how many sample words it fetched. The glue publishes these into the counter
   block once a frame. */
void aga_audio_set_mask(aga_t *a, int mask) { a->aud_mask = mask & 15; }

/* The last 32 channel starts, oldest first: chan, lc, len, per, vol, word0. */
void aga_audio_triggers(const aga_t *a, uint32_t *out, int *count)
{
    uint32_t n = a->aud_trig_head < 32 ? a->aud_trig_head : 32;
    uint32_t start = a->aud_trig_head < 32 ? 0 : a->aud_trig_head;
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 6; k++)
            out[i * 6 + k] = a->aud_trig[(start + i) & 31][k];
    *count = (int)n;
}

/* The volume each channel is currently set to - the one thing that turns a
   busy-looking channel into a silent one. */
void aga_audio_vols(const aga_t *a, uint32_t *vol, uint32_t *volmax, uint32_t *audible)
{
    for (int n = 0; n < 4; n++) {
        vol[n] = (uint32_t)a->aud[n].vol;
        volmax[n] = a->aud_volmax[n];
        audible[n] = a->aud_audible[n];
    }
}

/* Does the game ever ASK this channel to make a sound, and does it ever switch
   it off? Three channels play continuous music through a section that should
   have none, so the question is whether the writes that would stop them are
   arriving at all. */
void aga_audio_volstats(const aga_t *a, uint32_t *wr, uint32_t *nz, uint32_t *dmaoff)
{
    for (int n = 0; n < 4; n++) {
        wr[n] = a->aud_volwr[n]; nz[n] = a->aud_volnz[n]; dmaoff[n] = a->aud_dmaoff[n];
    }
}

/* How often each sample looped, and how often I asked for its interrupt.
   Compare aud_irq against irq_l4: raises that never become exceptions are the
   reason a one-shot never gets turned into a silent loop. */
void aga_audio_wrapstats(const aga_t *a, uint32_t *wrap, uint32_t *irq)
{
    for (int n = 0; n < 4; n++) { wrap[n] = a->aud_wrap[n]; irq[n] = a->aud_irq[n]; }
}

/* The last 32 sampled loop-backs: chan, lc, len, peak of what is looping. */
void aga_audio_wrapinfo(const aga_t *a, uint32_t *out, int *count)
{
    uint32_t n = a->aud_wrapinfo_head < 32 ? a->aud_wrapinfo_head : 32;
    uint32_t start = a->aud_wrapinfo_head < 32 ? 0 : a->aud_wrapinfo_head;
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            out[i * 4 + k] = a->aud_wrapinfo[(start + i) & 31][k];
    *count = (int)n;
}

void aga_audio_stats(const aga_t *a, uint32_t *on, uint32_t *words, uint32_t *nz)
{
    for (int n = 0; n < 4; n++) {
        on[n] = a->aud_on[n]; words[n] = a->aud_words[n]; nz[n] = a->aud_nz[n];
    }
}
