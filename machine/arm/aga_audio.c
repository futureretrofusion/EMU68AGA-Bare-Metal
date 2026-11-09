/* AGA-STORM — Paula audio kroz pravu Paulu, korištenu kao DAC.
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
 * Sva bus pisanja ovdje dolaze iz chipset jezgre (jezgra 3) i prolaze kroz bus
 * mutex dodan u Emu68 ps_protocol (vidi emu68-aga.patch), pa se nikad ne
 * isprepliću sa 68k-ovim vlastitim bus transakcijama na jezgri 0.
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
       over the game 437 times a second. Fucking brilliant.

   No fixed address can be safe in sandbox mode, because the OS owns all of chip
   RAM. AGA_RING_FALLBACK is used only when agaboot is too old to pass one, and
   carries the second bug knowingly; it stays inside the first megabyte so it
   behaves the same on an A500 with 1 MB of chip as on a 2 MB machine. */
static volatile int running;

#define AGA_RING_FALLBACK 0x0FE000
/* Četiri loopa, ne dva: lijevo visoko, lijevo nisko, desno visoko, desno
 * nisko.
 *
 * One 8-bit Paula channel per side threw away most of the mix. A channel at
 * sample 128 and volume 44 gives 5632, which my mixer turned into 44 out of
 * 127, where real hardware produces 127*44/64 = 87 - I was 6 dB down and it
 * sounded like shit, because hardware sums two channels per side in the
 * ANALOG domain and can swing to twice one DAC's range while I had to fit
 * that sum into a single damn byte. And
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
 * što je 14-bitna vrijednost reproducirana kroz 8-bitne DAC-ove, i dvostruka
 * razina koju sam imao. 64 puta veća rezolucija za tihe kanale koji su
 * nestajali. */
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

/* Poziva se iz control-register trapa prije ulaska u sandbox. base je 68k
   chip RAM adresa od RING_BYTES koju je agaboot alocirao i koju će osloboditi. */
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
    running    = 0;                 /* ponovno programiraj Paulu na novu adresu */
    kprintf("[AGA] audio: ring at %08x/%08x (allocated by agaboot)\n", left_ring, right_ring);
}

/* Izlazak iz sandboxa: agaboot će upravo FreeMem-ati buffer, pa mu prestani vjerovati. */
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
static uint32_t lead_target;                   /* postavlja se pri prvom stream pozivu */
#define LEAD_SAMPLES     lead_target
/* Koliko lead smije lutati prije nego ga vrijedi ispravljati.
 *
 * Nekada se ispravljao kad god je lead bio više od JEDNOG uzorka od cilja, a
 * lead nije mjerenje - to je PROCJENA, iz timera, gdje je Paula stigla, prema
 * produceru koji radi u burstovima oko svakog frame presenta. Zato se korektor
 * nikad nije smirio: ditherirao je, ispuštajući uzorak pa ponavljajući jedan,
 * oko 92 puta u sekundi. Izmjereno tijekom jedne igre:
 *   aud_drops 5652, aud_repeats 5551
 * - within 2% of each other, i.e. no net drift at all, just 92 discontinuities
 * a second chasing noise. That is what "catching other tones, some kind of lag"
 * sounds like, and it damages short sound effects worst, since a 60 ms effect
 * spans several of them.
 *
 * Mrtva zona od 64 uzorka je 2.3 ms - mnogo veća od burst jittera, mnogo manja
 * od leada od 512 uzoraka ili resync prozora s obje strane. Stvarni drift
 * kristala je oko 0.005%, ili 1.4 uzorka u sekundi, pa se pravi drift još
 * ispravlja, otprilike jednom svakih 45 sekundi umjesto 92 puta u sekundi. */
#define LEAD_DEADBAND    64
/* Primjenjuje se u 14-bitnoj domeni, gdje ne košta rezoluciju. 2 je sigurno
   prema izmjerenom gameplay peaku od 45/127; povećaj ga samo uz novo mjerenje. */
#define AUD_GAIN         2
/* Prozor izvan kojeg se lead proglašava izgubljenim. 1024 + burst jednog
   framea ostaje dobro ispod 1900; sam ring je 2048 (73 ms). */
#define RESYNC_LOW       64
#define RESYNC_HIGH      1900

