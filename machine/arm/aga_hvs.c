#include "frf_osd_steps.h"
/* AGA-STORM — video izlaz kroz VideoCore HVS (VC6: Pi 4 / CM4).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Scan the AGA frame as an HVS plane on the same display the P96 VideoCore
 * driver owns. Native screen up: I build my own list (one scaled RGBA8888
 * plane) in the free dlist RAM above the allocator and point DISPLIST1 at
 * it. RTG screen in front: DISPLIST1 goes back to whatever list was live
 * (P96's or the firmware's). Same trick the PI Mailbox GPU driver
 * (vc4gpu.c) proved from the 68k side, moved into Emu68.
 *
 * List format: VideoCore.card VC6_SetPanning, scaled plane path.
 */
#include <stdint.h>
#include "aga/chipset.h"
#include "aga_glue.h"
#if defined(FRF_VIRTUAL_AGA) && defined(FRF_RTG_HANDOFF)
#include "frf_rtg_handoff.h"
#endif
#include "aga_crop.h"
#include "aga_sharp.h"
#include "aga_sync.h"

extern void kprintf(const char *fmt, ...);
extern uintptr_t mmu_virt2phys(uintptr_t virt);
extern void arm_flush_cache(uintptr_t addr, uint32_t len);
extern void arm_dcache_invalidate(uintptr_t addr, uint32_t len);

#define LE32(x) __builtin_bswap32(x)            /* Emu68 vrti ARM u big-endian */

/* ---- HVS ------------------------------------------------------------------ */
#define ARM_PERIIOBASE      0xF2000000UL
#define HVS_BASE            (ARM_PERIIOBASE + 0x400000)
/* Dlist RAM is at +0x4000 on VC6 (Pi 4 / CM4), +0x2000 on VC4 (Pi 3), and
   the plane words differ too. Generation settled once at init from CNTFRQ
   (same call Emu68 makes) and every builder below asks. Layouts from
   VideoCore.card's vc4.c / vc6.c. */
#define DLIST_BASE_VC6      (HVS_BASE + 0x4000)
#define DLIST_BASE_VC4      (HVS_BASE + 0x2000)
static int       hvs_gen = 6;
static uintptr_t dlist_base = DLIST_BASE_VC6;
#define REG_DISPLIST1       (HVS_BASE + 0x24)
#define REG_DISPSTAT1       (HVS_BASE + 0x58)
#define REG_DISPLACT1       (HVS_BASE + 0x34)   /* lista koju HVS skenira SADA (latchana iz DISPLIST1 na početku framea) */

/* Where my two lists and the scaling kernel sit in the dlist RAM. These were
   constants once - measured free one day, hardcoded the next. They were not
   free: the machine reported 331 of 331 words in use. VideoCore.card parks
   its video overlay CLUT at 0x400 (overlay.c OVL_CLUT_WORD). An older note
   here claimed the mouse pointer lived there too; the driver source says no
   - see ptr_probe().

   Now picked the moment they are needed, from a survey of what is actually
   free with the desktop up, re-picked on every sandbox entry so the answer
   cannot rot like the constants did. The values below are just the fallback
   when no free run is big enough. */
#define LIST_STRIDE         0xA0UL              /* words per list */
#define KERNEL_WORDS        11                  /* one scaling kernel */
/* two lists, then two kernels: KERNEL_WORD scales across, KERNEL_WORD +
   KERNEL_WORDS down - the TV look blends across only */
#define DL_SPAN             (LIST_STRIDE * 2 + 2 * KERNEL_WORDS)
static uint32_t LIST_A      = 0x400UL;
static uint32_t LIST_B      = 0x4A0UL;
static uint32_t KERNEL_WORD = 0x540UL;

#define CTL_VALID           (1UL << 30)
#define CTL_WORDS(n)        (((n) & 0x3f) << 24)
#define CTL_ALPHA_EXPAND    (1UL << 12)
#define CTL_RGB_EXPAND      (1UL << 11)
#define CTL_FORMAT(n)       ((n) & 0x1f)
#define CTL_PIXEL_ORDER(n)  (((n) & 3) << 13)
#define FMT_RGBA8888        7
#define ORDER_ARGB          2                   /* memory bytes B,G,R,A (little-endian 0xAARRGGBB) */
/* Core writes pixels as native words on a big-endian kernel: memory bytes
   A,R,G,B. HVS reads little-endian, sees 0xBBGGRRAA, calls that BGRA. The
   alternative was swapping 230k words a frame on core 3 - fuck that, the
   order field is free. */
#define ORDER_BGRA          1
/* VC4: no alpha word of its own - alpha rides in POS0 - and the alpha mode
   sits in POS2's top bits. FIXED_NONZERO is what both lists here rely on:
   opaque where the pixel's alpha byte is set (core writes 0xFF), not drawn
   where the word is zero (the idle plane). */
#define VC4_POS0(x, y)      (((x) & 0xfff) | (((y) & 0xfff) << 12) | (0xffUL << 24))
#define VC4_POS1(w, h)      (((w) & 0xffff) | (((h) & 0xffff) << 16))
#define VC4_POS2(w, h)      (((w) & 0xfff) | (((h) & 0xfff) << 16) | (2UL << 30))
#define VC4_CTL_UNITY       (1UL << 4)
/* VC4 names its pixel orders backwards vs the VI: VideoCore.card's vc4.c
   maps Picasso96's A8R8G8B8 - memory bytes A,R,G,B, the core's native words
   spot on - to HVS_PIXEL_ORDER_RGBA, where the VI wants BGRA for the same
   bytes. Measured 2026-09-14: BGRA on a Pi 3 swaps red and blue (Banshee's
   brown sand came out blue). */
#define ORDER_VC4_ARGB_BYTES 0
#define POS0_X(n)           ((n) & 0x2fff)
#define POS0_Y(n)           (((n) & 0x2fff) << 16)
#define POS1_W(n)           ((n) & 0xffff)
#define POS1_H(n)           (((n) & 0xffff) << 16)
#define POS2_W(n)           ((n) & 0x3fff)
#define POS2_H(n)           (((n) & 0x3fff) << 16)
#define POS2_ALPHA_FIXED    (1UL << 30)
#define POS2_ALPHA(n)       (((n) << 4) & 0xfff0)
#define LIST_END            0x80000000UL
#define PPF_SCALER          0xc0000000UL        /* VideoCore.card default (AGC + bilinear) */
#define PPF_PHASE           128

/* Mitchell-Netravali (B = C = 1/3) kernel, VideoCore.card compute_scaling_kernel layout */
static const uint32_t kernel_words[11] = {
    0x07f40000, 0x07e7f1fa, 0x00600ffd, 0x01fcb035, 0x036988a4, 0x0001c6e3,
    0x036988a4, 0x01fcb035, 0x00600ffd, 0x07e7f1fa, 0x07f40000
};
/* Nearest neighbour for crisp square pixels (agaboot ZOOM ... SHARP): the
   Linux vc4 driver's nearest_neighbour_kernel, taps (0,0,0,0,0,0,0,0,1,1,
   1,1,255,255,255,255) in the same layout - three 9-bit taps a word, first
   five words mirrored. The smooth kernel above blurs every edge; an Amiga
   upscaler is supposed to show them hard. */
#define KW(a, b, c) (((uint32_t)(a) & 0x1ff) | (((uint32_t)(b) & 0x1ff) << 9) | (((uint32_t)(c) & 0x1ff) << 18))
static const uint32_t kernel_nearest[11] = {
    KW(0, 0, 0), KW(0, 0, 0), KW(0, 0, 1), KW(1, 1, 1), KW(255, 255, 255), KW(255, 255, 0),
    KW(255, 255, 255), KW(1, 1, 1), KW(0, 0, 1), KW(0, 0, 0), KW(0, 0, 0)
};
/* TV: all four scaler taps equal - every output pixel is the mean of four
   hires pixels, two lores. Games dithered for composite video (Virtual
   Karting: red next to cyan, colour next to black) put their pattern on
   that period and a PAL TV smeared it into the colour meant: grey
   road, green grass. Averaged over one whole period the pattern is gone at
   every phase; a tent or Gaussian that fits in four taps tints every other
   column. Across only - rows stay as the vertical kernel draws them. */
static const uint32_t kernel_tv[11] = {
    KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 0),
    KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64), KW(64, 64, 64)
};
/* The look (agaboot's RTG Image): 0 sharp, 1 smooth, 2 TV. H and V kernels
   are separate words in the list, so TV blends across while the rows keep
   the smooth kernel. */
static const uint32_t *cur_kernel_h = kernel_words, *cur_kernel_v = kernel_words;
/* Sharp since 1.114: nearest where the picture is enlarged by a whole
   factor, and the area kernel for its step where it is not (aga_sharp.h,
   sharp_fit below) - every Amiga pixel as wide and as tall as the next */
static uint32_t kernel_area_h[SHARP_KERNEL_WORDS], kernel_area_v[SHARP_KERNEL_WORDS];
static uint32_t area_step_h, area_step_v;   /* za što su napravljeni; 0 ništa */
static int cur_look = 1;

static inline uint32_t reg_rd(uintptr_t a)          { return LE32(*(volatile uint32_t *)a); }
static inline void     reg_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = LE32(v); }
/* one read of a display-list word, as it comes - only for the reads that
   report and never write anything back (the pointer probe, the map, the
   watch). Everything else reads through dl_rd below; a word read straight
   out of here is whatever the RAM feels like serving. */
static inline uint32_t dl_peek(uint32_t w)          { return reg_rd(dlist_base + w * 4); }
static inline void     dl_wr(uint32_t w, uint32_t v) { reg_wr(dlist_base + w * 4, v); }

/* A word of the display-list RAM, read until two reads in a row agree.
   RGB2RTG found it on 2026-10-09 (its dev34): an ARM read of this RAM now
   and then returns ANOTHER word of the list, roughly one read in 5000, while
   the HVS reads the same RAM - CTL0's bits came back for a scaling word, a
   context word's for a POS0. RGB2RTG kept such a word and wrote it back
   every frame, and POS0's fixed alpha $80 left the HDMI picture dark until
   the game ended. It saw this in the list RAM only and still reads its
   registers (DISPLIST1, the PixelValve) once, so reg_rd stays one read too.

   Here every read whose word gets written back goes through this: dl_borrow's
   backup, which dl_return writes back after every game; the survey, which
   decides which words I write at all; and the windowed list, a word-by-word
   copy of the live RTG list (list_len, base_plane_geometry). A bad backup
   word stays in the RAM after the game. Where the survey found a free run
   that is a stray word in free RAM; where it found none and I sat on words
   in use (aga_dl_inuse not 0 - the Pi 3's map on 2026-09-14 had no free run
   at all), it is a wrong word in somebody else's list or palette. A
   candidate for the odd picture or pointer after games, not a proven cause:
   no misread has been seen in this kernel yet, and the first few now go into
   the diagnostic log so a hardware test can tell.

   A word the HVS rewrites itself (the context words of a plane it is
   scanning) can honestly differ between two reads; it settles at the next
   pair. Eight reads at most, then the last one: a word that keeps moving
   must not stall the frame loop. */
#define DL_RD_TRIES 8                   /* one digit, for the log line */
static uint32_t dl_misreads;            /* reads that disagreed, since boot */
static void hex_put(char *d, uint32_t v, int digits);  /* with the map, below */
static void dl_misread(uint32_t w, uint32_t reads)
{
    uint32_t n = __atomic_add_fetch(&dl_misreads, 1, __ATOMIC_RELAXED);
    if (n > 8) return;                  /* log je za rijetke linije */
    char line[96];
    int k = 0;
    const char *t = "[AGA] HVS: dlist word ";
    while (*t) line[k++] = *t++;
    hex_put(line + k, w, 3); k += 3;
    t = reads ? " agreed at read " : " never agreed, reads ";
    while (*t) line[k++] = *t++;
    line[k++] = (char)('0' + (reads ? reads : DL_RD_TRIES));
    t = (n == 8) ? " - the last such line this boot" : "";
    while (*t) line[k++] = *t++;
    line[k] = 0;
    aga_diag_log(line);
}
static uint32_t dl_rd(uint32_t w)
{
    uint32_t v = dl_peek(w);
    for (uint32_t k = 2; k <= DL_RD_TRIES; k++) {
        uint32_t x = dl_peek(w);
        if (x == v) {
            if (k > 2) dl_misread(w, k);
            return v;
        }
        v = x;
    }
    dl_misread(w, 0);
    return v;
}

static void write_kernels(void)
{
    for (int i = 0; i < KERNEL_WORDS; i++) {
        dl_wr(KERNEL_WORD + i, cur_kernel_h[i]);
        dl_wr(KERNEL_WORD + KERNEL_WORDS + i, cur_kernel_v[i]);
    }
}

/* ---- borrowing display-list memory ----------------------------------------
   LIST_A, LIST_B and KERNEL_WORD point into HVS dlist RAM that happened to
   be free when measured. Not mine to keep: other code writes that RAM too
   (the video overlay CLUT sits at 0x400). Save a copy before the first
   write, put it back when the screen is released, wherever the survey below
   parked me.

   First version assumed the RTG driver's mouse pointer plane lived in these
   words. VideoCore.card's source (vc6.c VC6_SetPanning) builds the pointer
   plane and palette inside the driver's own buddy block below 0x300, image
   in Amiga fast RAM - and saving/restoring these words never brought the
   pointer back, which fits. */
#define DL_BORROW_WORDS  DL_SPAN
static uint32_t dl_backup[DL_BORROW_WORDS];
static uint32_t dl_borrow_first;
static int      dl_borrowed;
uint32_t aga_dl_inuse;      /* words that were non-zero when borrowed */
uint32_t aga_dl_free_at;    /* start of the longest free run in the dlist RAM */
uint32_t aga_dl_free_len;   /* and how long it is - both published to agastat */

static void dl_survey(void);    /* definirano niže; zove se iz dl_borrow */

static void dl_borrow(void)
{
    if (dl_borrowed) return;

    /* What is free NOW, desktop up, driver settled - not what somebody
       measured free once. */
    dl_survey();
    if (aga_dl_free_len >= DL_SPAN) {
        LIST_A      = aga_dl_free_at;
        LIST_B      = LIST_A + LIST_STRIDE;
        KERNEL_WORD = LIST_B + LIST_STRIDE;
    }

    dl_borrow_first = LIST_A;
    uint32_t used = 0;
    for (uint32_t i = 0; i < DL_BORROW_WORDS; i++) {
        /* dl_return ovo vraća nakon igre: jedno čitanje koje je vratilo drugi
           word ostalo bi u RAM-u (dl_rd, 2026-10-10) */
        dl_backup[i] = dl_rd(dl_borrow_first + i);
        if (dl_backup[i]) used++;
    }
    aga_dl_inuse = used;        /* 0 = somewhere genuinely spare */
    dl_borrowed = 1;
    /* Kernels go in now, words are mine and their old shit is saved: always,
       wherever the lists landed, whichever look is chosen. Used to write
       them only when a new spot was found - and before the backup, so
       handing back "restored" my own kernel. Otherwise the boot-time one
       stayed: agaboot ZOOM ... SHARP picked nearest and the screen kept
       smooth. */
    write_kernels();
}

/* Where in the dlist RAM is there actually room?

   dlist_words_inuse came back 331 of 331: not one word at LIST_A/LIST_B/
   KERNEL_WORD was free. They land on overlay.c's OVL_CLUT_WORD, the video
   overlay palette. That is NOT where the mouse pointer lives, so moving
   stops a real trespass but is not the pointer fix; the PTR_* counters
   exist to find that.

   Find the longest run of zero words in the 4096-word (16 KB) dlist RAM and
   report it, so the next build places my two lists and the kernel somewhere
   the driver is not - from a measurement, not a guess gone stale. */
#define DL_RAM_WORDS  4096

/* Never below 0x500. Per VideoCore.card's source everything under it is
   spoken for even where it reads zero: buddyalloc.c manages words 0-0x2FF
   (live lists, pointer plane and its palette), vc6.c keeps Picasso96's
   palette and the Unicam list at 0x300, overlay.c has the 256-entry overlay
   CLUT at 0x400. A mostly black palette is one long zero run and the survey
   must not call it room. */
#define DL_SURVEY_FLOOR 0x500

static void dl_survey(void)
{
    uint32_t best_at = 0, best_len = 0, run_at = 0, run = 0;
    /* dl_rd, ne jedno čitanje: word u upotrebi koji se jednom pročitao kao
       nula stavio bi moje liste na njega. Dva čitanja po wordu gdje je išlo
       jedno - oko 3600 čitanja više, jednom po igri na prvom frameu
       (dl_borrow). */
    for (uint32_t i = DL_SURVEY_FLOOR; i < DL_RAM_WORDS; i++) {
        if (dl_rd(i) == 0) {
            if (!run) run_at = i;
            run++;
            if (run > best_len) { best_len = run; best_at = run_at; }
        } else {
            run = 0;
        }
    }
    /* dl_return() zeroes my lists before the next survey, so last time's
       region can be picked again. */
    aga_dl_free_at = best_at;
    aga_dl_free_len = best_len;
}

static void dl_return(void)
{
    if (!dl_borrowed) return;
    for (uint32_t i = 0; i < DL_BORROW_WORDS; i++)
        dl_wr(dl_borrow_first + i, dl_backup[i]);
    dl_borrowed = 0;
}

/* ---- the RTG mouse pointer, read out of the driver's live list -------------
   Diagnostic only - writes nothing, and reads one word at a time (dl_peek):
   a misread here is a wrong counter, never a wrong word in a list. Walks a
   list the way VideoCore.card vc6.c VC6_SetPanning lays it out, unity path
   and scaled: screen plane, pointer plane (CTL, POS0 at +1, PTR0 at +6,
   18 words), optional overlay plane, then 0x80000000, one zero word
   (colour 0, transparent) and the pointer's three colours. Fills six
   counters from `base`: LIST PLANES CTL POS0 IMAGE PAL. */
static void ptr_probe(uint32_t list, int base)
{
    if (hvs_gen != 6) return;            /* samo VC6 layout; diagnostic */
    uint32_t planes = 0, ctl2 = 0, pos0 = 0, image = 0, pal = 0;
    uint32_t o = list;
    for (int n = 0; n < 8 && o + 6 < DL_RAM_WORDS; n++) {
        uint32_t ctl = dl_peek(o);
        if (ctl == LIST_END) {
            pal = dl_peek(o + 2);           /* terminator, boja 0, boja 1 */
            break;
        }
        uint32_t words = (ctl >> 24) & 0x3f;
        if (!(ctl & CTL_VALID) || words == 0) break;
        if (++planes == 2) {                /* plane 2 je pointer */
            ctl2  = ctl;
            pos0  = dl_peek(o + 1);
            image = dl_peek(o + 6);
        }
        o += words;
    }
    aga_diag_set(base + 0, list);
    aga_diag_set(base + 1, planes);
    aga_diag_set(base + 2, ctl2);
    aga_diag_set(base + 3, pos0);
    aga_diag_set(base + 4, image);
    aga_diag_set(base + 5, pal);
}