static uint32_t wr_pos;             /* indeks sljedećeg uzorka za pisanje (0..RING_SAMPLES-1) */
static uint64_t start_tick;         /* vrijednost timera kad je DMA loop (ponovno) pokrenut */
/* Osvježavanje registara, jednom u sekundi stvarnog vremena. Brojilo se u
   pozivima aga_audio_stream - oko 437 u sekundi, ne 50 - pa je radilo 8.7 puta
   u sekundi, jedanaest bus pisanja u jednom housekeeper posjetu (otkrio
   RGB2RTG). */
static uint64_t refresh_at;         /* jezgra 3: kad je zadnji put zatraženo */
static int      refresh_step;       /* housekeeper: dokle je osvježavanje stiglo */
static volatile int paula_reset;    /* housekeeper -> jezgra 3: reset je isključio prave kanale */
static int16_t  pcm[4096];          /* scratch: interleaved stereo iz jezgre */
static uint8_t  lbuf[RING_SAMPLES], rbuf[RING_SAMPLES];     /* visoki bajtovi */
static uint8_t  llob[RING_SAMPLES], rlob[RING_SAMPLES];     /* niski bajtovi */
static int      have;               /* uzorci na čekanju u lbuf/rbuf */

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

/* Nema mirroringa registara: virtualna Paula posjeduje audio registre. */
int aga_audio_reg_write(uint32_t reg, uint16_t v)
{
    (void)v;
    if (reg == DMACON) return 1;                              /* audio DMA bitovi ostaju virtualni */
    return (reg >= AUD0LCH && reg < AUD0LCH + 4 * 16) ? 1 : 0;
}

/* ring, volumen i base registra kanala n: 0 = lijevo visoko, 1 = desno visoko,
   2 = desno nisko, 3 = lijevo nisko */
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
    ps_write_16(0xDFF09E, 0x00FF);                /* ADKCON: očisti audio modulacijske bitove */
    ps_write_16(0xDFF096, 0x820F);                /* DMAEN | AUD0..AUD3 */
    start_tick = cntvct();
    refresh_step = 0;
}

/* Amigin kristal, sa stvarnog beam-a: koliko colour clockova prođe u 8 ms Pi
   timera, uzetih kao 227 po liniji - NTSC Agnusovih 227.5 košta 0.2%, prema
   0.92% između kristala (na pola puta: 3,563,000 Hz). Od 68k starta, na
   housekeeperu, po jedan pogled, pa je poznat davno prije prve igre i ništa ga
   nikad ne čeka; do tada ringovi reproduciraju na PAL periodi. Mjerenje je
   pogled blizu vrha framea (linija < 64) i još jedan 8 ms dalje, unutar istog
   framea na oba standarda. Svako čitanje omeđeno je timerom i odbacuje se kad
   ga je bus zadržao preko 4 us - bus lock se dijeli sa 68k CIA ciklusima, a
   čekanje na jednom kraju iskrivilo je prozor od 2 ms dovoljno da se US A500
   proglasi PAL-om (otkrio review). Dva mjerenja moraju se slagati. Nema
   odgovora u 2 s: ringovi nastavljaju na PAL periodi, kao prije, a mjerenje se
   također nastavlja, pokušaj svakih četvrt sekunde - jedan loš prozor (zadržan
   bus, reset) ne smije fiksirati PAL za cijeli boot, koliko god to bilo
   primamljivo. NTSC otkriven dok ringovi već reproduciraju na 127: stream se
   restarta na 128 (paula_reset).

   Ne prije nego Emu68 preda stroj 68k-u (housekeeper_enabled): do tada jezgra
   0 vozi iste GPIO-e bez bus locka - CPLD-ov JTAG check ili flash, reset
   pulse, Gayle probe - a čitanje odavde oduzelo im je pinove usred toga
   (otkrio review). Iz istog razloga nema pogleda dok se reset šalje
   (ignore_reset): taj put također ne uzima lock. Provjera sužava taj prozor na
   trenutak kad se flag podigne; zatvaranje zahtijeva lock u Emu68
   ps_send_reset, koji bi i sami ringovi upisi htjeli. */