/* ---- mailbox (koristi se samo pri initu, na coreu 0) ----------------------- */
#define MBOX_BASE           (ARM_PERIIOBASE + 0xB880)
#define MBOX_READ           (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS         (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE          (*(volatile uint32_t *)(MBOX_BASE + 0x20))
#define MBOX_FULL           0x80000000u
#define MBOX_EMPTY          0x40000000u
/* 128 bajtova na 64-byte granici: dva cijela cache linea u kojima ne živi
   ništa drugo. Bio je aligned(16), jebote, pa su se lineovi u kojima sjedi
   mogli dijeliti sa susjednim staticima, a susjeda kojeg drugi core upiše
   između cleana i firmwareovog odgovora ponovno zaprlja line sa STARIM
   requestom u njemu - vraćenim pri evictionu, preko odgovora. */
/* U low memory od 1.121: Emu68 premješta svoj kernel na vrh RAM-a, a na Pi
   s više od 1 GB (CM4 na 0x7a7c2440) buffer među mojim staticima je iza
   onoga u što firmware piše - vrati adresu i ostavi odgovor nenapisan, pa je
   temperatura na CM4 čitala 0 dok je display size, pitan prije premještanja,
   stigao. Emu68 vlastiti FBReq živi na phys 0x1000-0x11ff; moji uzimaju
   0x1c00 i 0x1c80 u tom pageu, po jedan 64-byte-aligned 128-byte blok (req
   ovdje, hud_req za core 3). */
static uint32_t req_storage[32] __attribute__((aligned(64)));
static uint32_t hud_storage[32] __attribute__((aligned(64)));
#define MBOX_LOW(off)  ((off)==0x1c00 ? req_storage : hud_storage)
#define MBOX_BUF_BYTES 128
static uint32_t *const req = MBOX_LOW(0x1c00);

static inline uint64_t mbox_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }

/* Jedan property-channel poziv. Čeka MOJ odgovor: firmware vraća adresu
   buffera koju je dobio, a odgovor za buffer nekog drugog pozivatelja
   (ostavljen u queueu ili stigao prvi) nije moj da se na njemu zaustavim.
   Prva verzija pucala je na svaki channel-8 odgovor i onda čitala buffer na
   koji firmware još nije odgovorio: temperature read pri svakom ulasku i
   izlasku iz sandboxa vraćao se s response codeom 0, na obje ploče, cijelo
   vrijeme, dok je poziv pri video initu slučajno radio. Ograničeno na 100 ms
   da firmware koji nikad ne odgovori ne može zaglaviti pozivatelja. */
/* Od 1.121 pita i core 3 (OSD monitor line, vlastiti buffer), pa moji
   pozivi idu naizmjence: svaki bi odgovor onoga drugog odbacio kao foreign.
   wait 0: odustani odmah kad je neki moj u tijeku. */
static volatile int mbox_busy;

static int mbox_call_on(uint32_t *b, uint32_t bytes, int wait)
{
    while (__atomic_test_and_set(&mbox_busy, __ATOMIC_ACQUIRE)) {
        if (!wait) return -1;
        __asm__ volatile("yield");
    }
    int rc = -1;
    uintptr_t phys = mmu_virt2phys((uintptr_t)b);
    uint32_t addr = (uint32_t)(phys & ~0xFu);
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t end = mbox_now() + f / 10;
    arm_flush_cache((uintptr_t)b, bytes);
    while (LE32(MBOX_STATUS) & MBOX_FULL) if (mbox_now() > end) { aga_diag_inc(AGA_D_MBOX_TIMEOUTS); goto out; }
    MBOX_WRITE = LE32(addr | 8);
    for (;;) {
        while (LE32(MBOX_STATUS) & MBOX_EMPTY)
            if (mbox_now() > end) { aga_diag_inc(AGA_D_MBOX_TIMEOUTS); goto out; }
        uint32_t r = LE32(MBOX_READ);
        if ((r & 0xF) == 8 && (r & ~0xFu) == addr) break;
        aga_diag_inc(AGA_D_MBOX_FOREIGN);
    }
    arm_dcache_invalidate((uintptr_t)b, bytes);
    rc = (LE32(b[1]) == 0x80000000u) ? 0 : -1;
out:
    __atomic_clear(&mbox_busy, __ATOMIC_RELEASE);
    return rc;
}

static int mbox_call(void) { return mbox_call_on(req, MBOX_BUF_BYTES, 1); }

/* Firmwareov pogled na SoC: temperatura (milistupnjevi) i GET_THROTTLED
   bitovi - 0 under-voltage sada, 1 frekvencija ograničena sada, 2 throttled
   sada, 3 soft temperature limit sada; bitovi 16-19 isto od boota. Neovisno
   o video putu, pa radi i na Pi 3. */
int aga_soc_status(uint32_t *temp_mc, uint32_t *throttled)
{
    int c = 1;
    req[c++] = 0;
    req[c++] = LE32(0x30006); req[c++] = LE32(8); req[c++] = 0;   /* GET_TEMPERATURE id 0 */
    int tpos = c; req[c++] = 0; req[c++] = 0;
    req[c++] = LE32(0x30046); req[c++] = LE32(4); req[c++] = 0;   /* GET_THROTTLED, 0 = ne čisti */
    int fpos = c; req[c++] = 0;
    req[c++] = 0;
    req[0] = LE32(c << 2);
    mbox_call();
    aga_diag_inc(AGA_D_SOC_CALLS);
    aga_diag_set(AGA_D_SOC_CODE, LE32(req[1]));   /* $80000000 = firmware je odgovorio */
    if (LE32(req[1]) != 0x80000000u) return -1;
    *temp_mc = LE32(req[tpos + 1]);
    *throttled = LE32(req[fpos]);
    return 0;
}

/* Firmware display blanking (property tag 0x40003), the call
   VideoCore.card makes for Picasso96's SetSwitch. Pi 4 firmware seems to
   ignore it; Pi 3 honours it, and with no unblank the desktop is drawn
   into a dark output. Sent on the way out on a VC4. */
static int fw_blank(int blank)
{
    int c = 1;
    req[c++] = 0;
    req[c++] = LE32(0x40003); req[c++] = LE32(4); req[c++] = 0;
    int vpos = c; req[c++] = LE32((uint32_t)(blank ? 1 : 0));
    req[c++] = 0;
    req[0] = LE32(c << 2);
    mbox_call();
    return (LE32(req[1]) == 0x80000000u) ? (int)(LE32(req[vpos]) & 1) : -1;
}

/* ---- state ---------------------------------------------------------------- */
static uint8_t  *fb_va;             /* three frames of AGA_OUT_W x AGA_OUT_H x 4, uncached */
static uintptr_t fb_phys;
static int       fb_page;
static uint32_t  disp_w, disp_h;
static uint32_t  dst_w, dst_h, dst_x, dst_y;
static uint32_t  scale_x, scale_y;  /* 16.16, src/dst */
/* The part of the frame shown full screen (agaboot ZOOM). The frame is the
   whole overscan area, 800 x 286 from lores column $50 and line $1A, and a
   standard 640 x 256 screen is just the middle: fitted whole, a Workbench
   came out 63% of a 1080p width with border all round - an Amiga upscaler
   shows the visible area, not that. Plane just starts further in: same
   pitch, smaller source, nothing more to render. */
static uint32_t  crop_x, crop_y, crop_w = AGA_OUT_W, crop_h = AGA_OUT_H, crop_off;
static int       zoom_gen, cur_zoom_gen = -1;
static void fit(void);
static void crop_state(void);       /* the custom crop, AGA_D_CROP (below) */
static void snap_take(void);        /* the last game's picture kept (below) */
static void hsync_restore(void);    /* HDMI sync: the mode's own porch again (below) */
static void hsync_park(void);       /* 1.118: park adjusted porch for next game */
static uint32_t  saved_list;        /* DISPLIST1 as found before the takeover */

/* Map the dlist RAM into the diag log: every run of non-zero words as
   start:length (hex words), plus DISPLIST1. On the Pi 3 the survey found no
   free run at all (2026-09-14) and my lists fell back to the compile-time
   words; this says what the RAM really holds - at boot, first frame, after
   the hand-back - with no serial console. */
static void hex_put(char *d, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) { d[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
}
static void dl_map_log(const char *tag)
{
    char line[120];
    int n = 0;
    while (*tag && n < 24) line[n++] = *tag++;
    line[n++] = ' '; line[n++] = 'L';
    hex_put(line + n, reg_rd(REG_DISPLIST1), 4); n += 4; line[n++] = ' ';
    uint32_t run_at = 0, run = 0; int runs = 0;
    for (uint32_t i = 0; i <= DL_RAM_WORDS && runs < 12; i++) {
        uint32_t v = (i < DL_RAM_WORDS) ? dl_peek(i) : 0;
        if (v) { if (!run) run_at = i; run++; }
        else if (run) {
            if (n + 9 >= (int)sizeof line) break;
            hex_put(line + n, run_at, 3); n += 3; line[n++] = ':';
            hex_put(line + n, run, 3); n += 3; line[n++] = ' ';
            run = 0; runs++;
        }
    }
    line[n] = 0;
    aga_diag_log(line);
}
/* osam wordova liste, za isti log */
static void dl_words_log(const char *tag, uint32_t at)
{
    char line[120];
    int n = 0;
    while (*tag && n < 24) line[n++] = *tag++;
    line[n++] = ' '; hex_put(line + n, at, 4); n += 4; line[n++] = ':';
    for (int i = 0; i < 8; i++) { line[n++] = ' '; hex_put(line + n, dl_peek(at + i), 8); n += 8; }
    line[n] = 0;
    aga_diag_log(line);
}

/* The watch. hide() arms it; the housekeeper calls aga_hvs_service() every
   loop and it samples every 20 ms for three seconds: does DISPLIST1 still
   hold what got written, what does DISPLACT1 say the HVS is really
   scanning, does the line counter move between samples at all. Last sample
   dumps the map and the live list's first words. */
static volatile uint64_t watch_next, watch_end;
static uint32_t watch_list_prev, watch_stat_prev;
static int      watch_samples;
static inline uint64_t cnt_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }
static void watch_arm(void)
{
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t now = cnt_now();
    watch_samples = 0;
    watch_list_prev = reg_rd(REG_DISPLIST1);
    watch_stat_prev = reg_rd(REG_DISPSTAT1);
    aga_diag_set(AGA_D_WATCH_SAMPLES, 0);
    aga_diag_set(AGA_D_WATCH_LIST_CHANGES, 0);
    aga_diag_set(AGA_D_WATCH_STAT_MOVES, 0);
    aga_diag_set(AGA_D_WATCH_LACT_FIRST, reg_rd(REG_DISPLACT1));
    watch_end = now + f * 3;
    __atomic_store_n(&watch_next, now + f / 50, __ATOMIC_RELEASE);
}
/* Wait for the HVS to latch DISPLIST1 into DISPLACT1 - frame start only -
   so the words of the list it is scanning can be rewritten. Bounded: a
   stalled channel must not stall the caller too. Returns 1 when DISPLACT1
   reads the list asked for. */
static int wait_latched(uint32_t list, uint32_t timeout_us)
{
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t end = cnt_now() + (f * timeout_us) / 1000000u;
    do {
        if (reg_rd(REG_DISPLACT1) == list) return 1;
    } while (cnt_now() < end);
    return reg_rd(REG_DISPLACT1) == list;
}
void aga_hvs_service(void)
{
    if (!(AGA_PI3_BUILD && AGA_PROBES_ON)) return;   /* Pi 3 test kernels only */
    uint64_t nx = __atomic_load_n(&watch_next, __ATOMIC_ACQUIRE);
    if (!nx) return;
    uint64_t now = cnt_now();
    if (now < nx) return;
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint32_t list = reg_rd(REG_DISPLIST1);
    uint32_t lact = reg_rd(REG_DISPLACT1);
    uint32_t stat = reg_rd(REG_DISPSTAT1);
    if (list != watch_list_prev) aga_diag_inc(AGA_D_WATCH_LIST_CHANGES);
    if ((stat & 0xfff) != (watch_stat_prev & 0xfff)) aga_diag_inc(AGA_D_WATCH_STAT_MOVES);
    watch_list_prev = list; watch_stat_prev = stat;
    watch_samples++;
    aga_diag_set(AGA_D_WATCH_SAMPLES, (uint32_t)watch_samples);
    aga_diag_set(AGA_D_WATCH_LIST_LAST, list);
    aga_diag_set(AGA_D_WATCH_LACT_LAST, lact);
    aga_diag_set(AGA_D_WATCH_STAT_LAST, stat);
    aga_diag_set(AGA_D_WATCH_CTL_LAST, dl_peek(lact));
    aga_diag_set(AGA_D_WATCH_PTR0_LAST, dl_peek(lact + 4));
    if (now >= watch_end) {
        __atomic_store_n(&watch_next, (uint64_t)0, __ATOMIC_RELEASE);
        dl_map_log("[AGA] dlist@+3s");
        dl_words_log("[AGA] DISPLACT1 words", lact);
        if (list != lact) dl_words_log("[AGA] DISPLIST1 words", list);
    } else {
        __atomic_store_n(&watch_next, now + f / 50, __ATOMIC_RELEASE);
    }
}
static uint32_t  cur_list;          /* LIST_A/LIST_B kad je moja live, inače 0 */
static uint32_t  ptr0_word;         /* dlist word index PTR0 moje plane */
static int       video_ok;
#if defined(FRF_VIRTUAL_AGA) && defined(FRF_RTG_HANDOFF)
static frf_rtg_handoff frf_rtg_display;
#endif


#define FB_BYTES   (AGA_OUT_W * AGA_OUT_H * 4)
static inline int fb_next_page(int p) { return p >= 2 ? 0 : p + 1; }

int aga_video_init_at(uint8_t *fbmem_va, uintptr_t fbmem_phys)
{
    fb_va = fbmem_va;
    fb_phys = fbmem_phys;

    /* Which HVS. Emu68 tells a Pi 3 from a Pi 4 by the generic timer - 19.2
       vs 54 MHz - and so do I. A VC6 list written into a VC4 scaler corrupts
       it (first Pi 3 boot of this kernel froze on the Emu68 logo that way),
       so the generation picks the RAM offset and every plane word from here
       on. */
    {
        uint64_t cntfrq;
        __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));
        if (cntfrq <= 20000000) { hvs_gen = 4; dlist_base = DLIST_BASE_VC4; }
        else                    { hvs_gen = 6; dlist_base = DLIST_BASE_VC6; }
        kprintf("[AGA] HVS: VideoCore %s, display lists at %p\n", hvs_gen == 4 ? "IV (Pi 3)" : "VI (Pi 4)", (void *)dlist_base);
    }
    if (hvs_gen == 4 && !AGA_PI3_BUILD) {
        /* Pi 4 kernel on a Pi 3. Its lists are VC6 lists and corrupt a VC4
           scaler, so no AGA picture: games still run, AGA toggle tells the
           user to rerun the installer for the Pi 3 kernel. */
        aga_diag_log("[AGA] HVS: Pi 4 kernel on a Pi 3 - no AGA picture, reinstall for the Pi 3");
        return -1;
    }
    if (AGA_PI3_BUILD && AGA_PROBES_ON) dl_map_log("[AGA] dlist@boot");

    /* fizička veličina displaya */
    int c = 1;
    req[c++] = 0;
    req[c++] = LE32(0x40003); req[c++] = LE32(8); req[c++] = 0;
    int pos = c;
    req[c++] = 0; req[c++] = 0;
    req[c++] = 0;
    req[0] = LE32(c << 2);
    mbox_call();
    disp_w = LE32(req[pos]);
    disp_h = LE32(req[pos + 1]);
    if (disp_w < 320 || disp_h < 200 || disp_w > 4096 || disp_h > 4096) {
        kprintf("[AGA] HVS: implausible display size %dx%d, video disabled\n", disp_w, disp_h);
        return -1;
    }

    fit();

    write_kernels();

    saved_list = reg_rd(REG_DISPLIST1);
    cur_list = 0;
#if defined(FRF_VIRTUAL_AGA) && defined(FRF_RTG_HANDOFF)
    frf_rtg_handoff_init(&frf_rtg_display, saved_list);
    aga_diag_log("[RTG] HDMI HVS arbitration active, waiting for external Picasso96 list");
#endif

    video_ok = 1;
    crop_state();                       /* bit 0: Custom shvaćen */
    kprintf("[AGA] HVS: display %dx%d, AGA plane %dx%d at %d,%d (scale %08x/%08x), fb %p\n",
            disp_w, disp_h, dst_w, dst_h, dst_x, dst_y, scale_x, scale_y, (void *)fb_phys);
    return 0;
}

int aga_video_init(void)
{
    return video_ok ? 0 : -1;
}

/* The frame about to be shown, from the chipset loop (aga_video_set_frame):
   woven by the core - both pages as one frame of 572 rows, own field on rows
   2y + frame_row (aga_set_weave) - and PAL or NTSC. What the plane reads
   follows (set_source): src_rows rows of src_pitch bytes from crop_off. */
static int frame_woven, frame_row, frame_ntsc;
static uint32_t src_rows = AGA_OUT_H, src_pitch = AGA_OUT_W * 4;

/* Fit the shown part of the frame into the display. Scale (agaboot RTG
   Image, $597S), RGB2RTG's three:
     0 4:3      shape a real monitor gives - hires pixel is 0.52 of a line's
                height (14.75 MHz square-pixel rate over the 28.37 MHz hires
                one, a line two PAL scanlines tall: RGB2RTG's measure) - as
                big as fits
     1 integer  that shape, whole number of display lines per Amiga line:
                sharpest, scanlines come out even
     2 fill     whole display, shape be damned
   and since 1.116 RGB2RTG's shapes (0.75-dev63/64):
     3 5:4      square pixels - hires pixel half a line's height, PAL or
                NTSC: a PAL screen 5:4 (on a 1280 x 1024 monitor 4x
                exactly), an NTSC one 16:10
     4-7        16:9, 16:10, 21:9, 32:9: the picture stretched to that
                shape, whatever its own, as big as fits
   Up to 1.106 the one fit took hires pixels square on doubled lines, 4%
   narrower than a monitor.

   NTSC hires pixel is 0.43 of a line's height (12.27 MHz square pixels over
   the same 28.37 MHz, RGB2RTG's measure too). TV border ($59Bn across,
   $59Cn down, 0-10%, RGB2RTG's) keeps that much of the display clear round
   the picture for a TV that cuts the edges off 1080p: picture fits what is
   left, in shape, centred. */
static int scale_mode;
static int border_x, border_y;
static int scan_pct, lscan_pct;    /* the scanlines (below), for Whole lines */
static int nscan_pct = -1;  /* v1.118 NTSC scanlines: -1 inherits PAL setting */
static int scan_pct_now(void)
{
    return frame_woven ? lscan_pct : (frame_ntsc && nscan_pct >= 0) ? nscan_pct : scan_pct;
}

static int scan_shape;  /* 0 Even, 1 Crisp, 2 Whole, 4 Beam */
static int scan_top;    /* 0 bottom, 1 top */
#define SHAPE_WHOLE(x) ((x)==2 || (x)==4)
#define SHAPE_BEAM(x) ((x)==4)
static void set_source(void);
static void fit(void)
{
    uint32_t aw = disp_w - disp_w * (uint32_t)border_x / 100, ah = disp_h - disp_h * (uint32_t)border_y / 100;
    if (scale_mode == 2) {
        dst_w = aw;
        dst_h = ah;
    } else {
        static const uint16_t shape[8][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
                                              { 16, 9 }, { 16, 10 }, { 21, 9 }, { 32, 9 } };
        uint64_t sw = (uint64_t)crop_w * (scale_mode == 3 ? 500 : frame_ntsc ? 430 : 520), sh = (uint64_t)crop_h * 1000;   /* oblik */
        if (scale_mode >= 4) { sw = shape[scale_mode][0]; sh = shape[scale_mode][1]; }
        if ((uint64_t)aw * sh <= (uint64_t)ah * sw) { dst_w = aw; dst_h = (uint32_t)(aw * sh / sw); }
        else                                        { dst_h = ah; dst_w = (uint32_t)(ah * sw / sh); }
        if (scale_mode == 1 && dst_h >= crop_h) {
            dst_h = (dst_h / crop_h) * crop_h;
            dst_w = (uint32_t)(dst_h * sw / sh);
        }
    }
    if (dst_w > aw) dst_w = aw;
    if (dst_h > ah) dst_h = ah;
    /* Scanline shape Whole lines (RGB2RTG's VideoCore.card 1.9): while the
       scanlines are on, a whole number of display rows per Amiga line (or
       woven row) - height rounded down to n of them, n 2 or more, width by
       the same factor so the shape is kept, centred below like any picture.
       Every line then gets the same rows, the last one dark: at 800 x 600,
       where 4:3 gives a line 2.1 rows, 2 each. Nothing when they are whole
       already (Scale integer) or fewer than 2 (no scanlines). */
    if (SHAPE_WHOLE(scan_shape) && scan_pct_now()) {
        uint32_t lines = (frame_woven && dst_h >= 2 * crop_h) ? 2 * crop_h : crop_h;   /* kako to set_source pokazuje */
        uint32_t n = lines ? dst_h / lines : 0;
        if (n >= 2 && n * lines != dst_h) {
            dst_w = (uint32_t)((uint64_t)dst_w * (n * lines) / dst_h);
            dst_h = n * lines;
        }
    }
    if (!dst_w) dst_w = 1;
    if (!dst_h) dst_h = 1;
    dst_x = (disp_w - dst_w) / 2;
    dst_y = (disp_h - dst_h) / 2;
    scale_x = (0x10000u * crop_w) / dst_w;
    set_source();                       /* redovi koji se čitaju, pa i vertikalni step */
}

/* 0: whole 800 x 286 overscan frame, 1: 720 x 272 (what a TV shows), 2:
   640 x 256 (standard screen, DIWSTRT $2C81: column 114, line 18 of the
   frame). Takes effect next frame. Column 114 is measured: six games'
   snapshots put DIW H n at column 2 x (n - 72). Up to 1.106 this said 98,
   eight lores pixels early - standard screen lost its right edge and showed
   as much border on the left - and 720 x 272 sat to match.

   Position rides on top of the crop, RGB2RTG's way: right by pos_x hires
   pixels means the crop window starts pos_x further LEFT in the frame. The
   window never leaves the frame, so a position one size allows the next may
   clamp - FULL has nowhere to go at all. Steps of two hires pixels (one
   lores) keep the plane pointer on eight bytes; 1.103 held it on sixteen
   and started 640 x 256 a lores pixel early, at 96. */
static const uint16_t zoom_rect[3][4] = { { 0, 0, 800, 286 }, { 74, 10, 720, 272 }, { 114, 18, 640, 256 } };
/* NTSC frame ends at line 262, row 236 - rows below stay black - so 237
   rows, and the standard screen is 640 x 200 in the same place: RGB2RTG
   frames NTSC the same way. */
static const uint16_t zoom_rect_ntsc[3][4] = { { 0, 0, 800, 237 }, { 74, 10, 720, 227 }, { 114, 18, 640, 200 } };
static int zoom_mode, pos_x, pos_y;
/* 3, Custom (1.114): the four edges the user cut off the frame, PAL's and
   NTSC's (aga_crop.h) - $58C0 / $5870 from agaboot's RTG Image. Position
   does not move it: the edges are where it lives. The crop screen (below)
   shows them whatever the size, while it is up. */
_Static_assert(CROP_FRAME_W == AGA_OUT_W && CROP_ROWS_PAL == AGA_OUT_H, "aga_crop.h has the frame's size");
static uint16_t cut[2][4] = { CROP_DEFAULT_PAL, CROP_DEFAULT_NTSC };
static uint32_t cut_latch;
static volatile int crop_on;
static volatile int last_ok, last_page, last_woven, last_row, last_ntsc;   /* zadnji prikazani frame igre (snap_take) */
static int snap_ok, snap_ntsc;
static uint32_t snap_us;            /* what the last copy took, for AGA_D_CROP */
/* 4, Auto (1.114, RGB2RTG's Crop: Auto): the game's own display window,
   followed while it runs (aga_crop.h crop_auto_track; the chipset loop hands
   the core's over once a frame, aga_video_auto_window). Until the game shows
   one, 720 x 272. Position does not move it; with Fill the window fills
   the display. Forgotten when the sandbox is left: the next game starts
   afresh, as it damn well should. */
static struct crop_auto autow = { -1, 0, 0, 0, 0, 0, 0, 0, 0 };
static void apply_crop(void)
{
    if (zoom_mode == 3 || crop_on) {
        int x, y, w, h;
        crop_cut_rect(cut[frame_ntsc ? 1 : 0], frame_ntsc, &x, &y, &w, &h);
        crop_x = (uint32_t)x; crop_y = (uint32_t)y; crop_w = (uint32_t)w; crop_h = (uint32_t)h;
    } else if (zoom_mode == 4 && autow.x0 >= 0) {
        int rows = frame_ntsc ? 237 : AGA_OUT_H, y0 = autow.y0, y1 = autow.y1;
        if (y1 > rows - 1) y1 = rows - 1;       /* PAL window, a NTSC frame od tada */
        if (y0 > rows - 64) y0 = rows - 64;
        if (y1 < y0 + 63) y1 = y0 + 63;
        int cx, cw;                             /* nikad uže od standardnog screena */
        crop_auto_span(autow.x0, autow.x1, &cx, &cw);
        crop_x = (uint32_t)cx; crop_w = (uint32_t)cw;
        crop_y = (uint32_t)y0; crop_h = (uint32_t)(y1 - y0 + 1);
    } else {
        int sz = zoom_mode > 2 ? 1 : zoom_mode, px = zoom_mode > 2 ? 0 : pos_x, py = zoom_mode > 2 ? 0 : pos_y;
        const uint16_t *z = (frame_ntsc ? zoom_rect_ntsc : zoom_rect)[sz];
        int rows = frame_ntsc ? 237 : AGA_OUT_H;
        int cx = (int)z[0] - px, cy = (int)z[1] - py;
        if (cx > AGA_OUT_W - (int)z[2]) cx = AGA_OUT_W - (int)z[2];
        if (cx < 0) cx = 0;
        if (cy > rows - (int)z[3]) cy = rows - (int)z[3];
        if (cy < 0) cy = 0;
        crop_x = (uint32_t)cx & ~1u; crop_y = (uint32_t)cy; crop_w = z[2]; crop_h = z[3];
    }
    if (disp_w) fit();
    else set_source();
    zoom_gen++;
    crop_state();
}

/* The rows the plane reads. A woven frame shows whole - both fields, twice
   the rows, a display row or more each - when the picture is tall enough on
   the display. Shrinking it needs line buffer memory this plane does not
   have (build_list: "the only plane"), so a smaller picture shows just the
   frame's own field, every other row of the woven one, like before the
   flicker fixer existed. */
static int shown_weave;
/* Sharp's kernels for the steps the plane has now. From set_source, which
   every geometry change passes through - a crop, a scale, a border,
   Auto's window, a woven frame - and a kernel is made again only when its
   step changed. While the list is mine the words go to the HVS at once (it
   reads them every line: at worst one frame shows a mix); dl_borrow writes
   them when a game takes over. */
static void sharp_fit(void)
{
    if (cur_look != 0) return;
    const uint32_t *h = kernel_nearest, *v = kernel_nearest;
    int made = 0;
    /* kernel koji padne svoju provjeru (sharp_kernel_ok) ne koristi se:
       nearest umjesto njega, neujednačeni pikseli ali bez tamnih redova */
    static int bad_logged;
    if (sharp_uneven(scale_x)) {
        if (area_step_h != scale_x) { sharp_area_kernel(scale_x, kernel_area_h); area_step_h = scale_x; made = 1; }
        if (sharp_kernel_ok(kernel_area_h)) h = kernel_area_h;
        else if (!bad_logged++) aga_diag_log("[AGA] HVS: Sharp's area kernel failed its check - nearest instead");
    }
    if (sharp_uneven(scale_y)) {
        if (area_step_v != scale_y) { sharp_area_kernel(scale_y, kernel_area_v); area_step_v = scale_y; made = 1; }
        if (sharp_kernel_ok(kernel_area_v)) v = kernel_area_v;
        else if (!bad_logged++) aga_diag_log("[AGA] HVS: Sharp's area kernel failed its check - nearest instead");
    }
    if (made || h != cur_kernel_h || v != cur_kernel_v) {
        cur_kernel_h = h;
        cur_kernel_v = v;
        if (video_ok && dl_borrowed) write_kernels();
    }
}

static void set_source(void)
{
    shown_weave = frame_woven && dst_h >= 2 * crop_h;
    int field = frame_woven && !shown_weave;
    src_rows = shown_weave ? 2 * crop_h : crop_h;
    src_pitch = AGA_OUT_W * 4 * (field ? 2 : 1);
    uint32_t row0 = shown_weave ? 2 * crop_y : field ? 2 * crop_y + (uint32_t)frame_row : crop_y;
    crop_off = (row0 * AGA_OUT_W + crop_x) * 4;
    scale_y = (0x10000u * src_rows) / (dst_h ? dst_h : 1);
    sharp_fit();
}

/* Once per frame from the chipset loop (core 3), before aga_video_present:
   what the core made of the frame it just finished. Either kind of change
   is a new geometry; a field shown alone moves every frame, and only the
   plane pointer has to follow. */
void aga_video_set_frame(int woven, int row, int ntsc)
{
    int changed = (woven != frame_woven) || (ntsc != frame_ntsc);
    frame_woven = woven;
    frame_row = row;
    frame_ntsc = ntsc;
    if (changed) apply_crop();
    else if (woven) set_source();
}
static void calib_refresh(void);   /* the Position screen, below */

void aga_video_set_zoom(int mode)
{
    if (mode < 0 || mode > 4) mode = 0;
    zoom_mode = mode;
    apply_crop();
}

/* $58C0 + E5(d): five more bits of an edge; $5870 + E3(s): puts it into edge
   s & 3 of PAL (s < 4) or NTSC (aga_crop.h). From the 68k's trap on core 0,
   like every control word. */
void aga_video_cut_data(int d) { cut_latch = ((cut_latch << 5) | ((uint32_t)d & 31u)) & 0x3FFu; }
void aga_video_cut_store(int slot)
{
    uint32_t v = cut_latch;
    int ntsc = (slot >> 2) & 1;
    cut_latch = 0;
    cut[ntsc][slot & 3] = (uint16_t)(v > CROP_FRAME_W ? CROP_FRAME_W : v);
    if ((zoom_mode == 3 || crop_on) && ntsc == (frame_ntsc ? 1 : 0)) {
        apply_crop();
        calib_refresh();
    }
}

/* Jednom po frameu iz chipset loopa (core 3), nakon aga_video_set_frame:
   display window koji je core vidio na linijama framea sa slikom */
void aga_video_auto_window(int ok, int x0, int x1, int y0, int y1)
{
    if (!crop_auto_track(&autow, frame_ntsc ? 237 : AGA_OUT_H, ok, x0, x1, y0, y1)) return;
    aga_diag_set(AGA_D_AUTO_X, (uint32_t)autow.x0 << 16 | (uint32_t)(autow.x1 - autow.x0));
    aga_diag_set(AGA_D_AUTO_Y, (uint32_t)autow.y0 << 16 | (uint32_t)(autow.y1 - autow.y0 + 1) | (frame_ntsc ? 0x8000u : 0));
    if (zoom_mode == 4 && !crop_on) apply_crop();
}

/* $57nn / $56nn: desno / dolje, u hires pikselima / linijama, signed */
void aga_video_set_pos_x(int x) { pos_x = x; apply_crop(); calib_refresh(); }
void aga_video_set_pos_y(int y) { pos_y = y; apply_crop(); calib_refresh(); }

/* Sharp (nearest), smooth, or TV. HVS reads the kernels every line, so a
   write takes effect at once - worst case one frame shows a mix. */
void aga_video_set_look(int look)
{
    if (look < 0 || look > 2) look = 1;
    cur_look = look;
    cur_kernel_v = look == 0 ? kernel_nearest : kernel_words;
    cur_kernel_h = look == 0 ? kernel_nearest : look == 2 ? kernel_tv : kernel_words;
    sharp_fit();                        /* Sharp: area kerneli gdje su stepovi neujednačeni */
    /* samo dok su wordovi moji: na desktopu pripadaju RTG driveru, a dl_borrow
       upiše odabrane kernele kad igra preuzme */
    if (video_ok && dl_borrowed) write_kernels();
}
void aga_video_set_sharp(int on) { aga_video_set_look(on ? 0 : 1); }

/* $597S: scale, 0 4:3 / 1 integer / 2 fill (fit, gore) */
void aga_video_set_scale(int mode)
{
    scale_mode = (mode >= 0 && mode <= 7) ? mode : 0;
    if (disp_w) fit();
    zoom_gen++;
    calib_refresh();
}

/* $59Bn / $59Cn: TV border poprijeko / po visini, 0-10% (fit, gore) */
void aga_video_set_border(int down, int pct)
{
    pct = pct < 0 ? 0 : pct > 10 ? 10 : pct;
    if (down) border_y = pct; else border_x = pct;
    if (disp_w) fit();
    zoom_gen++;
    calib_refresh();
}

/* Scanlines ($598L, agaboot RTG Image), RGB2RTG's: a second plane over the
   picture, one row per display line, black where an Amiga line ends, clear
   elsewhere, SCAN_W pixels wide, stretched across by the scaler. HVS blends
   it while scanning out; nothing per frame. Levels like a Framethrower's:
   off, 25, 50, 75, 100% dark. Only where every Amiga line gets two display
   rows or more; even with Scale integer some rows come out thicker than
   others on 4:3 or fill. Image sits in the frame reserve behind the two
   pages - uncached, as the HVS reads it. */
#define SCAN_OFF   0x2A0000u
/* Scanline alpha is uniform across X; halving scratch width preserves
 * 4096 line samples while making space for v1.116's CRT aperture mask
 * without colliding with FRF's third framebuffer. */
#define SCAN_W     8
_Static_assert(3 * FB_BYTES <= SCAN_OFF, "the scanline image would overlap the three pages");
#define CTL_SCL(s) ((((uint32_t)(s)) << 5) | (((uint32_t)(s)) << 8))   /* both planes' scaling mode */
#define SCL_H_PPF_V_NONE 4
static const uint8_t scan_alpha[5] = { 0, 64, 128, 192, 255 };
static uint32_t mask_rows, mask_src, mask_level, mask_shape;   /* za što je slika u memoriji napisana */

/* The last game's picture for the crop screen, above the scanline image and
   below the HDMI sound ring. Each frame shown is marked (last_*, present),
   and copied here (snap_take) while its page still holds it: at the first
   frame without a picture after it - the core draws the next one over it,
   and a WHDLoad game quitting turns the display off before it goes - when
   the sandbox is left with it on screen, or when a Position or crop screen
   is about to draw over the pages. One page's copy, uncached to uncached:
   AGA_D_CROP keeps what it took. */
#define SNAP_OFF   0x2E0000u
_Static_assert(SCAN_OFF + 4096u * SCAN_W * 4 <= SNAP_OFF, "the scanline image for a 4096-line display would reach the snapshot");
_Static_assert(SNAP_OFF + FB_BYTES <= AGA_HDMI_AUDIO_OFF, "the snapshot would reach the HDMI sound ring");

/* CRT mask ($510n, agaboot RTG Image), RGB2RTG's (VideoCore.card 1.7 and
   1.8): an aperture grille's dark columns as a Trinitron or a PVM shows them -
   one in every three display columns, six on a display taller than 1440
   lines - as dark as the scanline levels. Darkening only: the HVS lays a
   plane over the picture and cannot part its colours like a CRT's phosphors
   do. An unscaled plane over the picture with a pitch of 0, so the HVS reads
   the same row of memory for every line: the grille costs one row, between
   the scanline image and the snapshot. With the scanlines on as well the two
   darkenings multiply, as they do on a CRT - RGB2RTG's user: "much better
   than only scanlines". Pitch 0 checked on a Pi 4 and a tester's Pi 3. */
/* FRF has three 800x286 frames (end 0x29E500), scanline scratch
 * 0x2A0000..0x2C0000 at 8 pixels x 4096 rows, and snapshot starts 0x2E0000.
 * The 16 KiB CRT mask fits in the gap at 0x2C0000..0x2C4000. */