static volatile int crystal_known;
extern volatile int housekeeper_enabled;      /* ps_*_protocol.c: Emu68 je predao */
extern volatile int ignore_reset;             /* ps_*_protocol.c: jezgra 0 šalje RESET */
static void crystal_measure(void)
{
    static uint64_t t0, began, next;
    static uint32_t pos0;
    static int last = -1;                         /* prethodno mjerenje: 0 PAL, 1 NTSC */
    static int late;                              /* prošle 2 s bez odgovora */
    uint64_t now = cntvct(), f = cntfrq();
    if (!began) began = now;
    if (!late && now - began > 2 * f) {           /* još nema odgovora: zasad PAL, kao prije */
        late = 1;
        aga_diag_log("[AGA] audio: the real beam could not be timed yet - PAL period 127 until it is");
    }
    if (now < next) return;                       /* između pogleda: bez bus prometa */
    if (ignore_reset) { t0 = 0; next = now + f / 20; return; }   /* reset je na putu: za 50 ms */
    uint64_t ta = cntvct();
    uint32_t beam = ps_read_32(0xDFF004);         /* VPOSR, VHPOSR */
    uint64_t tb = cntvct();
    if (tb - ta > f / 250000) { next = tb + f / 20000; return; }   /* zadržano: opet za 50 us */
    uint64_t t = ta + (tb - ta) / 2;
    uint32_t v = ((beam >> 16) & 7) << 8 | ((beam >> 8) & 0xFF), pos = v * 227 + (beam & 0xFF);
    if (!t0) {
        if (v < 64) { t0 = t; pos0 = pos; next = t + f / 125; }   /* drugi pogled, 8 ms dalje */
        else next = t + f / 2000;                 /* vrh sljedećeg framea: pogled svakih 0.5 ms */
        return;
    }
    uint64_t hz = pos > pos0 ? (uint64_t)(pos - pos0) * f / (t - t0) : 0;
    t0 = 0;
    next = t + (late ? f / 4 : f / 2000);         /* kasno: sljedeći pokušaj četvrt sekunde dalje */
    if (hz < 3400000 || hz > 3700000) return;     /* kraj framea, ili besmislica: opet */
    int ntsc = hz > 3563000;
    if (ntsc != last) { last = ntsc; return; }    /* još jednom, za sigurnost */
    /* prvo perioda, zatim restart, pa rate (objavljen): jezgra 3 koja modelira
       read poziciju s novim rateom vidi i restart, pa lead koji dobije iz
       starog ishodišta uvijek završi u tom jednom restartu - koji programira
       novu periodu i čisti paula_reset */
    paula_period = ntsc ? 128 : PAULA_PERIOD;
    if (ntsc && __atomic_load_n(&running, __ATOMIC_ACQUIRE))
        __atomic_store_n(&paula_reset, 1, __ATOMIC_RELEASE);   /* reproducira na 127: restart na 128 */
    __atomic_store_n(&sample_hz, (ntsc ? 3579545u : 3546895u) / paula_period, __ATOMIC_RELEASE);
    __atomic_store_n(&crystal_known, 1, __ATOMIC_RELEASE);
    aga_diag_log(ntsc ? "[AGA] audio: NTSC crystal - the real Paula plays the rings at period 128"
                      : "[AGA] audio: PAL crystal - the real Paula plays the rings at period 127");
}

/* Rate kojim writer proizvodi uzorke, u sekundi stvarnog vremena: onaj kojim
   ih prava Paula reproducira (sample_hz). 27,928 na PAL kristalu, kao uvijek;
   27,965 na NTSC-ovom, gdje je 27,928 ostavljalo 37 uzoraka u sekundi da ih
   popune repeats od jednog uzorka - zadržavanje svakih 27 ms dok god je zvuk
   svirao (otkrio review). Glue traži taj rate od core mixa, a HDMI-jev
   resampler počinje od njega. */
uint32_t aga_audio_rate(void)
{
    return __atomic_load_n(&sample_hz, __ATOMIC_ACQUIRE);
}

/* Stereo i Left and right u Sound prozoru (agaboot SOUND STEREO / SWAP, $585n
   i $5860/$5861 iz 68k trapa na jezgri 0: obični storeovi). Rade na mixu koji
   uzimaju oba izlaza - ringovi prave Paule i HDMI - pa ih dobiva i System wide
   Workbench, uključujući AHI na paula.audio. */
static volatile int32_t stereo_width16 = 0x10000;     /* Amigina vlastita separacija */
static volatile int stereo_swap;
void aga_audio_stereo(int i) { if (i >= 0 && i < 5) stereo_width16 = (int32_t)((mix_width_pc(i) << 16) / 100); }
void aga_audio_swap(int on)  { stereo_swap = on ? 1 : 0; }
void aga_audio_width_swap(int32_t *width16, int *swap) { *width16 = stereo_width16; *swap = stereo_swap; }