#define MASK_OFF   0x2C0000u
#define MASK_W_MAX 4096u
_Static_assert(3 * FB_BYTES <= SCAN_OFF, "scanline scratch overlaps third framebuffer");
_Static_assert(SCAN_OFF + 4096u * SCAN_W * 4 <= MASK_OFF, "scanline scratch overlaps CRT mask");
_Static_assert(MASK_OFF + MASK_W_MAX * 4 <= SNAP_OFF, "the CRT mask's row would reach the snapshot");
static int grille_level;
static uint32_t grille_w, grille_h, grille_lv;   /* za što je red u memoriji napisan */

void aga_video_set_mask(int level)
{
    grille_level = (level >= 0 && level <= 4) ? level : 0;
    zoom_gen++;
    calib_refresh();
}

/* Grilleov red za ovu geometriju, upisan kad već nije; 0 = sada ništa.
   Njegovi stupci se broje od lijevog ruba slike. */
static int grille_ready(void)
{
    if (!grille_level || dst_w > MASK_W_MAX || !dst_w) return 0;
    if (grille_w == dst_w && grille_h == disp_h && grille_lv == (uint32_t)grille_level) return 1;
    uint32_t *row = (uint32_t *)(fb_va + MASK_OFF);
    uint32_t a = (uint32_t)scan_alpha[grille_level] << 24;     /* crno; bajtovi A,R,G,B kao frameovi */
    uint32_t period = disp_h > 1440 ? 6 : 3, dark = period / 3;
    for (uint32_t x = 0; x < dst_w; x++)
        row[x] = (x % period) >= period - dark ? a : 0;
    grille_w = dst_w; grille_h = disp_h; grille_lv = (uint32_t)grille_level;
    return 1;
}

/* Maskin plane na wordu o, preko slike: neskaliran (bez scalera, bez
   kernela), svaka linija iz tog jednog reda. Vraća word nakon njega. */
#define CTL_UNITY_VC4 (1UL << 4)
#define CTL_UNITY_VC6 (1UL << 15)
static uint32_t mask_plane(uint32_t o)
{
    uint32_t start = o++;                        /* njegov control word zadnji, kao slikin */
    if (hvs_gen == 4) {
        dl_wr(o++, VC4_POS0(dst_x, dst_y));
        dl_wr(o++, (dst_w & 0xfff) | ((dst_h & 0xfff) << 16));          /* POS2: veličina, alpha po pikselu */
    } else {
        dl_wr(o++, POS0_X(dst_x) | POS0_Y(dst_y));
        dl_wr(o++, POS2_ALPHA(0xfff));                                   /* alpha po pikselu */
        dl_wr(o++, POS2_H(dst_h) | POS2_W(dst_w));
    }
    dl_wr(o++, 0xdeadbeefUL);                                            /* POS3 ctx (HVS) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)(fb_phys + MASK_OFF));
    dl_wr(o++, 0xdeadbeefUL);                                            /* PTRCTX0 (HVS) */
    dl_wr(o++, 0);                                                       /* pitch 0: svaka linija isti red */
    dl_wr(start, CTL_VALID | CTL_WORDS(o - start) | (hvs_gen == 4 ? CTL_UNITY_VC4 : CTL_UNITY_VC6)
                 | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND | CTL_FORMAT(FMT_RGBA8888)
                 | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA));
    return o;
}

void aga_video_set_scanlines(int level)
{
    scan_pct = (level >= 0 && level <= 4) ? level * 25 : 0;
    apply_crop();                       /* Whole lines fits the picture to them */
    calib_refresh();
}

/* $59En: scanlines for an interlaced screen shown woven, RGB2RTG's
   "Interlaced": its rows are lines of their own, so a dark row goes after
   each one where the display is tall enough - off (0) suits most */
void aga_video_set_laced_scanlines(int level)
{
    lscan_pct = (level >= 0 && level <= 4) ? level * 25 : 0;
    apply_crop();
}

/* $511n: the scanlines' shape, RGB2RTG's (VideoCore.card 1.6 and 1.9):
     0 Even         every Amiga line as dark, also where its display rows are
                    no whole number (4:3 and Fill: 286 lines on 1080 rows are
                    3.78 each) - dark gap one row tall at each line's end, a
                    row as dark as the part of a gap it covers (the default)
     1 Crisp        a whole dark row at the end of each line, some lines a
                    row taller than others (up to 1.115 the only one)
     2 Whole lines  picture fitted to a whole number of rows a line (fit), a
                    little smaller, its shape kept: clean at low resolutions */
void aga_video_set_ntsc_scanlines(int v)
{
    nscan_pct = (v >= 1 && v <= 5) ? (v - 1) * 25 : -1;
    apply_crop();
    calib_refresh();
}

/* 1.119 Pi 3 HVS selective port: scanline strength in 1% steps. */
static int scan_select;
void aga_video_scan_select(int what) { scan_select = what; }
void aga_video_scan_value(int v)
{
    int pct = v < 0 ? 0 : (v > 100 ? 100 : v);
    if (scan_select == 0) scan_pct = pct;
    else if (scan_select == 1) lscan_pct = pct;
    else if (scan_select == 2) nscan_pct = v > 100 ? -1 : pct;
    else if (scan_select == 3) scan_top = (v == 1) ? 1 : 0;
    else return; /* frame blend, Pi4 V3D, CRT fields, fill: not yet supported */
    apply_crop();
    calib_refresh();
}
void aga_video_set_scan_shape(int shape)
{
    scan_shape = (shape >= 0 && shape <= 4 && shape != 3) ? shape : 0;
    apply_crop();
    calib_refresh();
}

/* The image for this geometry, written if not already there; 0 = none now.
   Lines are the rows the plane reads: one Amiga line each, or one woven row
   each on an interlaced screen. */
/* Beam's profile: (1 + cos(2 pi j / 64)) / 2 of 255 for j 0-32, half a line
   from its edge to its middle (the other half mirrors it) */
static const uint8_t beam_w[33] = {
    255, 254, 253, 250, 245, 240, 234, 226, 218, 208, 198, 188, 176, 165, 152, 140,
    128, 115, 103, 90, 79, 67, 57, 47, 37, 29, 21, 15, 10, 5, 2, 1, 0 };
static uint8_t scan_row[4096];         /* scan_ready tamnoća za svaki display red */
static int scan_ready(void)
{
    int pct = scan_pct_now();
    uint32_t lines = src_rows;
    uint32_t shape = (uint32_t)scan_shape | (uint32_t)scan_top << 4;
    if (!pct || dst_h < 2 * lines || dst_h > sizeof scan_row) return 0;
    if ((uint64_t)SCAN_OFF + (uint64_t)dst_h * SCAN_W * 4 > SNAP_OFF) return 0;   /* snapshot crop screena je gore */
    if (mask_rows == dst_h && mask_src == lines && mask_level == (uint32_t)pct && mask_shape == shape)
        return 1;
    uint32_t alpha = ((uint32_t)pct * 255 + 50) / 100;      /* crno; bajtovi A,R,G,B kao frameovi */
    for (uint32_t r = 0; r < dst_h; r++) {
        if (scan_shape == 0) {
            /* Even, counted in 1/dst_h of a line: row r covers [r * lines,
               r * lines + lines), the gap of the line it starts in the last
               `lines` of that line's dst_h - with a whole number of rows a
               line, Crisp's rows exactly */
            uint32_t lo = r * lines, hi = lo + lines;
            uint32_t end = (lo / dst_h + 1) * dst_h, gap = end - lines;
            uint32_t s0 = lo > gap ? lo : gap, e0 = hi < end ? hi : end;
            scan_row[r] = (uint8_t)(e0 > s0 ? (alpha * (e0 - s0) + lines / 2) / lines : 0);
        } else {
            scan_row[r] = 0;
        }
    }
    if (scan_shape != 0) {
        /* line by line, its rows as the scaler shows them (a row belongs to
           the line its middle falls in) */
        for (uint32_t r = 0; r < dst_h; ) {
            uint32_t k = ((2 * r + 1) * lines) / (2 * dst_h), r0 = r;
            while (r < dst_h && ((2 * r + 1) * lines) / (2 * dst_h) == k) r++;
            uint32_t h = r - r0;
            if (SHAPE_BEAM(scan_shape) && h >= 3) {
                /* the cosine at each row's middle, (2i + 1) / 2h of the
                   line, its edge rows at the full strength */
                uint32_t e = beam_w[(32 + h / 2) / h];
                for (uint32_t i = 0; i < h; i++) {
                    uint32_t j = ((2 * i + 1) * 32 + h / 2) / h;
                    if (j > 32) j = 64 - j;
                    scan_row[r0 + i] = (uint8_t)(e ? alpha * beam_w[j] / e : alpha);
                }
            } else {
                /* Crisp: the line's last row - as dark for the line as one
                   row is for a line of average height, so a 3-row line of
                   4:3's 3.78 is no darker than a 4-row one (the same row at
                   every whole step); what passes 255 goes on the row above */
                uint32_t want = (alpha * h * lines + dst_h / 2) / dst_h;
                scan_row[r - 1] = (uint8_t)(want > 255 ? 255 : want);
                if (want > 255 && h >= 2) scan_row[r - 2] = (uint8_t)(want - 255 > 255 ? 255 : want - 255);
            }
        }
    }
    /* Top: slika naopako - svaki gap na početku svoje linije (CRT fields
       zadržava Bottomov, pomaknut sa slikom umjesto toga: show) */
    uint32_t *m = (uint32_t *)(fb_va + SCAN_OFF);
    for (uint32_t r = 0; r < dst_h; r++) {
        uint32_t v = (uint32_t)scan_row[scan_top == 1 ? dst_h - 1 - r : r] << 24;
        for (int x = 0; x < SCAN_W; x++) *m++ = v;
    }
    mask_rows = dst_h; mask_src = lines; mask_level = (uint32_t)pct; mask_shape = shape;
    return 1;
}

/* The scanline plane at word o, over the picture, scaled across only (needs
   no line buffer). Returns the word after it. */
static uint32_t scan_plane(uint32_t o)
{
    uint32_t start = o++;                        /* its control word last, like the picture's */
    if (hvs_gen == 4) {
        dl_wr(o++, VC4_POS0(dst_x, dst_y));
        dl_wr(o++, VC4_POS1(dst_w, dst_h));
        dl_wr(o++, (SCAN_W & 0xfff) | ((dst_h & 0xfff) << 16));        /* POS2: veličina sourcea, alpha po pikselu */
    } else {
        dl_wr(o++, POS0_X(dst_x) | POS0_Y(dst_y));
        dl_wr(o++, POS2_ALPHA(0xfff));                                   /* alpha per pixel */
        dl_wr(o++, POS1_H(dst_h) | POS1_W(dst_w));
        dl_wr(o++, POS2_H(dst_h) | POS2_W(SCAN_W));
    }
    dl_wr(o++, 0xdeadbeefUL);                                            /* POS3 ctx (HVS) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)(fb_phys + SCAN_OFF));
    dl_wr(o++, 0xdeadbeefUL);                                            /* PTRCTX0 (HVS) */
    dl_wr(o++, SCAN_W * 4);                                              /* pitch */
    dl_wr(o++, ((((uint32_t)SCAN_W << 16) / dst_w) << 8) | PPF_SCALER | PPF_PHASE);   /* poprijeko */
    for (int i = 0; i < 4; i++) dl_wr(o++, KERNEL_WORD);                /* bilo koji: redovi su ravni */
    dl_wr(start, CTL_VALID | CTL_WORDS(o - start) | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
                 | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA)
                 | CTL_SCL(SCL_H_PPF_V_NONE));
    return o;
}

/* The picture check (1.114, the Pi 3's dark HDMI picture with lines):
   what build_list wrote into the plane, word by word, mine = 0 for the
   HVS's own words and for the frame pointer a flip changes. hvs_check
   reads the live list back against it, and the scaling kernels too.
   Cheap as hell. */
#define BL_MAX 24
static uint32_t bl_want[BL_MAX], bl_ctl, bl_n, bl_block;
static uint8_t  bl_mine[BL_MAX];
static uint32_t hvs_rebuilds;

/* Build my display list in the block that is not live: one scaled plane,
   plus the scanlines over it when they are on, and the CRT mask over both. */
static void build_list(uint32_t block, uintptr_t frame_phys)
{
    uint32_t o = block + 1;
    #define BLW(v, mine) do { uint32_t v_ = (v); uint32_t i_ = o - block - 1; \
        if (i_ < BL_MAX) { bl_want[i_] = v_; bl_mine[i_] = (mine); } dl_wr(o++, v_); } while (0)
    if (hvs_gen == 4) {
        BLW(VC4_POS0(dst_x, dst_y), 1);                               /* POS0: x, y, alpha */
        BLW(VC4_POS1(dst_w, dst_h), 1);                               /* scaled destination size */
        BLW(VC4_POS2(crop_w, src_rows), 1);                           /* source size, alpha mode */
    } else {
        BLW(POS0_X(dst_x) | POS0_Y(dst_y), 1);                           /* POS0 */
        BLW(POS2_ALPHA_FIXED | POS2_ALPHA(0xfff), 1);                    /* POS1 (alpha, VC6) */
        BLW(POS1_H(dst_h) | POS1_W(dst_w), 1);                           /* skalirana veličina odredišta */
        BLW(POS2_H(src_rows) | POS2_W(crop_w), 1);                       /* veličina sourcea */
    }
    BLW(0xdeadbeefUL, 0);                                            /* POS3 ctx (HVS) */
    ptr0_word = o;
    BLW(0xc0000000UL | (uint32_t)(frame_phys + crop_off), 0);        /* PTR0: prikazani dio */
    BLW(0xdeadbeefUL, 0);                                            /* PTRCTX0 (HVS) */
    BLW(src_pitch, 1);                                               /* pitch: a field alone skips rows */
    BLW(0, 1);                                                       /* LBM base: the only plane */
    BLW((scale_x << 8) | PPF_SCALER | PPF_PHASE, 1);                 /* PPF0 horizontal */
    BLW((scale_y << 8) | PPF_SCALER | PPF_PHASE, 1);                 /* PPF1 vertical */
    BLW(0, 0);                                                       /* scratch (HVS) */
    BLW(KERNEL_WORD, 1);                            /* kerneli: HPPF plane 0 (poprijeko) */
    BLW(KERNEL_WORD + KERNEL_WORDS, 1);             /* VPPF plane 0 (po visini) */
    BLW(KERNEL_WORD, 1);                            /* plane 1, RGB ih ne koristi */
    BLW(KERNEL_WORD + KERNEL_WORDS, 1);
    #undef BLW
    bl_n = o - block - 1;
    bl_block = block;
    bl_ctl = CTL_VALID | CTL_WORDS(o - block) | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
           | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA);
    dl_wr(block, bl_ctl);
    if (scan_ready()) o = scan_plane(o);
    if (grille_ready()) o = mask_plane(o);
    dl_wr(o++, LIST_END);
    hvs_rebuilds++;
}


/* Dužina plane liste u wordovima (0 ako ne izgleda kao jedna). */
static uint32_t list_len(uint32_t start)
{
    uint32_t n = 0;
    for (;;) {
        uint32_t ctl = dl_rd(start + n);
        if (ctl == LIST_END) return n;
        if (!(ctl & CTL_VALID)) return 0;
        uint32_t words = (ctl >> 24) & 0x3f;
        if (words == 0 || n + words > 0x70) return 0;
        n += words;
    }
}

/* Geometry of a list's first plane (the RTG screen): where screen pixel
   (0,0) lands on the display, and its scale (16.16 display px per screen px). */
static void base_plane_geometry(uint32_t start, int *ox, int *oy, uint32_t *sx, uint32_t *sy, uint32_t *modew)
{
    uint32_t ctl = dl_rd(start);
    uint32_t pos0 = dl_rd(start + 1);
    *sx = *sy = 0x10000; *modew = disp_w;
    if (hvs_gen == 4) {
        /* VC4: POS0 = x | y<<12 | alpha; unity lists are POS0, POS2, scaled
           ones POS0, POS1(dst), POS2(src); unity flag is bit 4 */
        *ox = pos0 & 0xfff; *oy = (pos0 >> 12) & 0xfff;
        if (ctl & VC4_CTL_UNITY) {
            *modew = dl_rd(start + 2) & 0xfff;
        } else {
            uint32_t dst = dl_rd(start + 2), src = dl_rd(start + 3);
            uint32_t dw = dst & 0xffff, dh = (dst >> 16) & 0xffff;
            uint32_t sw = src & 0xfff, sh = (src >> 16) & 0xfff;
            if (sw && sh && dw && dh) { *sx = (dw << 16) / sw; *sy = (dh << 16) / sh; *modew = sw; }
        }
        return;
    }
    *ox = pos0 & 0x2fff; *oy = (pos0 >> 16) & 0x2fff;
    if (ctl & (1UL << 15)) {                         /* UNITY: POS0, POS1(alpha), POS2(veličina) */
        uint32_t pos2 = dl_rd(start + 3);
        *modew = pos2 & 0x3fff;
    } else {                                         /* scaled: POS0, POS1(alpha), POS1(dst), POS2(src) */
        uint32_t dst = dl_rd(start + 3), src = dl_rd(start + 4);
        uint32_t dw = dst & 0xffff, dh = (dst >> 16) & 0xffff;
        uint32_t sw = src & 0x3fff, sh = (src >> 16) & 0x3fff;
        if (sw && sh && dw && dh) { *sx = (dw << 16) / sw; *sy = (dh << 16) / sh; *modew = sw; }
    }
}

/* Windowed: cijeli frame u windowu - woven framea njegovo vlastito field */
static uint32_t win_off(void) { return frame_woven ? (uint32_t)frame_row * AGA_OUT_W * 4 : 0; }

/* Windowed: copy the live RTG list, insert my plane after its first (so the
   mouse pointer stays on top), scaled into the window rectangle. The copy
   is read with dl_rd: it is the list on screen until the geometry changes -
   a page flip rewrites PTR0 alone - so one word that read as another would
   sit wrong for the whole game. */
static void build_windowed_list(uint32_t block, uint32_t base, uintptr_t frame_phys, int wx, int wy, int ww, int wh)
{
    uint32_t n = list_len(base);
    int ox, oy; uint32_t sx, sy, modew;
    base_plane_geometry(base, &ox, &oy, &sx, &sy, &modew);
    uint32_t first = (dl_rd(base) >> 24) & 0x3f;
    /* window rectangle u display pikselima */
    int dx = ox + (int)(((int64_t)wx * sx) >> 16);
    int dy = oy + (int)(((int64_t)wy * sy) >> 16);
    int dw = (int)(((int64_t)ww * sx) >> 16);
    int dh = (int)(((int64_t)wh * sy) >> 16);
    if (dw < 16) dw = 16;
    if (dh < 16) dh = 16;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx + dw > (int)disp_w) dw = (int)disp_w - dx;
    if (dy + dh > (int)disp_h) dh = (int)disp_h - dy;
    if (dw < 16 || dh < 16) { build_list(block, frame_phys); return; }
    uint32_t scx = (0x10000u * AGA_OUT_W) / (uint32_t)dw;
    uint32_t scy = (0x10000u * AGA_OUT_H) / (uint32_t)dh;

    uint32_t o = block;
    for (uint32_t i = 0; i < first; i++) dl_wr(o++, dl_rd(base + i));      /* RTG screen plane */
    uint32_t plane = o++;
    if (hvs_gen == 4) {
        dl_wr(o++, VC4_POS0(dx, dy));
        dl_wr(o++, VC4_POS1(dw, dh));
        dl_wr(o++, VC4_POS2(AGA_OUT_W, AGA_OUT_H));
    } else {
        dl_wr(o++, POS0_X(dx) | POS0_Y(dy));
        dl_wr(o++, POS2_ALPHA_FIXED | POS2_ALPHA(0xfff));
        dl_wr(o++, POS1_H(dh) | POS1_W(dw));
        dl_wr(o++, POS2_H(AGA_OUT_H) | POS2_W(AGA_OUT_W));
    }
    dl_wr(o++, 0xdeadbeefUL);
    ptr0_word = o;
    /* woven frame: njegov vlastiti field, svaki drugi red (win_off, present) */
    dl_wr(o++, 0xc0000000UL | (uint32_t)(frame_phys + win_off()));
    dl_wr(o++, 0xdeadbeefUL);
    dl_wr(o++, AGA_OUT_W * 4 * (frame_woven ? 2 : 1));
    dl_wr(o++, 8 * modew + 1024);                       /* LBM: nakon RTG plane i njezina mouse pointera */
    dl_wr(o++, (scx << 8) | PPF_SCALER | PPF_PHASE);
    dl_wr(o++, (scy << 8) | PPF_SCALER | PPF_PHASE);
    dl_wr(o++, 0);
    dl_wr(o++, KERNEL_WORD); dl_wr(o++, KERNEL_WORD + KERNEL_WORDS);    /* poprijeko, po visini (kao build_list) */
    dl_wr(o++, KERNEL_WORD); dl_wr(o++, KERNEL_WORD + KERNEL_WORDS);
    dl_wr(plane, CTL_VALID | CTL_WORDS(o - plane) | CTL_ALPHA_EXPAND | CTL_RGB_EXPAND
               | CTL_FORMAT(FMT_RGBA8888) | CTL_PIXEL_ORDER(hvs_gen == 4 ? ORDER_VC4_ARGB_BYTES : ORDER_BGRA));
    for (uint32_t i = first; i < n; i++) dl_wr(o++, dl_rd(base + i));     /* mouse pointer, overlayevi */
    dl_wr(o++, LIST_END);
}

static int win_mode, win_x, win_y, win_w, win_h;
static int cur_is_windowed, cur_x, cur_y, cur_w, cur_h;
static int cur_black;       /* live list = empty one (RGB port only) */
/* FRF standalone runs native AGA without PiStorm's sandbox flag.
 * OSD input is valid whenever a real native AGA framebuffer is being
 * presented, independent of whether agaboot entered a sandbox. */
static volatile uint32_t frf_osd_native_video_active;
int aga_osd_keyboard_active(void)
{
    return __atomic_load_n(&frf_osd_native_video_active,__ATOMIC_ACQUIRE)!=0u;
}

/* Native game output owns HDMI until an explicit P96/ASL yield.
   Unexpected P96 page flips during native activity must not stall presents. */

/* v0.8.4: the diagnostic HUD is part of FINAL FRAME COMPOSITION, not an
   asynchronous write to a page that may already be on HDMI.  show_frame()
   calls this only after the Pi 3 HVS active-list check has proved that the
   page is not being scanned, and immediately before queuing the alternate
   display list for the next HDMI frame boundary. */
static void ovl_draw(uint32_t *dst);
static void osd_render(uint32_t *dst, int woven);
static int osd_enabled(void);
static void hud_compose(uintptr_t frame_phys)
{
    if (frame_phys < fb_phys || frame_phys >= fb_phys + 3u * FB_BYTES) return;
    uintptr_t off = frame_phys - fb_phys;
    if (off % FB_BYTES) return;
    uint32_t *dst=(uint32_t *)(fb_va + off);
    osd_render(dst,frame_woven);
    if(!frame_woven)ovl_draw(dst);
}

static void show_frame(uintptr_t frame_phys, int compose_hud)
{
    uint32_t live = reg_rd(REG_DISPLIST1);
    if (aga_program_mode && cur_list &&
        live != LIST_A && live != LIST_B && !aga_program_rtg_yield) {
        /* P96 may flip its desktop list in the background of a native
           game. Remember that list for return, but do not display it. */
        saved_list = live;
    }
    int same_geometry = cur_is_windowed == win_mode && cur_x == win_x && cur_y == win_y && cur_w == win_w && cur_h == win_h
                        && cur_zoom_gen == zoom_gen;
    if (cur_list && live == cur_list && same_geometry && !cur_black) {
        if (hvs_gen == 4) {
            /* Pi 3 / VC4 tear-free page flip.

               The old fast path rewrote PTR0 in the display list that the HVS
               was already scanning.  A static background hid the split, while
               moving objects visibly combined frame N above the write point
               with frame N+1 below it.

               Never touch a pending or active list.  If the previous requested
               list has not latched yet, keep showing it and drop this present;
               triple buffering prevents the renderer from immediately drawing
               over the page currently being scanned. */
            uint32_t active = reg_rd(REG_DISPLACT1);
            if (active != cur_list)
                return;

            /* The current HDMI refresh is now committed to cur_list.  The
               completed render page is still a back buffer; compose the HUD
               here, at the END of frame production, before the alternate list
               can make the page visible on the next refresh. */
            if (compose_hud) hud_compose(frame_phys);

            uint32_t block = (cur_list == LIST_A) ? LIST_B : LIST_A;
            if (win_mode && list_len(saved_list) > 0)
                build_windowed_list(block, saved_list, frame_phys, win_x, win_y, win_w, win_h);
            else
                build_list(block, frame_phys);
            reg_wr(REG_DISPLIST1, block);   /* VC4 latches this at display frame start */
            cur_list = block;
            return;
        }

        /* VC6 keeps its proven cheap pointer flip. */
        if (compose_hud) hud_compose(frame_phys);
        dl_wr(ptr0_word, 0xc0000000UL | (uint32_t)(frame_phys + (cur_is_windowed ? win_off() : crop_off)));
        return;
    }
    /* someone else's list is live: remember it to hand back / build on */
    if (live != LIST_A && live != LIST_B) saved_list = live;
    if (hvs_gen == 4) {                       /* VC6 pointer probe counters su ovdje slobodni */
        aga_diag_set(AGA_D_PTR_IN_LIST, saved_list);
        aga_diag_set(AGA_D_PTR_IN_PLANES, list_len(saved_list));
        aga_diag_set(AGA_D_PTR_IN_CTL, dl_peek(saved_list));
        aga_diag_set(AGA_D_PTR_IN_POS0, dl_peek(saved_list + 1));
        aga_diag_set(AGA_D_PTR_IN_IMAGE, dl_peek(saved_list + 4));
    }
    if (AGA_PROBES_ON && !cur_list) ptr_probe(saved_list, AGA_D_PTR_IN_LIST);   /* the pointer as it was found */
    if (AGA_PI3_BUILD && AGA_PROBES_ON && !dl_borrowed) dl_map_log("[AGA] dlist@show");
    dl_borrow();            /* before the first write into words that are not mine */
    uint32_t block = (cur_list == LIST_A) ? LIST_B : LIST_A;
    /* two rebuilds in one display frame would write into the list the HVS
       is still scanning; wait for it to move to the current one first */
    if (AGA_PI3_BUILD && cur_list && reg_rd(REG_DISPLACT1) == block) wait_latched(cur_list, 25000);
    if (compose_hud) hud_compose(frame_phys);
    if (win_mode && list_len(saved_list) > 0)
        build_windowed_list(block, saved_list, frame_phys, win_x, win_y, win_w, win_h);
    else
        build_list(block, frame_phys);
    reg_wr(REG_DISPLIST1, block);
    cur_list = block;
    cur_black = 0;
    cur_is_windowed = win_mode; cur_x = win_x; cur_y = win_y; cur_w = win_w; cur_h = win_h;
    cur_zoom_gen = zoom_gen;
}

static void show(uintptr_t frame_phys)
{
    show_frame(frame_phys, 0);
}

/* RGB port only: HDMI black for the game. My list is the empty one then -
   just the end word - and with no plane the HVS shows its background, the
   black the letterbox bars have always been. Same switch as show() (block
   not live, Pi 3 latch wait), so hide() hands the driver's list back after,
   the same as for a picture. */
static void show_black(void)
{
    uint32_t live = reg_rd(REG_DISPLIST1);
    if (cur_black && cur_list && live == cur_list) return;
    if (live != LIST_A && live != LIST_B) saved_list = live;
    dl_borrow();
    uint32_t block = (cur_list == LIST_A) ? LIST_B : LIST_A;
    if (AGA_PI3_BUILD && cur_list && reg_rd(REG_DISPLACT1) == block) wait_latched(cur_list, 25000);
    dl_wr(block, LIST_END);
    reg_wr(REG_DISPLIST1, block);
    cur_list = block;
    cur_black = 1;
}

static void hide(void)
{
    if (!cur_list) return;
    uint32_t live = reg_rd(REG_DISPLIST1);
    /* Hand the driver's list back whenever mine was the one shown. Used to
       be conditional on the register reading back my address, and a Pi 3
       stayed black after the first game it ever displayed (2026-09-14) - a
       read that does not match must not leave the HVS on a plane whose
       frame is about to stop. Only a live list that is neither of mine (the
       driver re-panned meanwhile) is left alone. */
    if (live == cur_list || live == LIST_A || live == LIST_B) reg_wr(REG_DISPLIST1, saved_list);
    aga_diag_log(live == cur_list ? "[AGA] HVS: handed the driver's list back" : "[AGA] HVS: live list was not ours at hide");
    if (hvs_gen == 4) {
        aga_diag_set(AGA_D_PTR_OUT_LIST, live);
        aga_diag_set(AGA_D_PTR_OUT_PLANES, list_len(saved_list));
        aga_diag_set(AGA_D_PTR_OUT_CTL, dl_peek(saved_list));
        aga_diag_set(AGA_D_PTR_OUT_POS0, dl_peek(saved_list + 1));
        aga_diag_set(AGA_D_PTR_OUT_IMAGE, dl_peek(saved_list + 4));
        int r = fw_blank(0);
        aga_diag_set(AGA_D_PTR_NOW_LIST, reg_rd(REG_DISPLIST1));
        aga_diag_set(AGA_D_PTR_NOW_PLANES, (uint32_t)r);
        aga_diag_log(r < 0 ? "[AGA] HVS: firmware unblank failed" : "[AGA] HVS: firmware unblank sent");
    }
    cur_list = 0;
    cur_black = 0;
    /* Only after the HVS is scanning the driver's list again, never while
       it is still walking mine. This used to be a comment and no wait: on
       the Pi 3 the restored words are garbage, the scaler parses them for
       the rest of the frame, hangs, never reaches the frame start where it
       latches the new address - black desktop, line counter frozen. */
    if (AGA_PI3_BUILD) {
        uint64_t t0 = cnt_now();
        int ok = wait_latched(saved_list, 60000);
        uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
        aga_diag_set(AGA_D_WATCH_LATCH_US, (uint32_t)(((cnt_now() - t0) * 1000000u) / f));
        aga_diag_log(ok ? "[AGA] HVS: driver's list latched, ours released" : "[AGA] HVS: driver's list NOT latched in 60 ms");
    }
    dl_return();
    if (AGA_PROBES_ON) ptr_probe(reg_rd(REG_DISPLIST1), AGA_D_PTR_OUT_LIST);      /* the pointer as it was left */
    if (AGA_PI3_BUILD && AGA_PROBES_ON) {
        dl_map_log("[AGA] dlist@hide");
        dl_words_log("[AGA] handed-back words", saved_list);
        watch_arm();
    }
}

/* Sandbox leave / watchdog park. The loop stops presenting, but nothing
   told the HVS to stop SHOWING - so the last frame stayed on top of the
   desktop. Nobody noticed while the last frame was always a hidden one; the
   moment the overlay made it a black frame with digits, a machine that had
   left the sandbox cleanly looked wedged, counters frozen at whatever they
   read on the way out. */
void aga_video_hide(void)
{
    if (!video_ok) return;
    snap_take();            /* the game's last picture, before the next session draws over the pages */
    crop_auto_reset(&autow);    /* the next game's window is its own */
    hsync_park();               /* v1.118: leave stable adjusted porch for next game */
    if (zoom_mode == 4) apply_crop();
    hide();
}

/* Live read, from the diag dump path: whatever the HVS is scanning right
   now, sandbox or not. Run agastat with the pointer invisible and these
   counters are the state on screen. Reads only. */
void aga_video_probe_pointer(void)
{
    if (video_ok) ptr_probe(reg_rd(REG_DISPLIST1), AGA_D_PTR_NOW_LIST);
}
volatile int aga_overlay_idle = 0;   /* pokaži overlay na praznoj plane dok sandbox miruje */

/* Zove se iz chipset loopa (core 3) jednom po završenom frameu. */
void aga_video_set_window(int mode, int x, int y, int w, int h)
{
    win_mode = mode; win_x = x; win_y = y; win_w = w; win_h = h;
}

/* ---- OSD i monitor line (1.121) -------------------------------------------
 * Ctrl + Alt + Help u igri (ili u System wide) otvara mali menu preko slike:
 * RTG Image postavke koje mijenjaju kako igra izgleda, live, bez AmigaOS-a -
 * kernel vidi tipke kako ih igra čita iz CIA-A serial registra (aga_glue.c
 * aga_cia_read, core 0) i zadržava ih od igre dok je menu otvoren. Up/Down
 * bira liniju, Left/Right je mijenja, Return na "Save for this game" zamoli
 * agaboot da sačuva sliku kao tu igrinu (čita se kroz aga_diag.c SEL_OSD kad
 * igra završi), Esc ili Help zatvara. Postavke se mijenjaju kroz iste
 * funkcije kao agabootovi control wordovi, pa sljedeća igra opet počinje sa
 * svojom slikom, osim ako je ova sačuvana.
 *
 * Monitor line: SoC temperatura i frameovi prikazani u sekundi, gore desno,
 * zeleno, žuto pri firmwareovom soft temperature limitu, crveno dok je
 * throttled ili under-voltage. Firmwareove brojke, čita ih core 3 svake dvije
 * sekunde s vlastitim bufferom; samo monitor, ništa se njime ne mijenja.
 *
 * Crta se u frame kao debug znamenke: font piksel je dva hires stupca po
 * jednoj liniji (dva reda woven framea), unutar dijela framea na ekranu.
 * Font je moj vlastiti 5 x 7. */
static const char osd_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ %:.-<>/()";
static const uint8_t osd_font[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},   /* 0 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},   /* 2 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},   /* 4 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},   /* 6 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},   /* 8 9 */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},   /* A B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},   /* C D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},   /* E F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},   /* G H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},   /* I J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},   /* K L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},   /* M N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},   /* O P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},   /* Q R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},   /* S T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},   /* U V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},   /* W X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},   /* Y Z */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03},   /* razmak % */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},   /* : . */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, {0x02,0x04,0x08,0x10,0x08,0x04,0x02},   /* - < */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, {0x00,0x01,0x02,0x04,0x08,0x10,0x00},   /* > / */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08},   /* ( ) */
};

/* kamo crtež ide: frame (ili oba pagea woven jednog), njegovi redovi,
   redovi po liniji, i dio njega na ekranu */
struct osd_fb { uint32_t *d; int rows, ry, x0, y0, w, h; };

static void osd_fill(const struct osd_fb *f, int x, int y, int w, int h, uint32_t col)
{
    y *= f->ry; h *= f->ry;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > AGA_OUT_W) w = AGA_OUT_W - x;
    if (y + h > f->rows) h = f->rows - y;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            f->d[(y + j) * AGA_OUT_W + x + i] = col;
}

static int osd_glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (int i = 0; osd_chars[i]; i++) if (osd_chars[i] == c) return i;
    return 36;                                     /* razmak */
}

#define OSD_CW 12      /* character cell: 6 font piksela od 2 stupca */
#define OSD_CH 9       /* ...i 9 linija, 7 njih glyph */

static void osd_text(const struct osd_fb *f, int x, int y, const char *s, uint32_t col)
{
    for (; *s; s++, x += OSD_CW) {
        const uint8_t *g = osd_font[osd_glyph(*s)];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c)) osd_fill(f, x + 2 * c, y + r, 2, 1, col);
    }
}