/* Poziva se iz chipset loopa (jezgra 3, izvan chipset locka) s uzorcima
   izvučenima iz jezgre od zadnjeg poziva. */
void aga_audio_stream(const int16_t *lr, int frames)
{
    /* prvo width i swap, na mjestu: lr je buffer aga_audio_scratch, glueov
       vlastiti. Na Amiginoj širini i bez swapa ništa se ne dira. */
    int32_t w16 = stereo_width16;
    int sw = stereo_swap;
    if (w16 != 0x10000 || sw) {
        int16_t *m = (int16_t *)lr;
        for (int i = 0; i < frames; i++) mix_stereo(&m[2 * i], &m[2 * i + 1], w16, sw);
    }
    aga_diag_set(AGA_D_AUDIO_MIX, (uint32_t)((w16 * 100 + 0x8000) >> 16) | (uint32_t)sw << 8);
    /* isti mix prvo na HDMI: ne treba ring u pravom chip RAM-u, pa svira i
       tamo gdje prava Paula ispod mora ostati tiha. Osim ako HDMI ne uzima
       Paulu band-limited: tada mu glue predaje Pauline korake. */
    if (!aga_hdmi_paula_bl()) aga_hdmi_feed(lr, frames);
    if (!lead_target) {
        lead_target = aga_pi3_mode() ? LEAD_PI3 : LEAD_PI4;
        /* kaže u logu koje je tolerancije dobila ova ploča, da se izvršavanje
           na kernelu koji nikad nije učitan ne može smatrati njegovim testom */
        aga_diag_log(lead_target == LEAD_PI3
            ? "[AGA] audio: Pi 3 - lead 1024 samples, loop catches up to two frames"
            : "[AGA] audio: lead 512 samples, loop catches up to one frame");
    }
    /* Sandbox mode: AmigaOS owns every byte of chip RAM, so without a ring
       agaboot allocated for me there is nowhere safe to put the samples.
       Silence is the correct failure - the alternative is writing 8 KB over
       whatever the running game was given, which is how this cost me a
       fucking day. In machine mode the real chip RAM is dead space and the
       fallback is fine. */
    if (!ring_owned && aga_sandbox_active) {
        static int moaned;
        if (!moaned) { moaned = 1; kprintf("[AGA] audio: no allocated ring in sandbox mode, staying silent\n"); }
        return;
    }
    if (!running) {
        /* počni s leadom tišine da DMA nikad ne pretekne writera; housekeeper
           čisti prave ringove i programira Paulu */
        for (int i = 0; i < RING_SAMPLES; i++) { lbuf[i] = rbuf[i] = llob[i] = rlob[i] = 0; }
        wr_pos = LEAD_SAMPLES;
        start_tick = cntvct();              /* privremeno; housekeeper postavlja pravi */
        refresh_at = start_tick;
        __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
        clear_pos = 0;
        svc_state = 1;
        __atomic_store_n(&pub_wr, wr_pos, __ATOMIC_RELEASE);
        __atomic_store_n(&restart_req, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
    }

    /* Procijenjena DMA read pozicija.
     *
     * NE `(elapsed << 32) / ticks_per_sample_fp`: elapsed je broj tickova, a
     * njegovo pomicanje ulijevo za 32 prelijeva se čim prijeđe 2^32 - oko 165
     * sekundi na Pi-jevih ~26 MHz. Množenje sample rateom najprije drži svaki
     * međurezultat unutar 64 bita satima - tko bi rekao, koji kurac.
     * restart_req prvi, s acquire: kad
     * pročita 0, start_tick koji je housekeeper upisao prije čišćenja jest onaj
     * koji se ovdje čita, a ne ishodište prije restarta. Rate također s
     * acquire: novi dolazi sa svojim paula_reset. */
    int pending = __atomic_load_n(&restart_req, __ATOMIC_ACQUIRE);
    uint64_t elapsed = cntvct() - start_tick;
    uint32_t rate = __atomic_load_n(&sample_hz, __ATOMIC_ACQUIRE);
    uint32_t rd_pos = (uint32_t)((elapsed * rate) / cntfrq()) % RING_SAMPLES;
    uint32_t lead = (wr_pos - rd_pos) & (RING_SAMPLES - 1);

    if (pending) {
        /* housekeeper još nije restarto DMA: model nema ishodište prema kojem
           bi regulirao, pa samo nastavi bufferati */
    } else if (lead < RESYNC_LOW || lead > RESYNC_HIGH || __atomic_load_n(&paula_reset, __ATOMIC_ACQUIRE)) {
        /* Stvarno izgubljeno: restartaj loop. Čujno, pa mora biti rijetko.
           Reset koji je isključio prave kanale (housekeeperov refresh ga je
           vidio) restarta se na isti način: hardverova read pozicija vratila se
           na početak, a i model mora. */
        __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
        aga_diag_inc(AGA_D_AUDIO_RESYNCS);
        rd_pos = 0;
        wr_pos = LEAD_SAMPLES;
        svc_state = 2;
        __atomic_store_n(&restart_req, 1, __ATOMIC_RELEASE);
    } else if (lead > LEAD_SAMPLES + LEAD_DEADBAND) {
        /* Paula plays on the Amiga's crystal and I write on the Pi's, so the
         * two clocks drift apart for ever - that is not a fault to recover
         * from, it is the normal condition, and the only question is how the
         * hell it is absorbed.
         *
         * Nekada se apsorbirao u program_paula(), koji izravno restarta DMA
         * loop. To je tvrd diskontinuitet nasred glazbe, a drift je mnogo veći
         * nego što je dizajn pretpostavljao: 31 restart u 110 sekundi, jedan
         * svakih tri i pol sekunde, gdje komentar obećava "nečujno
         * zastajkivanje svakih nekoliko minuta". Beats koji slijeću na krivo
         * mjesto točno je kako to zvuči.
         *
         * Apsorbiraj ga umjesto toga jedan uzorak po jedan. Ovo se izvodi oko
         * 437 puta u sekundi, pa guranje za jedan uzorak po pozivu ispravlja
         * mnogo više drifta nego što clockovi mogu proizvesti, a jedan uzorak
         * na 27.9 kHz je 36 mikrosekundi - nečujno, dok loop restart nije. */
        wr_pos = (wr_pos - 1) & (RING_SAMPLES - 1);   /* ide ispred: ispusti jedan */
        aga_diag_inc(AGA_D_AUD_NUDGE);
        aga_diag_inc(AGA_D_AUD_DROP);
    } else if (lead + LEAD_DEADBAND < LEAD_SAMPLES) {
        /* Ide iza: ponovi zadnji uzorak. Slot mora biti NAPUNJEN, ne samo
           preskočen - samo napredovanje wr_pos ostavlja u ovom slotu ono što je
           upisano prije punog kruga ringa (73 ms), a packer ispod to staro
           pisanje isporuči DAC-u. Zadržavanje od jednog uzorka je nečujno;
           uzorak star 73 ms je širokopojasni klik. */
        uint32_t prev = (wr_pos - 1) & (RING_SAMPLES - 1);
        lbuf[wr_pos] = lbuf[prev];  llob[wr_pos] = llob[prev];
        rbuf[wr_pos] = rbuf[prev];  rlob[wr_pos] = rlob[prev];
        wr_pos = (wr_pos + 1) & (RING_SAMPLES - 1);
        aga_diag_inc(AGA_D_AUD_NUDGE);
        aga_diag_inc(AGA_D_AUD_REPEAT);
    }

    aga_diag_inc(AGA_D_AUDIO_STREAMS);
    /* Pretvori u 8 bita, pa gurni svaku KOMPLETNU grupu od četiri uzorka koja
       još nije gurnuta - praćenu vlastitim pointerom.
       Nekada se flushalo samo na `(wr_pos & 3) == 0`, što pretpostavlja da
       wr_pos napreduje samo za jedan. Drift pomaci iznad pomiču ga za +1 ili
       -1, a to mijenja njegov ALIGNMENT: granica grupe može se preskočiti, pa
       se ta četiri uzorka nikad ne upišu u chip RAM i DAC reproducira ono što
       je prethodni krug ostavio tamo. Četiri stara uzorka su klik; ako jedan
       padne na kratki zvučni efekt, efekta nema. Zasebni flush pointer ne može
       preskočiti grupu kako god se wr_pos pomakne. */
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
            int32_t hi = v14 >> 6;                 /* aritmetički: -127..127 */
            int32_t lo = v14 - (hi << 6);          /* 0..63 */
            if (ch == 0) { lbuf[wr_pos] = (uint8_t)(int8_t)hi; llob[wr_pos] = (uint8_t)lo; }
            else         { rbuf[wr_pos] = (uint8_t)(int8_t)hi; rlob[wr_pos] = (uint8_t)lo; }
        }
        if (AGA_PROBES_ON) aga_diag_wave(lbuf[wr_pos], rbuf[wr_pos]);
        wr_pos = (wr_pos + 1) & (RING_SAMPLES - 1);
    }
    /* bajtovi su u lokalnim ringovima: reci housekeeperu dokle */
    __atomic_store_n(&pub_wr, wr_pos, __ATOMIC_RELEASE);

    /* Osvježi prave registre s vremena na vrijeme, jer 68k reset resetira i
       pravu Paulu. SAMO-SET: ovo ne smije restartati DMA.

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

/* Dio housekeeper jezgre, poziva se iz njezina loopa pri svakom posjetu (oko
   milijun puta u sekundi, bez zajebancije): najviše jedna grupa od četiri
   uzorka po posjetu, pa IPL uzorkovanje zbog kojeg postoji nikad ne čeka iza
   više od četiri bus pisanja - i, dok je HAM6 RGB porta uključen, četiri
   riječi njegovog framea (aga_rgbout.c, klasični PiStorm). Svježi start čisti
   četiri prava ringa šesnaest riječi po posjetu, zatim programira Paulu i
   zabilježi trenutak kad je DMA stvarno počeo. */