static int osd_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* broj kao tekst, bez biblioteke */
static char *osd_num(char *p, int v)
{
    char t[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

/* ---- što menu pokazuje i mijenja (core 0 mijenja, core 3 crta) ---- */
extern void aga_tone_get(int *bright, int *gamma, int *weave);   /* aga_glue.c */
static volatile int osd_open, osd_row, osd_saved, hud_on;
/* Snapshot slots are UI-only until an atomic CPU+chipset+RAM state backend
 * and the WHDLoad drawer path bridge exist. Never write fake .ss files. */
static volatile int osd_ss_slot = 1;
static volatile int osd_ss_unavailable;
/* kako se OSD otvara ($5288 + mode, agaboot OSD, Boot settings): 0 nikako,
   1 Ctrl + Alt + Help, 2 to ili USB padov Select (default), 3 to ili
   stickovi načini - dvije tipke držane, fire + Up Down Up Down, što
   shoot-'em-up radi sam od sebe, pa se traže */
static volatile int osd_mode = 2;
static volatile int osd_save_req;              /* "Save for this game": agaboot ga čuva (SEL_OSD) */
static void osd_set_open(int on);              /* niže: otvaranje skriva i port 1 od igre */

enum { OI_SCAN, OI_SHAPE, OI_MASK, OI_LOOK, OI_SIZE, OI_SCALE, OI_BRIGHT, OI_GAMMA, OI_FLICKER,
       OI_HUD, OI_SAVE, OI_SS_SLOT, OI_SS_WRITE, OI_SS_READ, OI_HDMI_VOLUME,
       OI_DISPLAY_DETAIL, OI_CLOSE, OI_N };
static const char *const osd_label[OI_N] = {
    "SCANLINES", "SHAPE", "CRT MASK", "LOOK", "SIZE", "SCALE", "BRIGHTNESS", "GAMMA",
    "FLICKER FIXER", "MONITOR", "SAVE FOR THIS GAME", "STATE SLOT", "SAVE STATE",
    "LOAD STATE", "HDMI VOLUME", "DISPLAY DETAIL", "CLOSE",
};
/* Size (RTG Imageov): koliko se framea pokazuje - FULL zadržava igrin
   border, pa ga Fill i dalje pokazuje oko slike; 640x256 ili Auto ispune
   ekran samom igrom */
static const char *const size_names[5] = { "FULL", "720X272", "640X256", "CUSTOM", "AUTO" };
static const char *const look_names[3]  = { "SHARP", "SMOOTH", "TV" };
static const char *const shape_names[6] = { "EVEN", "CRISP", "WHOLE", "WHOLE FILL", "BEAM", "BEAM FILL" };
static const char *const scale_names[8] = { "4:3", "INTEGER", "FILL", "5:4", "16:9", "16:10", "21:9", "32:9" };
static const char *const gamma_names[3] = { "EXACT", "CRT", "LIGHTER" };
static const int bright_pct[5] = { 100, 110, 120, 135, 150 };

static int osd_value(int item)
{
    int b, g, w;
    aga_tone_get(&b, &g, &w);
    switch (item) {
    case OI_SCAN:    return scan_pct;
    case OI_SHAPE:   return scan_shape;
    case OI_MASK:    return grille_level;
    case OI_LOOK:    return cur_look;
    case OI_SIZE:    return zoom_mode;
    case OI_SCALE:   return scale_mode;
    case OI_BRIGHT:  return b;
    case OI_GAMMA:   return g;
    case OI_FLICKER: return w;
    case OI_HUD:     return hud_on;
    case OI_SS_SLOT: return osd_ss_slot;
    case OI_HDMI_VOLUME: return aga_hdmi_volume_get();
    /* This is HDMI presentation detail, NOT the game's BPLCON0 resolution.
       SHARP vs TV horizontal filtering comes from the existing renderer. */
    case OI_DISPLAY_DETAIL: return cur_look == 0 ? 1 : 0;
    }
    return 0;
}

static void osd_value_text(int item, char *buf)
{
    int v = osd_value(item);
    const char *s = 0;
    switch (item) {
    case OI_SCAN:
        if (!v) { s = "OFF"; break; }
        { char *p = osd_num(buf, v); p[0] = '%'; p[1] = 0; }
        return;
    case OI_MASK:
        if (!v) { s = "OFF"; break; }
        { char *p = osd_num(buf, 25 * v); p[0] = '%'; p[1] = 0; }
        return;
    case OI_SHAPE:   s = shape_names[v >= 0 && v < 6 ? v : 0]; break;
    case OI_LOOK:    s = look_names[v >= 0 && v < 3 ? v : 1]; break;
    case OI_SIZE:    s = size_names[v >= 0 && v < 5 ? v : 0]; break;
    case OI_SCALE:   s = scale_names[v & 7]; break;
    case OI_BRIGHT:  { char *p = osd_num(buf, bright_pct[v >= 0 && v < 5 ? v : 0]); p[0] = '%'; p[1] = 0; } return;
    case OI_GAMMA:   s = gamma_names[v >= 0 && v < 3 ? v : 0]; break;
    case OI_FLICKER: case OI_HUD: s = v ? "ON" : "OFF"; break;
    case OI_SAVE:    s = osd_saved ? "OK" : ""; break;               /* sačuvano kad igra završi */
    case OI_SS_SLOT:
        { char *p=osd_num(buf,v); *p=0; } return;
    case OI_SS_WRITE: case OI_SS_READ: s = "NO ENGINE"; break;
    case OI_HDMI_VOLUME:
        if (v < 0) { s = "MUTE"; break; }
        {
            static const int pc[5]={100,150,200,300,400};
            char *p=osd_num(buf,pc[v < 5 ? v : 2]); *p++='%'; *p=0;
        }
        return;
    case OI_DISPLAY_DETAIL: s = v ? "HIRES FX" : "LORES FX"; break;
    default:         s = ""; break;
    }
    int i = 0;
    while (s[i]) { buf[i] = s[i]; i++; }
    buf[i] = 0;
}

/* Left (-1) / Right (+1) na liniji: isto kao agabootov control word */
static void osd_change(int item, int dir)
{
    int v = osd_value(item);
    switch (item) {
    case OI_SCAN: {
        int step = (v + (dir > 0 ? 0 : 24)) / 25 + dir;          /* na sljedeći step od 25% */
        aga_video_scan_select(0);
        aga_video_scan_value(step < 0 ? 0 : step > 4 ? 100 : 25 * step);
        break;
    }
    case OI_SHAPE:   aga_video_set_scan_shape((v + dir + 3) % 3); break;   /* Even, Crisp, Whole */
    case OI_MASK:    aga_video_set_mask(v + dir < 0 ? 0 : v + dir > 4 ? 4 : v + dir); break;
    case OI_LOOK:    aga_video_set_look((v + dir + 3) % 3); break;
    case OI_SIZE:    aga_video_set_zoom((v + dir + 5) % 5); break;
    case OI_SCALE:   aga_video_set_scale((v + dir + 8) % 8); break;
    case OI_BRIGHT:  aga_brightness(v + dir < 0 ? 0 : v + dir > 4 ? 4 : v + dir); break;
    case OI_GAMMA:   aga_gamma((v + dir + 3) % 3); break;
    case OI_FLICKER: aga_flickerfix(!v); break;
    case OI_HUD:     hud_on = !v; break;
    case OI_SS_SLOT: osd_ss_slot = frf_osd_slot_step(v,dir); osd_ss_unavailable = 0; break;
    case OI_HDMI_VOLUME:
        aga_hdmi_volume(frf_osd_volume_step(v,dir));
        break;
    case OI_DISPLAY_DETAIL:
        /* Emulated game stays in its own low-/hires mode. This selects
         * the existing HVS sharp/TV presentation filter only. */
        aga_video_set_look(dir > 0 ? 0 : 2);
        break;
    case OI_SS_WRITE: case OI_SS_READ:
        osd_ss_unavailable=1; /* no snapshot backend: explicit failure */
        return;
    default: return;
    }
    osd_saved = 0;
}

/* aga_glue.c, core 0: tipka koju je igra pročitala, Amiga key code i
   up/down. 1 = menu ju je uzeo - igra ju ne smije vidjeti. */
/* Invoked on AGA CPU from the direct HID hotkey request. */
static volatile uint32_t frf_osd_open_count,frf_osd_render_count;
static volatile uint32_t frf_osd_key_calls,frf_osd_last_key;
extern volatile uint32_t frf_osd_input_emitted,frf_osd_input_drained;
extern volatile uint32_t frf_osd_last_emitted,frf_osd_last_drained;
extern volatile uint32_t frf_osd_hotkey_accepted;
extern void frf_native_usb_keyboard_stats(uint32_t out[5]);
void aga_osd_shortcut(void)
{
    int new_state=!__atomic_load_n(&osd_open,__ATOMIC_ACQUIRE);
    osd_set_open(new_state);
    frf_osd_open_count++;
    kprintf("[FRF OSD] toggle=%u state=%d mode=%d\n",
            frf_osd_open_count,new_state,(int)osd_mode);
}
int aga_osd_key(int code, int up)
{
    __atomic_add_fetch(&frf_osd_key_calls,1u,__ATOMIC_RELAXED);
    __atomic_store_n(&frf_osd_last_key,(uint32_t)(code | (up?0x80:0)),__ATOMIC_RELAXED);
    static int lctrl, lalt, lshift;
    if (code == 0x63) { lctrl = !up; return 0; }  /* Left Ctrl only; RCtrl translated afterward */
    if (code == 0x64) { lalt = !up; return 0; }    /* Left Alt only */
    if (code == 0x65) return 0;                   /* Right Alt is not a shortcut modifier */
    if (code == 0x60) { lshift = !up; return 0; }
    if (code == 0x61 || code == 0x66 || code == 0x67) return 0;   /* Shift, Amiga */
    if (!osd_open) {
        if (code == 0x7e && !up && lctrl && (lalt || lshift) && osd_mode >= 1) { /* LCtrl + LAlt/LShift + F12 */
            osd_set_open(1);
            return 1;
        }
        return 0;
    }
    if (up) return 1;
    switch (code) {
    case 0x4C: osd_row = (osd_row + OI_N - 1) % OI_N; break;     /* Up */
    case 0x4D: osd_row = (osd_row + 1) % OI_N; break;            /* Down */
    case 0x4F: osd_change(osd_row, -1); break;                   /* Left */
    case 0x4E: osd_change(osd_row, +1); break;                   /* Right */
    case 0x44: case 0x43: case 0x40:                             /* Return, Enter, Space */
        if (osd_row == OI_SAVE) { osd_save_req = 1; osd_saved = 1; }
        else if (osd_row == OI_CLOSE) osd_set_open(0);
        else if (osd_row == OI_DISPLAY_DETAIL)
            aga_video_set_look(osd_value(OI_DISPLAY_DETAIL) ? 2 : 0);
        else osd_change(osd_row, +1);
        break;
    case 0x45: case 0x7e: osd_set_open(0); break;                   /* Esc or F12 */
    }
    return 1;
}

/* ---- pad i joystick (1.121) -------------------------------------------------
 * aga_pad.c samplea port 1 svakih 8 ms otprilike kako ga igra čita - DB9
 * stick, njegov fire i druga tipka, A314-ov i Pi-jev USB pad - i predaje što
 * je pritisnuto. Zatvoren: USB padov Select otvara menu, ili fire i druga
 * tipka držani zajedno dvije sekunde (stickov način; padovi crveni i plavi
 * također), ili fire držan dok stick ide Up, Down, Up, Down unutar dvije
 * sekunde (stick s jednom tipkom). Otvoren: smjerovi pomiču i mijenjaju
 * (držani se ponavljaju), fire / crveni bira kao Return, druga tipka / plavi
 * ili Select zatvara. Port 1 se igri čita kao ništa pritisnuto dok je menu
 * otvoren, i nakon što se zatvori dok se sve ne pusti.
 *
 * Sample se uzima unutar chipsetova locka (JOY1DAT read), a slikini setteri
 * uzimaju taj lock, pa promjena čeka sljedeći 68k I/O read izvan njega
 * (aga_osd_run, vectors.c). */
volatile int aga_osd_hide;                     /* port 1 neutralan za igru (aga_pad.c) */
volatile int aga_osd_pend;                     /* promjena čeka aga_osd_run */
static volatile int osd_pend_item, osd_pend_dir;
static int osd_in_last, osd_wait_release;
static uint64_t osd_both_from, osd_rep_at, osd_closed_at;
static int osd_flick;                          /* fire držan: Up Down Up Down dosad gurnuto, 0-4 */
static uint64_t osd_flick_from;

static void osd_set_open(int on)
{
    osd_open = on;
    if (on) { osd_saved = 0; osd_ss_unavailable = 0; aga_osd_hide = 1; }
    else { aga_osd_hide = 0; osd_wait_release = 0; }
}

static void osd_queue(int item, int dir)
{
    if (__atomic_load_n(&aga_osd_pend, __ATOMIC_ACQUIRE)) return;   /* jedno po jedno */
    osd_pend_item = item;
    osd_pend_dir = dir;
    __atomic_store_n(&aga_osd_pend, 1, __ATOMIC_RELEASE);
}

/* vectors.c, core 0, izvan locka: promjena koju je pad tražio */
void aga_osd_run(void)
{
    if (!__atomic_exchange_n(&aga_osd_pend, 0, __ATOMIC_ACQ_REL)) return;
    osd_change(osd_pend_item, osd_pend_dir);
}

/* aga_pad.c, core 0: što port 1 sada ima pritisnuto (OSDI_*, aga_glue.h) */
void aga_osd_input(int in)
{
    uint64_t now = mbox_now(), f;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    int prev = osd_in_last, edge = in & ~prev;
    osd_in_last = in;
    if (!osd_open && !osd_mode) {                /* isključeno: nema ulaza, igra ima sve */
        osd_both_from = 0;
        osd_flick = 0;
        aga_osd_hide = 0;
        return;
    }
    if (!osd_open) {
        if ((edge & OSDI_SEL) && osd_mode >= 2) { osd_set_open(1); osd_wait_release = 1; osd_both_from = 0; return; }
        if (osd_mode < 3) osd_both_from = 0, osd_flick = 0;   /* stickovi načini nisu traženi */
        else if ((in & OSDI_BOTH) == OSDI_BOTH) {
            if (!osd_both_from) osd_both_from = now;
            else if (now - osd_both_from >= 2 * f) {
                osd_set_open(1);
                osd_wait_release = 1;            /* dvije tipke su još dolje */
                osd_both_from = 0;
            }
        } else osd_both_from = 0;
        /* fire držan, stick Up, Down, Up, Down unutar dvije sekunde: način za
           stick čija druga tipka ne stiže do mene */
        if (!(in & OSDI_A) || osd_mode < 3) osd_flick = 0;
        else if (edge & (OSDI_UP | OSDI_DOWN)) {
            int want = (osd_flick & 1) ? OSDI_DOWN : OSDI_UP;
            if (osd_flick && now - osd_flick_from > 2 * f) osd_flick = 0, want = OSDI_UP;
            if (edge & want) {
                if (!osd_flick) osd_flick_from = now;
                if (++osd_flick == 4) {
                    osd_flick = 0;
                    osd_set_open(1);
                    osd_wait_release = 1;        /* fire je još dolje */
                    return;
                }
            } else if (edge & OSDI_UP) { osd_flick = 1; osd_flick_from = now; }
            else osd_flick = 0;
        }
        if (aga_osd_hide) {                      /* zatvoren: sve pušteno, ili najviše sekunda */
            if (!osd_closed_at) osd_closed_at = now;
            if (!in || now - osd_closed_at > f) { aga_osd_hide = 0; osd_closed_at = 0; }
        }
        return;
    }
    aga_osd_hide = 1;
    if (osd_wait_release) {                      /* što ga je otvorilo, prvo pušteno */
        if (in & (OSDI_A | OSDI_B | OSDI_SEL)) return;
        osd_wait_release = 0;
        edge = in;
    }
    int act = edge, dirs = in & 15;
    if (edge & 15) osd_rep_at = now + f * 2 / 5;            /* držan smjer ponavlja nakon 0.4 s */
    else if (dirs && now >= osd_rep_at) { act |= dirs; osd_rep_at = now + f * 3 / 20; }   /* ...svakih 0.15 s */
    if (act & OSDI_UP)    osd_row = (osd_row + OI_N - 1) % OI_N;
    if (act & OSDI_DOWN)  osd_row = (osd_row + 1) % OI_N;
    if (act & OSDI_LEFT)  osd_queue(osd_row, -1);
    if (act & OSDI_RIGHT) osd_queue(osd_row, +1);
    if (act & OSDI_A) {
        if (osd_row == OI_SAVE) { osd_save_req = 1; osd_saved = 1; }
        else if (osd_row == OI_CLOSE) osd_set_open(0);
        else osd_queue(osd_row, +1);
    }
    if (act & (OSDI_B | OSDI_SEL)) osd_set_open(0);
}

/* $5284 / $5285: monitor line off / on (agaboot HUD); $5286: save request
   pročitan i obavljen (agaboot, pri izlasku); $5288-$528B (8-11): kako se
   OSD otvara (osd_mode) - off ga zatvara i ne ostavlja način da se otvori */
void aga_osd_command(int v)
{
    if (v == 0 || v == 1) hud_on = v;
    if (v == 2) osd_save_req = 0;
    if (v >= 8 && v <= 11) {
        osd_mode = v & 3;
        if (!osd_mode) { osd_open = 0; hud_on = 0; }
    }
}

/* aga_diag.c SEL_OSD: word 0 flags (bit 0 save zatražen, bit 1 monitor
   uključen, bit 2 menu otvoren), pa scanlines %, shape, mask, look, scale,
   brightness, gamma, flicker fixer */
uint16_t aga_osd_info(int i)
{
    if (i == 0) return (uint16_t)((osd_save_req ? 1 : 0) | (hud_on ? 2 : 0) | (osd_open ? 4 : 0) | 8);   /* 8: word 9 je veličina */
    static const uint8_t item[10] = { 0, OI_SCAN, OI_SHAPE, OI_MASK, OI_LOOK, OI_SCALE,
                                      OI_BRIGHT, OI_GAMMA, OI_FLICKER, OI_SIZE };
    if (i >= 1 && i <= 9) return (uint16_t)osd_value(item[i]);
    return 0;
}

/* ---- monitorove brojke (core 3, svake dvije sekunde dok je uključen) ---- */
static uint32_t *const hud_req = MBOX_LOW(0x1c80);     /* low memory, kao req */
static int hud_temp = -1, hud_fps = -1;
static uint32_t hud_thr;
static uint64_t hud_next, hud_fps_from;
static uint32_t hud_frames;
static int mbox_call_on(uint32_t *b, uint32_t bytes, int wait);

static void hud_sample(void)
{
    uint64_t now = mbox_now(), f;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    hud_frames++;
    if (!hud_fps_from) { hud_fps_from = now; hud_frames = 0; }
    if (now - hud_fps_from >= f) {
        hud_fps = (int)((uint64_t)hud_frames * f / (now - hud_fps_from));
        hud_fps_from = now;
        hud_frames = 0;
    }
    if (now < hud_next) return;
    hud_next = now + 2 * f;
    int c = 1;
    hud_req[c++] = 0;
    hud_req[c++] = LE32(0x30006); hud_req[c++] = LE32(8); hud_req[c++] = 0;   /* GET_TEMPERATURE */
    int tpos = c; hud_req[c++] = 0; hud_req[c++] = 0;
    hud_req[c++] = LE32(0x30046); hud_req[c++] = LE32(4); hud_req[c++] = 0;   /* GET_THROTTLED */
    int fpos = c; hud_req[c++] = 0;
    hud_req[c++] = 0;
    hud_req[0] = LE32(c << 2);
    if (mbox_call_on(hud_req, MBOX_BUF_BYTES, 0) == 0) {           /* ne čeka drugog pozivatelja */
        hud_temp = (int)((LE32(hud_req[tpos + 1]) + 500) / 1000);
        hud_thr = LE32(hud_req[fpos]);
    }
}

static void hud_draw(const struct osd_fb *f)
{
    char t[24], *p = t;
    if (hud_temp >= 0) { p = osd_num(p, hud_temp); *p++ = 'C'; *p++ = ' '; }
    if (hud_fps >= 0) { p = osd_num(p, hud_fps); *p++ = 'F'; *p++ = 'P'; *p++ = 'S'; }
    *p = 0;
    if (!t[0]) return;
    uint32_t col = (hud_thr & 0x7) ? 0xFFFF4040u : (hud_thr & 0x8) ? 0xFFFFD040u : 0xFF60FF60u;
    int w = osd_len(t) * OSD_CW, x = f->x0 + f->w - w - 10, y = f->y0 + 4;
    osd_fill(f, x - 4, y - 2, w + 6, OSD_CH + 2, 0xFF000000u);
    osd_text(f, x, y, t, col);
}

static void osd_draw(const struct osd_fb *f)
{
    const uint32_t bg = 0xFF101828u, frame = 0xFF8090B0u, label = 0xFFC0C8D8u,
                   value = 0xFFFFD060u, hi_bg = 0xFF304878u, title = 0xFFFFFFFFu;
    int w = 32 * OSD_CW + 16, h = (OI_N + 3) * OSD_CH + 10;
    int x = f->x0 + (f->w - w) / 2, y = f->y0 + (f->h - h) / 2;
    if (x < f->x0) x = f->x0;
    if (y < f->y0) y = f->y0;
    osd_fill(f, x - 2, y - 2, w + 4, h + 4, frame);
    osd_fill(f, x, y, w, h, bg);
    osd_text(f, x + 8, y + 4, "AGA-STORM PICTURE", title);
    int row = osd_row;
    for (int i = 0; i < OI_N; i++) {
        int ly = y + 4 + (i + 2) * OSD_CH;
        char v[24];
        if (i == row) osd_fill(f, x + 4, ly - 1, w - 8, OSD_CH, hi_bg);
        osd_text(f, x + 8, ly, osd_label[i], label);
        osd_value_text(i, v);
        if (v[0]) {
            int vx = x + w - 8 - osd_len(v) * OSD_CW;
            osd_text(f, vx, ly, v, value);
        }
    }
    /* The OSD is not a functional save-state implementation yet. Tell
     * the user visibly instead of accepting Return and silently doing nothing. */
    if(osd_ss_unavailable)
        osd_text(f, x + 8, y + h - 10, "SAVE/LOAD NOT READY", value);
}

/* Allow overlay composition even without debug HUD enabled. */
static int osd_enabled(void)
{
    return __atomic_load_n(&osd_open,__ATOMIC_ACQUIRE) ||
           __atomic_load_n(&hud_on,__ATOMIC_ACQUIRE);
}
/* aga_video_present, core 3: menu i monitor preko ovog framea */
static void osd_render(uint32_t *dst, int woven)
{
    if (hud_on) hud_sample();
    else hud_fps_from = 0;
    if (!osd_open && !hud_on) return;
    if(osd_open && frf_osd_render_count++ < 3u)
        kprintf("[FRF OSD] draw=%u fb=%p mode=%d\n",
                frf_osd_render_count,dst,(int)osd_mode);
    struct osd_fb f;
    f.d = dst;
    f.ry = woven ? 2 : 1;
    f.rows = woven ? 2 * AGA_OUT_H : AGA_OUT_H;
    f.x0 = (int)crop_x; f.y0 = (int)crop_y;
    f.w = (int)crop_w;  f.h = (int)crop_h;
    if (f.w <= 0 || f.h <= 0) { f.x0 = f.y0 = 0; f.w = AGA_OUT_W; f.h = AGA_OUT_H; }
    if (hud_on) hud_draw(&f);
    if (osd_open) osd_draw(&f);
}

/* ---- debug overlay --------------------------------------------------------
 * When a game wedges, AmigaOS is gone and caffed cannot answer - but the
 * chipset loop keeps presenting frames, so the screen is the only channel
 * left. And every way out of a wedge (Ctrl-Amiga-Amiga included, measured
 * 2026-09-07) resets the Pi and wipes the counters, which is why two
 * Banshee runs in a row told me jack shit. So: draw the numbers into the
 * frame and photograph them.
 *
 * Seven-segment, not a font: sixteen glyphs from a sixteen-byte table and
 * four rectangles, no font data to carry.
 */
#define OVL_ROWS 12
static volatile uint32_t ovl_val[OVL_ROWS];
static volatile int      ovl_allowed = 0;  /* v0.8.5: only boot config may open this gate */
static volatile int      ovl_on = 0;       /* always off at power-on */

void aga_video_overlay(int row, uint32_t v) { if ((unsigned)row < OVL_ROWS) ovl_val[row] = v; }
void aga_video_overlay_allow(int on)
{
    ovl_allowed = on ? 1 : 0;
    if (!ovl_allowed) ovl_on = 0;
}
void aga_video_overlay_enable(int on)
{
    ovl_on = (ovl_allowed && on) ? 1 : 0;
}

/* bit 0..6 = segmenti a,b,c,d,e,f,g */
static const uint8_t seg7[16] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,
    0x7F, 0x6F, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71,
};

#define DW 10          /* širina digit cella  */
#define DH 18          /* visina digit cella */
#define DT 2           /* debljina segmenta */

static void ovl_fill(uint32_t *dst, int x, int y, int w, int h, uint32_t col)
{
    if (x < 0 || y < 0 || x + w > AGA_OUT_W || y + h > AGA_OUT_H) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            dst[(y + j) * AGA_OUT_W + (x + i)] = col;      /* native, kao coreovi pikseli */
}

static void ovl_digit(uint32_t *dst, int x, int y, int d, uint32_t col)
{
    int s = seg7[d & 15], half = (DH - 3 * DT) / 2;
    if (s & 0x01) ovl_fill(dst, x + DT,      y,                     DW - 2 * DT, DT,   col); /* a */
    if (s & 0x02) ovl_fill(dst, x + DW - DT, y + DT,                DT,          half, col); /* b */
    if (s & 0x04) ovl_fill(dst, x + DW - DT, y + 2 * DT + half,     DT,          half, col); /* c */
    if (s & 0x08) ovl_fill(dst, x + DT,      y + 2 * DT + 2 * half, DW - 2 * DT, DT,   col); /* d */
    if (s & 0x10) ovl_fill(dst, x,           y + 2 * DT + half,     DT,          half, col); /* e */
    if (s & 0x20) ovl_fill(dst, x,           y + DT,                DT,          half, col); /* f */
    if (s & 0x40) ovl_fill(dst, x + DT,      y + DT + half,         DW - 2 * DT, DT,   col); /* g */
}

static void ovl_draw(uint32_t *dst)
{
    const int x0 = 8, y0 = 8, pitch = DH + 6;
    ovl_fill(dst, x0 - 4, y0 - 4, 8 * (DW + 2) + 8, OVL_ROWS * pitch + 8, 0xFF000000u);
    for (int r = 0; r < OVL_ROWS; r++) {
        uint32_t v = ovl_val[r];
        /* red 0 je najvažniji, pa nek se istakne */
        uint32_t col = 0xFF000000u | (r == 0 ? 0x00FF6060u : 0x0000FF00u);
        for (int d = 0; d < 8; d++)
            ovl_digit(dst, x0 + d * (DW + 2), y0 + r * pitch, (v >> ((7 - d) * 4)) & 0xF, col);
    }
}

/* ---- the Position screen ------------------------------------------------------
 * agaboot POSITION (Chipset menu - RTG Image - Position...) moves the picture
 * over this. RGB2RTG opens a native screen for it because there the Amiga's
 * own output is what HDMI shows; here the desktop's native screens never
 * reach HDMI, so the kernel draws the pattern into the frame itself, right
 * where a standard PAL screen lands (DIWSTRT $2C81: 640 x 256 from column
 * 114, line 18), and shows it through the current size, look and position.
 * The rest of the frame - overscan a game may use - is dark blue, so its
 * edges show too. Grid, border, corner marks and centre as on RGB2RTG's
 * screen; the two offsets, right and down, in the overlay's digits. agaboot
 * keeps the window that takes the keys and mouse; on the desktop nobody can
 * see it anyway. Never while the sandbox is up: a game or the system-wide
 * desktop owns the plane then, and a move shows on the live picture. */
#define CAL_X0  114
#define CAL_Y0  18
#define CAL_W   640
#define CAL_H   256
static volatile int calib_on;

static void cal_hline(uint32_t *d, int x0, int x1, int y, uint32_t c) { ovl_fill(d, x0, y, x1 - x0 + 1, 1, c); }
static void cal_vline(uint32_t *d, int x, int y0, int y1, uint32_t c) { ovl_fill(d, x, y0, 1, y1 - y0 + 1, c); }

/* sign i do tri znamenke, desno poravnato u četiri cella */
static void cal_number(uint32_t *d, int x, int y, int v, uint32_t col)
{
    int a = v < 0 ? -v : v, half = (DH - 3 * DT) / 2;
    if (a > 999) a = 999;
    if (v) {
        ovl_fill(d, x + DT, y + DT + half, DW - 2 * DT, DT, col);                   /* minus: segment g */
        if (v > 0) ovl_fill(d, x + DW / 2 - DT / 2, y + DT + half / 2, DT, half + DT, col);   /* plus: i jedna uspravna */
    }
    for (int i = 3; i >= 1; i--) {
        ovl_digit(d, x + i * (DW + 2), y, a % 10, col);
        a /= 10;
        if (!a) break;
    }
}

static void calib_pattern(uint32_t *d)
{
    const uint32_t outside = 0xFF101830u, black = 0xFF000000u, grid = 0xFF404050u,
                   white = 0xFFFFFFFFu, orange = 0xFFFF9900u;
    for (int i = 0; i < AGA_OUT_W * AGA_OUT_H; i++) d[i] = outside;
    ovl_fill(d, CAL_X0, CAL_Y0, CAL_W, CAL_H, black);
    for (int x = 32; x < CAL_W; x += 32) cal_vline(d, CAL_X0 + x, CAL_Y0, CAL_Y0 + CAL_H - 1, grid);
    for (int y = 32; y < CAL_H; y += 32) cal_hline(d, CAL_X0, CAL_X0 + CAL_W - 1, CAL_Y0 + y, grid);
    for (int k = 0; k < 2; k++) {                  /* border standardnog područja, dva piksela */
        cal_hline(d, CAL_X0 + k, CAL_X0 + CAL_W - 1 - k, CAL_Y0 + k, white);
        cal_hline(d, CAL_X0 + k, CAL_X0 + CAL_W - 1 - k, CAL_Y0 + CAL_H - 1 - k, white);
        cal_vline(d, CAL_X0 + k, CAL_Y0 + k, CAL_Y0 + CAL_H - 1 - k, white);
        cal_vline(d, CAL_X0 + CAL_W - 1 - k, CAL_Y0 + k, CAL_Y0 + CAL_H - 1 - k, white);
    }
    for (int k = 0; k < 4; k++) {                  /* kutne oznake, osam piksela unutra */
        int cx = CAL_X0 + ((k & 1) ? CAL_W - 9 : 8), cy = CAL_Y0 + ((k & 2) ? CAL_H - 9 : 8);
        int hx = (k & 1) ? cx - 40 : cx, vy = (k & 2) ? cy - 20 : cy;
        cal_hline(d, hx, hx + 40, cy, orange);
        cal_vline(d, cx, vy, vy + 20, orange);
    }
    cal_hline(d, CAL_X0 + 300, CAL_X0 + 340, CAL_Y0 + CAL_H / 2, orange);          /* centar */
    cal_vline(d, CAL_X0 + 320, CAL_Y0 + CAL_H / 2 - 20, CAL_Y0 + CAL_H / 2 + 20, orange);
}

static void calib_draw(uint32_t *d)
{
    const uint32_t black = 0xFF000000u, white = 0xFFFFFFFFu, orange = 0xFFFF9900u;
    calib_pattern(d);
    /* offseti, kako ih agaboot kaže: strelica desno i njezin broj, strelica
       dolje i njezin broj, na crnom da ih grid ne presijeca */
    int y = CAL_Y0 + 80, x = CAL_X0 + 320 - 80, mid = y + DH / 2;
    ovl_fill(d, x - 8, y - 6, 176, DH + 12, black);
    cal_hline(d, x, x + 12, mid, white);
    for (int k = 1; k <= 4; k++) { ovl_fill(d, x + 12 - k, mid - k, 1, 1, white); ovl_fill(d, x + 12 - k, mid + k, 1, 1, white); }
    cal_number(d, x + 18, y, pos_x, orange);
    x += 18 + 4 * (DW + 2) + 16;
    cal_vline(d, x + 6, y + 2, y + DH - 3, white);
    for (int k = 1; k <= 4; k++) { ovl_fill(d, x + 6 - k, y + DH - 3 - k, 1, 1, white); ovl_fill(d, x + 6 + k, y + DH - 3 - k, 1, 1, white); }
    cal_number(d, x + 16, y, pos_y, orange);
}

/* Copy the last game frame to SNAP_OFF while the pages still hold it: for a
   woven one - both pages one frame of 572 rows - its own field. 64-bit
   volatile copies, aligned: the pages are uncached. */
static void snap_take(void)
{
    if (!last_ok) return;
    last_ok = 0;
    uint64_t t0 = cnt_now();
    volatile uint64_t *d = (volatile uint64_t *)(fb_va + SNAP_OFF);
    for (int y = 0; y < AGA_OUT_H; y++) {
        uintptr_t row = last_woven ? (uintptr_t)(2 * y + last_row) * AGA_OUT_W * 4
                                   : (uintptr_t)last_page * FB_BYTES + (uintptr_t)y * AGA_OUT_W * 4;
        const volatile uint64_t *s = (const volatile uint64_t *)(fb_va + row);
        for (int i = 0; i < AGA_OUT_W / 2; i++) *d++ = s[i];
    }
    snap_ok = 1;
    snap_ntsc = last_ntsc;
    uint64_t f;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    uint64_t us = ((cnt_now() - t0) * 1000000u) / (f ? f : 1);
    snap_us = us > 0xFFFF ? 0xFFFF : (uint32_t)us;
    crop_state();
}

/* ---- crop screen --------------------------------------------------------------
 * agabootov Crop... (RTG Image, Size Custom): slika zadnje igre - ili, kad
 * se nijedan standard nije rezao od kad je Pi startao, pattern Position
 * screena - odrezana na rubove koji se postavljaju, kako će je igra
 * pokazati. Broj svakog ruba sjedi uz njega; onaj koji tipke pomiču je
 * narančast, s trakom duž sebe. $5880 + E3(s) ga digne (aga_crop.h) ili
 * označi drugi rub, $5930 ga završi kako završava Position. Samo desktop,
 * kao Position: sa sandboxom gore agaboot reže live sliku umjesto toga. */
static int crop_std, crop_edge;

/* AGA_D_CROP, za agaboot: bit 0 kernel reže Custom rubove, bit 1 crop
   screen pokazuje sliku zadnje igre, bit 2 frame koji se reže je NTSC, bit 3
   postoji slika zadnje igre za pokazati, bit 4 ona je NTSC; bitovi 16-31
   mikrosekunde koje je kopija zadnje slike uzela */
static void crop_state(void)
{
    int have = snap_ok || last_ok, hn = last_ok ? last_ntsc : snap_ntsc;
    aga_diag_set(AGA_D_CROP, 1u | ((crop_on && snap_ok && snap_ntsc == crop_std) ? 2u : 0u) | (frame_ntsc ? 4u : 0u)
                             | (have ? 8u : 0u) | ((have && hn) ? 16u : 0u) | (snap_us << 16));
}

/* do tri znamenke, desno poravnato, na crnom */
static void crop_number(uint32_t *d, int x, int y, int v, uint32_t col)
{
    ovl_fill(d, x - 4, y - 4, 3 * (DW + 2) + 6, DH + 8, 0xFF000000u);
    for (int i = 2; i >= 0; i--) {
        ovl_digit(d, x + i * (DW + 2), y, v % 10, col);
        v /= 10;
        if (!v) break;
    }
}

static void crop_marks(uint32_t *d)
{
    const uint32_t white = 0xFFFFFFFFu, orange = 0xFFFF9900u;
    int x = (int)crop_x, y = (int)crop_y, w = (int)crop_w, h = (int)crop_h, nw = 3 * (DW + 2) - 2;
    int rows = frame_ntsc ? 237 : AGA_OUT_H;
    int v[4]  = { x, y, AGA_OUT_W - x - w, rows - y - h };
    int nx[4] = { x + 10, x + (w - nw) / 2, x + w - 10 - nw, x + (w - nw) / 2 };
    int ny[4] = { y + (h - DH) / 2, y + 10, y + (h - DH) / 2, y + h - 10 - DH };
    switch (crop_edge) {                         /* traka, unutar ruba */
    case CUT_L: ovl_fill(d, x, y, 4, h, orange); break;
    case CUT_T: ovl_fill(d, x, y, w, 2, orange); break;
    case CUT_R: ovl_fill(d, x + w - 4, y, 4, h, orange); break;
    default:    ovl_fill(d, x, y + h - 2, w, 2, orange); break;
    }
    for (int e = 0; e < 4; e++) crop_number(d, nx[e], ny[e], v[e], e == crop_edge ? orange : white);
}

static void crop_show(void)
{
    int p = fb_next_page(fb_page);
    uint32_t *d = (uint32_t *)(fb_va + (uintptr_t)p * FB_BYTES);
    if (snap_ok && snap_ntsc == crop_std) {
        volatile uint64_t *o = (volatile uint64_t *)d;
        const volatile uint64_t *s = (const volatile uint64_t *)(fb_va + SNAP_OFF);
        for (uint32_t i = 0; i < FB_BYTES / 8; i++) o[i] = s[i];
    } else {
        calib_pattern(d);
    }
    frame_woven = 0;                    /* jedan page, standarda koji se reže */
    frame_row = 0;
    frame_ntsc = crop_std;
    apply_crop();
    crop_marks(d);
    show(fb_phys + (uintptr_t)p * FB_BYTES);
    fb_page = p;
}

void aga_video_crop_screen(int slot)
{
    if (!video_ok || aga_sandbox_active) return;
    snap_take();
    calib_on = 0;
    crop_std = (slot >> 2) & 1;
    crop_edge = slot & 3;
    crop_on = 1;
    crop_show();
}

static void calib_show(void)
{
    int p = fb_next_page(fb_page);
    uint32_t *d = (uint32_t *)(fb_va + (uintptr_t)p * FB_BYTES);
    calib_draw(d);
    show(fb_phys + (uintptr_t)p * FB_BYTES);
    fb_page = p;
}

/* $5931 on, $5930 off. From the abort handler on core 0, like every control
   word; with the sandbox down core 3 does not present, so the pages and
   lists are mine to use here. */
void aga_video_calibrate(int on)
{
    if (!video_ok) return;
    if (!on) {
        if (!calib_on && !crop_on) return;
        int was_crop = crop_on;
        calib_on = 0;
        crop_on = 0;
        if (!aga_sandbox_active) hide();
        if (was_crop) apply_crop();     /* the size's own geometry again */
        return;
    }
    if (aga_sandbox_active) return;
    snap_take();                        /* the last game's picture, before the pattern covers it */
    if (crop_on) { crop_on = 0; apply_crop(); }
    calib_on = 1;
    /* the pattern is one PAL page: whatever the last game left the frame
       state at - woven, NTSC - the Position screen gets framed as PAL */
    if (frame_woven || frame_ntsc) aga_video_set_frame(0, 0, 0);
    calib_show();
}

static void calib_refresh(void)
{
    if (!video_ok || aga_sandbox_active) return;
    if (calib_on) calib_show();
    else if (crop_on) crop_show();
}

/* ---- HDMI sync (1.114, agaboot RTG Image) ----------------------------------------
 * The HDMI frame follows the emulated one (aga_sync.h): at each frame shown,
 * the HVS's line says where in the HDMI frame it was presented, and the
 * vertical front porch of the PixelValve that times HDMI - VERTB's top half -
 * is made a line or two longer to hold that place. RGB2RTG's rules for the
 * writes (its rgb_follow.c): the mode's own porch to 16 lines more, never
 * less; one line at a time; only while the HVS is well inside the picture,
 * since a porch changed while the PixelValve is in it could send its line
 * counter past the end and the display lose sync; the mode's own porch kept
 * from the first look, and the PixelValve looked at again once a second -
 * a new mode (another active height) is learned afresh, and a PixelValve
 * not enabled or not showing the HVS's height is never written. HDMI0's on
 * the Pi 4 (PixelValve 2), the only one on the Pi 3. Off by default -
 * $5891 on, $5890 off - and the mode's own porch comes back when it is
 * switched off and when the sandbox is left. */
#define PV_BASE      (hvs_gen == 4 ? 0xF2807000UL : 0xF220A000UL)
#define PV_V_CONTROL 0x04
#define PV_VERTA     0x14
#define PV_VERTB     0x18
static volatile int hsync_on;
static uint32_t frame_len; /* last emulated frame's actual colour-clock length */
static int pv_state;                    /* 0 not looked at, 1 good, -1 not to be written */
static int pv_have;                     /* vfp0/vactive0 are the mode's own */
static uint32_t pv_vfp0, pv_vactive0;
static int pv_k;                        /* porch lines over the mode's own, in force */
static uint64_t pv_look;                /* the next look at the PixelValve */
static struct hsync hs = { .prev = -1, .ph16 = -1 };

static uint64_t cnt_freq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f ? f : 1; }

static int pv_check(void)
{
    uint32_t vc = reg_rd(PV_BASE + PV_V_CONTROL), va = reg_rd(PV_BASE + PV_VERTA), vb = reg_rd(PV_BASE + PV_VERTB);
    if (!pv_have || (vb & 0xffff) != pv_vactive0) {        /* prvi pogled, ili novi mode */
        pv_vfp0 = vb >> 16;
        pv_vactive0 = vb & 0xffff;
        pv_have = 1;
        pv_k = 0;
        hsync_reset(&hs);
    }
    int32_t dh = (int32_t)disp_h - (int32_t)pv_vactive0;
    uint32_t vtotal = pv_vactive0 + pv_vfp0 + (va >> 16) + (va & 0xffff);
    aga_diag_set(AGA_D_HDMI_SYNC_PV, pv_vfp0 << 16 | pv_vactive0);
    return (vc & 1) && pv_vfp0 >= 1 && pv_vfp0 < 64 && dh >= -2 && dh <= 2 && vtotal > pv_vactive0;
}

static void hsync_report(int phase)
{
    aga_diag_set(AGA_D_HDMI_SYNC, (uint32_t)(hsync_on ? 1 : 0) | (uint32_t)(pv_state == 1 ? 2 : pv_state < 0 ? 4 : 0)
                                  | (uint32_t)(hs.reach + 1) << 4 | (uint32_t)pv_k << 8 | (uint32_t)(phase & 0xffff) << 16);
    aga_diag_set(AGA_D_HDMI_SYNC_NEED, (uint32_t)hs.need16);
    aga_diag_set(AGA_D_HDMI_SYNC_SLIPS, hs.slips);
}