extern void aga_hvs_service(void);      /* aga_hvs.c: HVS watch nakon predaje */
/* agaboot WEDGETEST YES ($52ED, od 68k): sljedeći posjet uzima bus lock i
   nikad ga ne vraća - housekeeper zaglavljen kako je bio testerov PiStorm16,
   pa Ctrl-Amiga-Amiga mora proći kroz drugu jezgru (ps_wedge_reset u
   protokolnim fileovima). Samo za isprobavanje tog puta. */
static volatile int wedge_req;
void aga_wedge_test(void) { wedge_req = 1; }
void aga_audio_bus_service(void)
{
#ifndef FRF_VIRTUAL_AGA
    if (__builtin_expect(wedge_req, 0)) { extern void ps_wedge_test(void); ps_wedge_test(); }
#endif
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
        /* perioda slijedi kristal (crystal_measure). Reset ili nova perioda
           viđeni prije ove točke pokriveni su ovim programiranjem, pa njihov
           paula_reset sada odlazi - ostavljen, jezgra 3 restarta drugi put
           čim ovaj završi (otkrio review). Onaj podignut nakon toga, na ovoj
           istoj jezgri, još dobiva svoj restart. */
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
        /* SAMO-SET, bez DMACON clear: falling edge na AUDxEN restarta kanal iz
           AUDxLC i model read pozicije od tada bi regulirao fikciju (vidi
           povijest u gitu za cijelu priču). Jedan korak po posjetu, najviše
           četiri bus pisanja svaki, kako uzorci idu. */
        if (refresh_step == 0) {
            /* DMAEN i AUD0-3 još svi uključeni? Jedan isključen: reset je uzeo
               prave kanale - jezgra 3 restarta stream (paula_reset) */
            if ((ps_read_16(0xDFF002) & 0x020F) != 0x020F) {
                __atomic_store_n(&paula_reset, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&reinit_req, 0, __ATOMIC_RELEASE);
            } else {
                refresh_step = 1;
            }
        } else {
            int n = (refresh_step - 1) >> 1;       /* koraci 1-8: dva po kanalu */
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
    /* Udaljenost naprijed od flush_pos, modulo ring. -1 nudge na jezgri 3
       može cilj staviti IZA mene; pola ringa "udaljenosti" znači točno to, a
       odgovor je odustati od razlike, ne proći cijeli ring pišući stare
       uzorke. */
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

/* Izlazak iz sandboxa: zaustavi DAC loopove i zaboravi stream stanje. */
void aga_audio_stop(void)
{
    int was = running;
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    /* pusti housekeeper da završi svoju grupu. Acquire load: nakon
       store-release runninga iznad ne može se podići prije njega (obični load
       bi mogao, pa je program_paula zatim radio nakon $000F ispod i ostavio
       DMA uključen - otkrio review) */
    while (__atomic_load_n(&svc_busy, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    __atomic_store_n(&restart_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&reinit_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&paula_reset, 0, __ATOMIC_RELEASE);
    refresh_step = 0;                            /* refresh sljedeće sesije počinje iznova */
    if (was) ps_write_16(0xDFF096, 0x000F);      /* očisti AUD0..AUD3 */
    have = 0;
    ring_release();
}