/* porch na `k` linija preko modeovih vlastitih, kad je HVS duboko unutar
   slike - čekajući do frame za to (wait 0: samo ako je sada) */
static int pv_write(int k, int wait)
{
    uint64_t end = cnt_now() + cnt_freq() / 40;
    for (;;) {
        uint32_t line = reg_rd(REG_DISPSTAT1) & 0xfff;
        if (line > 100 && line + 200 < pv_vactive0) {
            reg_wr(PV_BASE + PV_VERTB, (pv_vfp0 + (uint32_t)k) << 16 | pv_vactive0);
            pv_k = k;
            return 1;
        }
        if (!wait || cnt_now() >= end) return 0;
    }
}

/* opet modeov vlastiti porch (isključeno, sandbox napušten) */
static void hsync_restore(void)
{
    if (pv_have && pv_state == 1 && pv_k) pv_write(0, 1);
    hs.prev = -1;
    hsync_report(-1);
}

/* A stable, in-range sync porch may stay in place during desktop
   handoff: the next 50.2Hz game avoids six display timing writes.
   Out-of-reach or remeasuring modes go back to the original porch. */
static void hsync_park(void)
{
    if (hsync_on && pv_have && pv_state == 1 && hs.reach == 1 && hs.steady) {
        hs.prev = -1;
        hsync_report(-1);
        return;
    }
    hsync_restore();
}

void aga_video_set_frame_len(uint32_t cck)
{
    frame_len = cck;
}

/* svaki prikazani frame (core 3, nakon show) */
static void hsync_frame(void)
{
    if (!hsync_on) return;
    uint64_t now = cnt_now();
    if (!pv_state || now >= pv_look) {
        pv_state = pv_check() ? 1 : -1;
        pv_look = now + cnt_freq();
    }
    if (pv_state != 1) { hsync_report(-1); return; }
    uint32_t line = reg_rd(REG_DISPSTAT1) & 0xfff;
    int phase = line + 2 < pv_vactive0 ? (int)line : -1;
    hsync_len(&hs, frame_len);
    int want = hsync_step(&hs, phase, (int)pv_vactive0, pv_k);
    if (want > pv_k + 1) want = pv_k + 1;
    if (want < pv_k - 1) want = pv_k - 1;
    if (want != pv_k) pv_write(want, 0);
    hsync_report(phase);
}

/* $5891 on / $5890 off (agaboot RTG Image, HDMI sync) - core 0, trap */
void aga_video_hdmi_sync(int on)
{
    if (!video_ok) return;
    hsync_on = on ? 1 : 0;
    if (!on) hsync_restore();
    else hsync_report(-1);
}

/* Read the live plane and the kernels back against what was written: a
   word that differs is logged (the first eight a boot) with what it holds
   now. Every 50th frame, core 3, from aga_video_present. It found nothing
   changed when the Pi 3's picture went dark with lines (2026-10-10) - the
   kernel itself had been computed wrong, a GCC miscompile of the A53
   tuning (see Emu68's CMakeLists). Sneaky shit, that was. It stays as
   the cheap guard it is. */
static void hvs_check(void)
{
    static int logged;
    static uint32_t looks, bad_total;
    static char line[96];
    if (!cur_list || cur_black || cur_is_windowed || bl_block != cur_list) return;
    looks++;
    uint32_t bad = 0;
    for (uint32_t i = 0; i <= bl_n && i < BL_MAX + 1; i++) {
        uint32_t w, want;
        if (i == 0) { w = bl_block; want = bl_ctl; }
        else if (i - 1 < BL_MAX && bl_mine[i - 1]) { w = bl_block + i; want = bl_want[i - 1]; }
        else continue;
        uint32_t v = dl_rd(w);
        if (v == want) continue;
        bad++;
        if (logged < 8) {
            int n = 0;
            for (const char *s = "[AGA] HVS check: word "; *s; ) line[n++] = *s++;
            hex_put(line + n, w, 3); n += 3;
            for (const char *s = " wrote "; *s; ) line[n++] = *s++;
            hex_put(line + n, want, 8); n += 8;
            for (const char *s = " now "; *s; ) line[n++] = *s++;
            hex_put(line + n, v, 8); n += 8;
            line[n] = 0;
            aga_diag_log(line);
            logged++;
        }
    }
    for (int i = 0; i < KERNEL_WORDS; i++) {
        uint32_t kh = dl_rd(KERNEL_WORD + i), kv = dl_rd(KERNEL_WORD + KERNEL_WORDS + i);
        for (int k = 0; k < 2; k++) {
            uint32_t v = k ? kv : kh, want = k ? cur_kernel_v[i] : cur_kernel_h[i];
            if (v == want) continue;
            bad++;
            if (logged < 8) {
                int n = 0;
                for (const char *s = k ? "[AGA] HVS check: kernel V " : "[AGA] HVS check: kernel H "; *s; ) line[n++] = *s++;
                hex_put(line + n, (uint32_t)i, 2); n += 2;
                for (const char *s = " wrote "; *s; ) line[n++] = *s++;
                hex_put(line + n, want, 8); n += 8;
                for (const char *s = " now "; *s; ) line[n++] = *s++;
                hex_put(line + n, v, 8); n += 8;
                line[n] = 0;
                aga_diag_log(line);
                logged++;
            }
        }
    }
    bad_total += bad;
}

void aga_video_present(const uint32_t *frame, int native_active)
{
    /* Native output state is not the sandbox state in standalone mode. */
    __atomic_store_n(&frf_osd_native_video_active,0u,__ATOMIC_RELEASE);
    if (!video_ok) return;
#if defined(FRF_VIRTUAL_AGA) && defined(FRF_RTG_HANDOFF)
    /* P96 VideoCore.card writes DISPLIST1 independently on the 68k.
       On a standalone Amiga our AGA core runs persistently; without this
       ownership check show_frame() can overwrite P96's list every 20 ms.
       Nothing is inferred from missing hardware and no RTG driver is patched.
       Check a *valid* external HVS list only; don't misread blank/floating
       display memory as ownership. */
    {
        uint32_t live = reg_rd(REG_DISPLIST1);
        int external = live != 0 && live != LIST_A && live != LIST_B &&
                       live != frf_rtg_display.boot_list &&
                       live < DL_RAM_WORDS - 0x70u && list_len(live) > 0;
        unsigned was = frf_rtg_display.rtg_owns;
        int rtg = frf_rtg_handoff_step(&frf_rtg_display,live,
            LIST_A,LIST_B,native_active,external,cur_list != 0);
        if (rtg) {
            if (!was) {
                saved_list = live;
                aga_diag_log("[RTG] external P96 display list claimed HDMI; yielding native AGA plane");
            }
            if (cur_list) hide(); /* if P96 already owns HVS, don't overwrite it */
            return;
        }
        if (was) aga_diag_log("[RTG] native AGA display became active; HDMI reclaimed");
    }
#endif
    calib_on = 0;      /* the sandbox has the pages now: a Position screen left up is over */
    if (crop_on) { crop_on = 0; apply_crop(); }   /* ...and a crop screen */
    /* No native display. Normally hide the plane and let the RTG desktop
       show through - and that is what the "grey screen" was all along:
       not my output at all, but P96's screen sitting blank after WHDLoad
       took the machine off AmigaOS. Which also means the overlay never
       drew, because the code returned here first.

       Sandbox up, that is the case worth looking at, so show a
       black plane with the numbers on it instead. Outside the sandbox
       nothing changes, desktop untouched. */
    if (!native_active) {
        snap_take();        /* the frame before this one is still in its page, until the next is drawn */
        hs.prev = -1;       /* HDMI sync: the next frame shown starts a new run of phases */
        /* Default: hide. My display list REPLACES the HVS list, so while
           the plane is up the RTG desktop is not drawn at all - which hid
           WHDLoad's own splash menu and nobody could click Start. The idle
           overlay is opt-in (agaboot OVERLAY ON, $5F01) for when a game
           hangs without ever drawing. */
#ifdef FRF_VIRTUAL_AGA
        if (!(ovl_on && aga_overlay_idle)) { hide(); return; }
#else
        if (!(ovl_on && aga_sandbox_active && aga_overlay_idle)) { hide(); return; }
#endif
        if (frame_woven) aga_video_set_frame(0, 0, frame_ntsc);   /* the digits go on one page */
        /* TRANSPARENT, not black. The plane runs alpha mode FIXED_NONZERO
           (POS2_ALPHA_FIXED), so a pixel with a zero alpha byte is not drawn
           at all and the RTG desktop shows through; the overlay box stays
           opaque and readable. A black plane hid a WHDLoad requester behind
           the digits and cost a power-cut to find. */
        int p = fb_next_page(fb_page);
        uint32_t *d = (uint32_t *)(fb_va + (uintptr_t)p * FB_BYTES);
        for (int i = 0; i < AGA_OUT_W * AGA_OUT_H; i++) d[i] = 0;
        ovl_draw(d);
        show(fb_phys + (uintptr_t)p * FB_BYTES);
        fb_page = p;
        return;
    }

    __atomic_store_n(&frf_osd_native_video_active,1u,__ATOMIC_RELEASE);
    /* Core rendered this frame straight into one of my three pages (see
       aga_set_framebuffers3): no copy, and the pages are uncached, so no
       flush either. Used to be 900 KB read, 900 KB written, 900 KB of cache
       cleaned per frame - 1.2 ms on a Pi 4, and on a Pi 3 the working set
       the game's chip RAM traffic evicted. */
    int page = frame == (const uint32_t *)(fb_va + 2 * FB_BYTES) ? 2
             : frame == (const uint32_t *)(fb_va + FB_BYTES) ? 1 : 0;
    uint32_t *dst = (uint32_t *)(fb_va + (uintptr_t)page * FB_BYTES);
    /* the RGB port's picture: the frame, or for a woven one - both pages as
       one, from the first - its own field, every other row */
    const uint32_t *rgb = dst;
    int rgb_pitch = AGA_OUT_W;
    if (frame_woven) {
        page = 0;
        dst = (uint32_t *)fb_va;
        rgb = (const uint32_t *)(fb_va + (uintptr_t)frame_row * AGA_OUT_W * 4);
        rgb_pitch = 2 * AGA_OUT_W;
    }
    /* slika crop screena, dok je ništa ne prekriva (snap_take) */
    last_page = page;
    last_woven = frame_woven;
    last_row = frame_row;
    last_ntsc = frame_ntsc ? 1 : 0;
    last_ok = 1;
    crop_state();
    /* FRF RUN/P96 bridge: P96 owns HDMI temporarily, but the virtual AGA
       chipset and physical HAM6 stream continue without a sandbox teardown. */
    if (aga_program_mode && aga_program_rtg_yield) {
        hide();
        fb_page = page;
        aga_rgbout_frame(rgb, rgb_pitch, frame_woven ? -1 : page);
        return;
    }
    if (aga_rgbout_hdmi_off()) {
        /* igra samo na RGB portu: frame ipak ide coreu 1 */
        show_black();
        fb_page = page;
        aga_rgbout_frame(rgb, rgb_pitch, frame_woven ? -1 : page);
        return;
    }
    show_frame(fb_phys + (uintptr_t)page * FB_BYTES, (ovl_on && !frame_woven) || osd_enabled());
    fb_page = page;
    {
        static uint32_t check_n;
        if (++check_n % 50 == 0) hvs_check();
    }
    hsync_frame();
    aga_rgbout_frame(rgb, rgb_pitch, frame_woven ? -1 : page);   /* i na RGB port kao HAM6, kad ga igra ima uključenog (core 1) */
}

/* ---- mode switch: reboot the Pi, optionally with the firmware's one-shot
   tryboot flag so tryboot.txt (the AGA config) is used next boot only ------- */
#ifdef PISTORM_CLASSIC
/* Classic protocol keeps its reset inline in the housekeeper and exports no
   pi_reset(), so copy the watchdog sequence it uses (ps_classic_protocol.c,
   PIN_RESET branch). */
#define PM_RSTC         ((volatile unsigned int *)(0xf2000000 + 0x0010001c))
#define PM_RSTS         ((volatile unsigned int *)(0xf2000000 + 0x00100020))
#define PM_WDOG         ((volatile unsigned int *)(0xf2000000 + 0x00100024))
#define PM_WDOG_MAGIC   0x5a000000
#define PM_RSTC_FULLRST 0x00000020
static void pi_reset(void)
{
    unsigned int r = LE32(*PM_RSTS);
    r &= ~0xfffffaaau;                       /* boot s particije 0 */
    *PM_RSTS = LE32(PM_WDOG_MAGIC | r);
    *PM_WDOG = LE32(PM_WDOG_MAGIC | 10);
    *PM_RSTC = LE32(PM_WDOG_MAGIC | PM_RSTC_FULLRST);
    for (;;) { }
}
#else
extern void pi_reset(void);
#endif

void aga_reboot(int tryboot)
{
    aga_diag_inc(AGA_D_REBOOTS);
#ifdef FRF_VIRTUAL_AGA
    /* Standalone has no physical reset-line housekeeper to save the previous
       session. Save it explicitly before the Pi watchdog reset. */
    aga_diag_log("[AGA] standalone host reboot: preserving previous session");
    aga_diag_persist();
#endif
    if (tryboot) {
        int c = 1;
        req[c++] = 0;
        req[c++] = LE32(0x38064); req[c++] = LE32(4); req[c++] = 0;   /* SET_REBOOT_FLAGS */
        req[c++] = LE32(1);                                           /* bit 0 = tryboot */
        req[c++] = 0;
        req[0] = LE32(c << 2);
        mbox_call();
        kprintf("[AGA] tryboot flag set, rebooting into AGA mode\n");
    } else {
        kprintf("[AGA] rebooting into ECS mode\n");
    }
    pi_reset();
}
