/* AGA-PISTORM - Emu68 glue interface.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_GLUE_H
#define AGA_GLUE_H
#include <stdint.h>

extern volatile int aga_enabled;
extern int aga_enter_stop;
extern int aga_leave_stop;   /* bring-up: stop sandbox LEAVE after step N */
extern int aga_cia_mirror;  /* 0 none, 1 PORTS, 2 PORTS+EXTER */
extern int aga_keep_ciab;   /* do not silence the CIA-B sources at takeover */
extern int aga_loop_stop;    /* bring-up: limit what the chipset loop does */   /* bring-up: stop sandbox entry after step N */

/* Bring-up watchdog: seconds the chipset may run without completing a frame
   before it parks itself. Exists so a failed sandbox entry recovers instead of
   taking the machine down with it, leaving the counters unreadable. */
#define AGA_WATCHDOG_SEC 5

#define AGA_FB_RESERVE  (4u << 20)   /* three output frames + scan/crop scratch + HDMI audio */
/* its last 256 KB: the HDMI audio ring and its DMA control block (aga_hdmi.c;
   184 KB at 192 kHz on a Pi 3, 1.114 - 64 KB before); uncached and
   identity-mapped like the rest, so the DMA sees what core 3 writes */
#define AGA_HDMI_AUDIO_SIZE 0x40000u
#define AGA_HDMI_AUDIO_OFF (AGA_FB_RESERVE - AGA_HDMI_AUDIO_SIZE)

/* start.c: nakon memory setupa, prije M68K_StartEmu. chipram_va je ARM
   adresa cached, inner-shareable bloka koji je također mapiran na 68k
   0x000000; fb_va/fb_phys je cached AGA_FB_RESERVE blok za HVS output
   frameove. */
int  aga_glue_init(uint8_t *chipram_va, uint32_t size, uint8_t *fb_va, uintptr_t fb_phys, int ntsc, int enable);

/* Sandbox mode (ECS boot): prebaci running mašinu na virtualni chipset za
   WHDLoad igru i natrag. Zove se iz trap puta na control registru
   ($DFF1F2 = $5A0B enter, $5A0F leave). Return 1 = done. */
int  aga_sandbox_enter(void);
int  aga_sandbox_leave(void);
extern volatile int aga_sandbox_active;

/* Koji chipset sandbox predstavlja: $DFF1F2 = $5E00 AGA (default), $5E01 ECS
   Denise, $5E03 ECS Denise koje je user forsirao (nikad ne prebaci na AGA
   onim što igra radi). Šalje agaboot pri sandbox entryju, iz svog ECS
   togglea. */
void aga_chipset_ecs(int mode);
void aga_flickerfix(int on);         /* $59Dn: interlaced frames woven */
void aga_brightness(int level);      /* $59Fn: 0 off .. 4 (110-150%) */
void aga_gamma(int gamma);           /* $512n: 0 Exact, 1 CRT, 2 Lighter */
void aga_video_standard(int ntsc);   /* $591n: PAL or NTSC for the next entry */

/* Virtual floppy (vectors.c): CIA-B PRB writes / CIA-A PRA and SDR reads pass
   through here when aga_enabled. Return 1 = handled (value delivered). */
int  aga_cia_write(uint64_t far, int size, uint64_t value);
/* RGB2HDMI-SEL (aga_glue.c): $528n, i CIA-B PRA/DDRA writeovi dok je držan */
/* OSD i monitor line (aga_hvs.c, 1.121): key kako ga je igra pročitala
   (1 = menu ga je uzeo), $5284-$5286, i SEL_OSD-ove riječi */
int  aga_osd_key(int code, int up);
void aga_osd_shortcut(void);
int aga_osd_keyboard_active(void);
/* ...i s porta 1 (aga_pad.c ga samplea): pritisnut sad, OSDI_* */
#define OSDI_UP    1
#define OSDI_DOWN  2
#define OSDI_LEFT  4
#define OSDI_RIGHT 8
#define OSDI_A     16      /* fire, padov crveni */
#define OSDI_B     32      /* drugi button, padov plavi */
#define OSDI_SEL   64      /* USB padov Select */
#define OSDI_BOTH  (OSDI_A | OSDI_B)
void aga_osd_input(int in);
void aga_osd_run(void);                         /* vectors.c: promjena koju je pad tražio */
extern volatile int aga_osd_hide, aga_osd_pend;
void aga_pad_osd_poll(int joy1);                /* aga_pad.c: JOY1DAT kako je pročitan, ili -1 */
void aga_pad_usb_select(int down);              /* aga_usb.c: USB padov Select */
void aga_osd_command(int v);
uint16_t aga_osd_info(int i);
void aga_sel_command(int v);
int  aga_sel_write(uint64_t far, int size, uint64_t value);
int  aga_cia_read(uint64_t far, int size, uint64_t *value);
/* Prave floppy drive u sandboxu (aga_glue.c): postavljeno dok prava Paula
   vrti disk transfer za sandboxani OS. vectors.c tada zove poll na INTREQR
   i DSKBYTR readovima, izvan chipset locka. */
extern volatile int aga_dskb_pending;
void aga_disk_bridge_poll(void);
/* control register $DFF1F2 = $ADxx: disk image commands (see aga_disk_command) */
int  aga_disk_command(unsigned cmd);
uint16_t aga_disk_status(void);
/* M68k RESET instruction / ColdReboot: the machine restarts on the current
   chipset (virtual disks and window mode are kept) */
void aga_machine_reset(void);

/* vectors.c: first thing in SYSWriteValToAddr / SYSReadValFromAddr. Return 1 = handled. */
int  aga_trap_write(uint64_t far, int size, uint64_t value, uint64_t value2);
int  aga_trap_read(uint64_t far, int size, uint64_t *value, uint64_t *value2);

/* Chip RAM redirection WITHOUT the MMU. While the sandbox is up, every 68k
   access to 0..chip_size that reaches the abort handler is served from the
   virtual chip RAM here instead of going out over the PiStorm bus. Code and
   data alike: the JIT's instruction-cache line fill is a plain 16-byte load
   from the 68k address, so it faults into the same door. Return 1 = handled.
   Cheap when off: one volatile load and a compare. */
int  aga_chip_read(uint64_t far, int size, uint64_t *value, uint64_t *value2);
int  aga_chip_write(uint64_t far, int size, uint64_t value, uint64_t value2);
extern volatile int aga_chip_redirect;
extern int aga_use_mmu;      /* aga.mmu na cmdlineu: stari MMU-remap put, za A/B */
/* $5C0N na control registru: redirectaj prozor chip RAMa kroz abort handler
   s pravim chipsetom koji nastavlja raditi (1 = gornjih 64 KB, 2 = sve,
   0 = off, kopirano natrag). Izolira "posluživanje chip RAMa iz chip_va" od
   "mijenjanja chipseta", što sandbox entry radi oboje odjednom. */
int  aga_redirect_test(unsigned n);

/* start.c secondary_boot(): core 3 entry kad je aga_enabled. Nikad se ne
   vraća. */
void aga_chipset_main(void);
#ifdef FRF_VIRTUAL_AGA
void aga_virtual_cia_irq(uint16_t bit, int active);
uint8_t aga_virtual_ciaa_buttons(void);
void aga_virtual_joy_set(int port, uint16_t joydat, int fire);
void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);
int  aga_boot_insert_adf(int drive, const uint8_t *adf, uint32_t size, int wprot);
void aga_virtual_disk_prb(uint8_t prb);
int  aga_virtual_disk_pra(uint8_t *bits);
#endif

/* aga_hvs.c: HVS plane output, coexisting with the P96 VideoCore driver */
int  aga_video_init_at(uint8_t *fb_va, uintptr_t fb_phys);     /* core 0, at boot */
int  aga_video_init(void);
void aga_video_present(const uint32_t *frame, int native_active); /* AGA_OUT_W x AGA_OUT_H, 0x00RRGGBB */
void aga_video_set_window(int mode, int x, int y, int w, int h);  /* windowed presentation rectangle */
void aga_video_set_zoom(int mode);   /* full screen: 0 whole overscan, 1 720x272, 2 640x256, 3 custom */
void aga_video_cut_data(int d);      /* five more bits of a custom edge - $58C0 + E5(d) (aga_crop.h) */
void aga_video_cut_store(int slot);  /* that edge: slot & 3 L T R B, + 4 NTSC - $5870 + E3(slot) */
void aga_video_crop_screen(int slot);/* the crop screen, that standard and edge - $5880 + E3(slot) */
void aga_video_auto_window(int ok, int x0, int x1, int y0, int y1);   /* Size Auto: the frame's display window */
void aga_video_hdmi_sync(int on);    /* HDMI sync: the HDMI frame follows the emulated one - $5891 / $5890 */
void aga_video_set_sharp(int on);    /* nearest-neighbour scaling instead of the smooth kernel */
void aga_video_set_look(int look);   /* 0 sharp, 1 smooth, 2 TV (blends across) - $592L */
void aga_video_set_scale(int mode);  /* 0 4:3, 1 integer, 2 fill - $597S */
void aga_video_set_ntsc_scanlines(int v); /* $517n, v1.118 */
void aga_video_scan_select(int what);
void aga_video_scan_value(int value);
void aga_video_set_scanlines(int level);   /* 0 off .. 4 (25% steps) - $598L */
void aga_video_set_laced_scanlines(int level);   /* the same, woven interlaced frames - $59En */
void aga_video_set_mask(int level);            /* the CRT mask: 0 off, 1-4 = 25-100% dark ($510n) */
void aga_video_set_scan_shape(int shape);      /* the scanlines' shape: 0 Even, 1 Crisp, 2 Whole lines ($511n) */
void aga_video_set_border(int down, int pct);    /* TV border across / down, 0-10% - $59Bn / $59Cn */
void aga_video_set_frame(int woven, int row, int ntsc);   /* core 3, before each present */
void aga_video_set_frame_len(uint32_t cck); /* v1.118 measured HDMI frame clocks */
void aga_video_set_pos_x(int x);     /* the picture right by x hires pixels - $57nn */
void aga_video_set_pos_y(int y);     /* and down by y lines - $56nn */
void aga_video_calibrate(int on);    /* the Position screen, desktop only - $593N */
extern volatile int aga_blit_fast;   /* $5801: 1.101's fixed blit completion (A/B tests) */
void aga_blit_mode(int v);            /* $580N: bit 0 fast completion, bit 1 no held completions */
extern volatile int aga_beam_mode;   /* $581N: beam estimate lead - 0 one step, 1 none, 2 a frame */
void aga_video_hide(void);        /* take the plane off the display: sandbox leave, watchdog park */
void aga_video_probe_pointer(void); /* re-read the RTG mouse pointer into the PTR_NOW counters */
/* Debug overlay drawn into the presented frame. It is MY output, not the
   game's, so it appears even when the game draws fuck-all - which is the only
   way to read a machine whose OS a wedged game has taken away. */
void aga_video_overlay(int row, uint32_t value);
void aga_video_overlay_allow(int on);   /* boot-config gate; off means HUD cannot be enabled */
void aga_video_overlay_enable(int on);
extern volatile int aga_overlay_idle;   /* $5F0N: overlay na idle (no native display) planeu */

/* aga_audio.c: Paula audio through the real Paula (samples staged in real chip RAM) */
void aga_audio_init(const uint8_t *chipram, uint32_t chipram_size);
int  aga_audio_reg_write(uint32_t reg, uint16_t v);     /* AUDx registers and DMACON: swallowed (virtual) */
void aga_audio_stream(const int16_t *lr, int frames);   /* push mixed samples to the real Paula loop */
/* 32-bit WRITE to $DFF1EA: agaboot hands over a chip RAM buffer it has
   AllocMem'd for the Paula DMA rings, before it enters the sandbox.

   The Pi-side register block is shared, so keep this map straight:
     write $DFF1E8, 16 bit  diag selector      (aga_diag_select)
     read  $DFF1EA, 16 bit  diag data, auto-advancing
     write $DFF1EA, 32 bit  audio ring address (ovdje)
     write $DFF1EC/$1EE     ADF image address  (aga_disk_command, samo sandbox)
     write $DFF1F2, 16 bit  control word       ($5A0B/$5A0F/$5E0N/$ADxx/$A6A1/$EC50)
   Write and read of $DFF1EA are handled on different paths and different
   widths, so they do not collide - but do not add a third meaning to it. */
void aga_audio_set_ring(uint32_t base);
int16_t *aga_audio_scratch(int *max_frames);
void aga_audio_stop(void);
void aga_audio_bus_service(void);   /* housekeeper coreov udio u audiju: bus writeovi */
void aga_wedge_test(void);          /* $52ED: housekeeper namjerno zaglavi bus (agaboot WEDGETEST) */
/* isti mix na HDMI-u (aga_hdmi.c): postavljen pri bootu na coreu 0, hranjen
   iz aga_audio_stream na coreu 3, gledan iz housekeepera */
void aga_hdmi_boot(uint8_t *fb_va, uintptr_t fb_phys);
void aga_hdmi_feed(const int16_t *lr, int frames);
void aga_hdmi_service(void);
void aga_hdmi_enable(int on);       /* $5821 / $5820 from agaboot SOUND */
void aga_hdmi_filter(int mode);     /* 0 off, 1 A500, 2 A1200: $5830, $5831, $5834 (agaboot SOUND FILTER) */
void aga_hdmi_led(int lit);         /* power LED, CIA-A PRA bit 1 low: LED filter uključen */
void aga_hdmi_volume(int i);        /* 100 150 200 300 400 %: $5840 + enc(i) (agaboot SOUND VOLUME) */
void aga_hdmi_ahi_nibble(int d);    /* the AHI ring's address, four bits more: $58A0 + E4(d) */
void aga_hdmi_ahi(int on);          /* the AHI ring at that address on / off: $5895 / $5894 */
int  aga_hdmi_ahi_live(void);       /* an AHI ring is on (core 3's park runs the pump) */
void aga_hdmi_pump(void);           /* core 3, parked: the feed for the AHI ring, ~2 ms */
void aga_hdmi_rate_default(void);   /* the sandbox starts: HDMI back at 48 kHz unless an AHI ring is on */
struct aga_step;                    /* aga/chipset.h */
void aga_hdmi_paula(int bl);        /* Paula on HDMI: $5899 band-limited, $5898 classic */
void aga_hdmi_rate_mode(int m);     /* HDMI sound rate: $5868 + E2(m), 0 follow (EDID), 1 follow (any), 2 96 kHz, 3 192 kHz */
int  aga_hdmi_paula_bl(void);
/* band-limited: Paula's steps since the last call, all before colour clock now */
void aga_hdmi_feed_steps(const struct aga_step *s, int n, uint64_t now, uint32_t cck_hz);
void aga_audio_width_swap(int32_t *width16, int *swap);   /* Sound windowov stereo (aga_audio.c) */
void aga_audio_stereo(int i);       /* width 100 75 50 25 0 %: $5850 + enc(i), oba outputa (SOUND STEREO) */
void aga_audio_swap(int on);        /* left i right zamijenjeni: $5861 / $5860, oba outputa (SOUND SWAP) */
extern int aga_hdmi_audio_off;      /* cmdline aga.nohdmiaudio */

/* Timed waits that sleep (from RGB2RTG 0.75-dev48, EMM #2029). A core that
   spins on yield until a time comes runs flat out and heats the Pi: core 3,
   ahead of a game, spent most of every frame that way on a Pi 4, and core 1
   between RGB port frames. Such waits do a wfe instead, and the core's own
   timer event stream ends every wfe within about 10 us (Pi 4: 2^9 ticks of
   54 MHz, 9.5 us; Pi 3: 2^7 of 19.2 MHz, 6.7 us); a sev from another core
   ends it at once. aga_evstream_on sets the stream up - per core, once on
   each core that waits this way. OFF by default, an experiment: only
   cmdline.txt aga.sleep turns it on (1.115). In a game it saves little -
   Jim Power on a Pi 3 A+ ended half a degree cooler (49.4 against 49.9 C),
   since the 68k's core, the housekeeper and core 3's pacing steps keep
   running; core 1 sleeps through the RGB port's waits. (A crash report
   against RGB2RTG's Eco mode, which sleeps the same way, turned out to be
   a tester's own firmware.) Without it CNTKCTL_EL1 is not touched and
   every wait spins as before. A core sleeps only if its own stream is on - a wfe without it
   could wait for a sev that never comes. Lock spins stay on yield.
   A wait for a time sleeps only while more than two event periods are left
   (aga_nap_before): the stream ends a wfe up to a period late, and core 3
   paces a game in steps of 8 colour clocks, about 2.3 us - sleeping every
   one of those waits made each step last a whole 6.7 us period, and Jim
   Power ran at a quarter of its speed with its sound broken up (Pi 3 A+,
   2026-10-11). Such short waits spin as before. */
extern int aga_sleep;               /* cmdline aga.sleep */
extern uint32_t aga_nap_min;        /* timer ticks: dva event perioda, postavlja aga_evstream_on */
static inline void aga_evstream_on(void)
{
    uint64_t frq, k;
    unsigned n = 0;
    if (!aga_sleep) return;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    while (n < 15 && (4ull << n) <= frq / 100000) n++;          /* najveći n s 2^(n+1) tickova <= 10 us */
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(k));
    k = (k & ~0xFCull) | (1ull << 2) | ((uint64_t)n << 4);     /* EVNTEN; EVNTDIR 0 (bit n raste); EVNTI n */
    __asm__ volatile("msr cntkctl_el1, %0\n\tisb" :: "r"(k));
    aga_nap_min = 4u << n;                                       /* event svakih 2^(n+1) tickova: dva takva */
}
static inline void aga_nap(void)
{
    uint64_t k;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(k));
    if (aga_sleep && (k & (1ull << 2))) __asm__ volatile("wfe");    /* stream ovog corea je uključen */
    else __asm__ volatile("yield");
}
/* Jedan krug waita da timer dođe do deadlinea: spava samo dok je ostalo
   više od dva event perioda, inače spin */
static inline void aga_nap_before(uint64_t deadline)
{
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    if ((int64_t)(deadline - t) > (int64_t)aga_nap_min) aga_nap();
    else __asm__ volatile("yield");
}
extern int aga_hdmi_force;          /* cmdline aga.hdmiforce: preskoči HDMI clock check */

/* aga_hvs.c: reboot the Pi into AGA (tryboot.txt, one shot) or ECS (config.txt) */
void aga_reboot(int tryboot);

/* ---- diagnostics (aga_diag.c) ---------------------------------------------
   Counteri su na frame granularnosti ili rjeđe, pa ostaju u shipped buildu;
   per-trap oni trebaju AGA_DIAG i inače se compileaju van. */
enum {
    AGA_D_FRAMES = 0, AGA_D_FRAME_US, AGA_D_FRAME_US_WORST, AGA_D_LAG_MAX,
    AGA_D_RESYNCS, AGA_D_AUDIO_STREAMS, AGA_D_AUDIO_RESYNCS,
    AGA_D_PRESENTS, AGA_D_VIDEO_ERRORS, AGA_D_SANDBOX_IN, AGA_D_SANDBOX_OUT,
    AGA_D_DISK_CMDS, AGA_D_REBOOTS, AGA_D_TRAPS_R, AGA_D_TRAPS_W, AGA_D_STATE,
    AGA_D_BPL_FRAMES,   /* frameovi u kojima je nešto stvarno imalo bitplane uključene */
    /* interrupt delivery: pretvori li se IPL koji digne virtualni chipset ikad
       u 68k exception? ipl_hook broji dizanja; irq_l1..7 broje exceptione
       koje je execution loop stvarno uzeo, po levelu, iz BILO KOJEG izvora;
       irq_from_virtual broji one gdje je virtualni chipsetov bajt pobijedio. */
    AGA_D_IPL_HOOK, AGA_D_IRQ_VIRT,
    AGA_D_IRQ_L1, AGA_D_IRQ_L2, AGA_D_IRQ_L3, AGA_D_IRQ_L4, AGA_D_IRQ_L5, AGA_D_IRQ_L6, AGA_D_IRQ_L7,
    /* the REAL chipset's registers, sampled once per frame by the chipset core
       while the sandbox is up. The 68k cannot read them itself there (its reads
       are the virtual ones), and "level 2 stops after three seconds" needs to
       know whether the real Paula still has PORTS enabled and pending. */
    AGA_D_REAL_INTENA, AGA_D_REAL_INTREQ, AGA_D_REAL_DMACON,
    /* in-game blank screen: puni li išta bitplane uopće? */
    AGA_D_BLITS,        /* blitovi startani od reseta */
    AGA_D_DATA_FRAMES,  /* frameovi u kojima je bitplane fetch vratio non-zero data */
    /* 68k promet kroz chip RAM redirect, i GDJE piše: osam 256 KB bucketa
       preko 2 MB. Displayov BPLxPT (najdublji snapshot) kaže odakle chipset
       fetcha; ovi kažu gdje je igra stvarno stavila bajtove. Blank screen s
       writeovima u pogrešnom bucketu je address problem; nijedan write je
       loading problem. */
    AGA_D_CHIP_R, AGA_D_CHIP_W,
    AGA_D_CHIPW_B0, AGA_D_CHIPW_B1, AGA_D_CHIPW_B2, AGA_D_CHIPW_B3,
    AGA_D_CHIPW_B4, AGA_D_CHIPW_B5, AGA_D_CHIPW_B6, AGA_D_CHIPW_B7,
    /* Po audio kanalu: koliko puta je igra uključila njegov DMA, i koliko je
       sample riječi stvarno fetchala. Glazba svira a efekti uglavnom ne, i
       nijedan postojeći counter ne razlikuje to dvoje - ovi kažu je li efekt
       kanal ikad STARTAN (igra koja ga nikad ne starta je drugi bug od one
       čiji se zvuk starta pa izgubi). */
    AGA_D_AUD0_ON, AGA_D_AUD1_ON, AGA_D_AUD2_ON, AGA_D_AUD3_ON,
    AGA_D_AUD0_W,  AGA_D_AUD1_W,  AGA_D_AUD2_W,  AGA_D_AUD3_W,
    AGA_D_AUD_NUDGE,      /* one-sample drift corrections: never counted until now */
    /* Split by DIRECTION, which is the whole diagnosis: dropping samples makes
       the music play fast, repeating them makes it play slow. 92 nudges a
       second under gameplay against 1.5 on the desktop is a 0.33% rate error,
       far more than two crystals can differ by. */
    AGA_D_AUD_DROP, AGA_D_AUD_REPEAT,
    /* Colour clocks each channel spent playing a NON-ZERO sample. Words fetched
       proves nothing: the standard one-shot idiom plays a sound and then parks
       the channel on a two-word silent loop, so a channel can fetch millions of
       words that are all zeros. This says whether a channel makes any SOUND. */
    AGA_D_AUD0_NZ, AGA_D_AUD1_NZ, AGA_D_AUD2_NZ, AGA_D_AUD3_NZ,
    AGA_D_AUD0_VOL, AGA_D_AUD1_VOL, AGA_D_AUD2_VOL, AGA_D_AUD3_VOL,
    AGA_D_AUD0_VMAX, AGA_D_AUD1_VMAX, AGA_D_AUD2_VMAX, AGA_D_AUD3_VMAX,
    AGA_D_AUD0_AUD, AGA_D_AUD1_AUD, AGA_D_AUD2_AUD, AGA_D_AUD3_AUD,
    AGA_D_AUD0_VWR, AGA_D_AUD1_VWR, AGA_D_AUD2_VWR, AGA_D_AUD3_VWR,
    AGA_D_AUD0_VNZ, AGA_D_AUD1_VNZ, AGA_D_AUD2_VNZ, AGA_D_AUD3_VNZ,
    AGA_D_AUD0_OFF, AGA_D_AUD1_OFF, AGA_D_AUD2_OFF, AGA_D_AUD3_OFF,
    AGA_D_AUD0_WRAP, AGA_D_AUD1_WRAP, AGA_D_AUD2_WRAP, AGA_D_AUD3_WRAP,
    AGA_D_AUD_INTENA,     /* virtualni INTENA, da vidim je li AUDx uopće enabled */
    /* Riječi display liste koje su već bile u upotrebi kad sam posudio
       regiju LIST_A..KERNEL_WORD. Non-zero znači da je RTG driver tamo
       alocirao - tako je mouse pointer postao nevidljiv dok je još trackao. */
    AGA_D_DL_INUSE,
    /* najduži niz nula-riječi u display-list RAMu, i gdje počinje: gdje bi
       moje liste mogle živjeti bez da stanu na RTG driver. */
    AGA_D_DL_FREE_AT, AGA_D_DL_FREE_LEN,
    /* Mouse pointer kako ga je RTG driver izgradio, pročitan iz driverove
       vlastite žive display liste: plane 2 te liste je pointer (VideoCore.card
       vc6.c VC6_SetPanning ga gradi odmah nakon screen planea). Snimljen pri
       sandbox entryju (IN), pri izlasku u hide() (OUT), i uživo kad god alat
       počne diagnostic dump (NOW) - pa agastat pokrenut dok je pointer
       nevidljiv pokaže što je na ekranu.
         LIST   DISPLIST1 vrijednost iz koje je lista pročitana
         PLANES planeovi prije terminatora (screen, pointer, overlay)
         CTL    plane 2 control word (0 = nema drugog planea)
         POS0   njegova position riječ; 0x2FFF2FFF = parkiran od
                VC6_SetSprite(FALSE)
         IMAGE  njegov PTR0: 0xC0000000 | Amiga adresa vc4_SpriteShape
         PAL    prva pointer boja, alpha u gornjem bajtu (0 = transparent) */
    AGA_D_PTR_IN_LIST,  AGA_D_PTR_IN_PLANES,  AGA_D_PTR_IN_CTL,
    AGA_D_PTR_IN_POS0,  AGA_D_PTR_IN_IMAGE,   AGA_D_PTR_IN_PAL,
    AGA_D_PTR_OUT_LIST, AGA_D_PTR_OUT_PLANES, AGA_D_PTR_OUT_CTL,
    AGA_D_PTR_OUT_POS0, AGA_D_PTR_OUT_IMAGE,  AGA_D_PTR_OUT_PAL,
    AGA_D_PTR_NOW_LIST, AGA_D_PTR_NOW_PLANES, AGA_D_PTR_NOW_CTL,
    AGA_D_PTR_NOW_POS0, AGA_D_PTR_NOW_IMAGE,  AGA_D_PTR_NOW_PAL,
    /* frameov time budget na chipset coreu (2026-09-13, za Pi 3 pitanje):
       busy = frame minus pacing wait, u us; audio = vrijeme unutar
       aga_audio_stream, tj. PiStorm bus */
    AGA_D_LOOP_BUSY_US, AGA_D_LOOP_BUSY_WORST, AGA_D_LOOP_WAIT_US,
    AGA_D_AUDIO_US_FRAME, AGA_D_AUDIO_US_WORST,
    AGA_D_LOCK_US_FRAME, AGA_D_LOCK_US_WORST,   /* core 3 čeka chipset lock */
    AGA_D_SOC_TEMP_MC, AGA_D_THROTTLED,         /* firmware: temperatura, throttle bitovi, pri leaveu */
    AGA_D_BUS_US_FRAME, AGA_D_BUS_US_WORST,     /* tri real-register reada po frameu */
    AGA_D_CYC_US_FRAME, AGA_D_CYC_US_WORST,     /* aga_run_cycles: po frameu, najgori pojedinačni chunk */
    AGA_D_STAT_US_WORST, AGA_D_BODY_US_WORST,   /* frame statistics blok; cijelo tijelo između dva waita */
    AGA_D_CORE3_DAIF,                           /* interrupt mask bitovi na coreu 3 */
    AGA_D_CYC_FRAME_WORST,                      /* najgori per-frame zbroj aga_run_cycles */
    AGA_D_CHIPW_FRAME, AGA_D_CHIPW_FRAME_WORST, /* 68k chip RAM writeovi po frameu (AGA_DIAG buildovi) */
    AGA_D_MEM_US, AGA_D_MEM_US_WORST,           /* DRAM latency probe, po frameu */
    AGA_D_CHIP_REAL_KB,                         /* pravi chip RAM nađen pri zadnjem entryju */
    AGA_D_ABORT_US_FRAME, AGA_D_ABORT_US_WORST, /* core 0 unutar chip RAM aborta, po frameu */
    AGA_D_ABORTS_FRAME, AGA_D_ABORTS_WORST,     /* koliko ih je po frameu */
    AGA_D_L1_REFILL_FRAME, AGA_D_L1_REFILL_WORST, /* core 3 PMU: L1 data cache refillovi po frameu */
    AGA_D_L2_REFILL_FRAME, AGA_D_L2_REFILL_WORST, /* core 3 PMU: L2 refillovi po frameu */
    /* HVS tri sekunde nakon hand-backa, samplano svakih 20 ms od
       housekeeper corea (Pi 3 crni ekran, 2026-09-14) */
    AGA_D_WATCH_SAMPLES, AGA_D_WATCH_LIST_CHANGES, AGA_D_WATCH_STAT_MOVES,
    AGA_D_WATCH_LACT_FIRST, AGA_D_WATCH_LIST_LAST, AGA_D_WATCH_LACT_LAST,
    AGA_D_WATCH_STAT_LAST, AGA_D_WATCH_CTL_LAST, AGA_D_WATCH_PTR0_LAST,
    AGA_D_WATCH_LATCH_US,                       /* hide: koliko je trebalo da HVS latcha driverovu listu */
    AGA_D_FRAMES_SKIPPED,                       /* Pi 3: frameovi nenacrtani jer je loop bio iza */
    /* 3 ili 4: Pi za koji je ovaj kernel buildan, i Pi koji je našao. agaboot
       ih čita PO INDEXU (147, 148) za AGA toggleov reinstall warning;
       aga_diag.c asserta indexe, pa dodaj nove countere nakon ovih. */
    AGA_D_BUILD_BOARD, AGA_D_RUN_BOARD,
    /* prezentirani frameovi po tome koliko je loop bio iza realnog vremena:
       <2, 2-5, 5-10, 10-20, 20-40, >=40 ms */
    AGA_D_LAG_HIST0, AGA_D_LAG_HIST1, AGA_D_LAG_HIST2,
    AGA_D_LAG_HIST3, AGA_D_LAG_HIST4, AGA_D_LAG_HIST5,
    /* čitanje broja linije kao dva bajta: koliko parova, koliko ih je imalo
       promjenu linije između polovica, i najgori gap u mikrosekundama */
    AGA_D_BEAM_PAIRS, AGA_D_BEAM_PAIR_SKIPS, AGA_D_BEAM_PAIR_US,
    /* beam s obje strane: kroz što core 3 vodi chipset, što vidi trapani
       read, koliko read interpolira, i geometrija koju oba koriste */
    AGA_D_LOOP_VPOS_MAX, AGA_D_TRAP_VPOS_MAX, AGA_D_TRAP_ADD_MAX,
    AGA_D_CORE_LINES, AGA_D_CORE_MAXHPOS,
    /* PAL/NTSC: koliko se često frame mijenjao veličinom, između čega, i
       BEAMCON0 writeovi koje je sam 68k napravio */
    AGA_D_GEOM_CHANGES, AGA_D_GEOM_FROM, AGA_D_GEOM_TO,
    AGA_D_GEOM_MIN, AGA_D_GEOM_MAX,
    AGA_D_BEAMCON0_W, AGA_D_BEAMCON0_LAST, AGA_D_BEAMCON0_NTSC_W, AGA_D_BEAMCON0_PC,
    /* kad je frame promijenio veličinu i tko je radio: unutar igre ili na
       desktopu, koji frame, i što je BEAMCON0 držao u tom trenutku */
    AGA_D_GEOM_FRAME, AGA_D_GEOM_BEAMCON0, AGA_D_GEOM_SANDBOX,
    AGA_D_BEAMCON0_NTSC_SBX, AGA_D_BEAMCON0_NTSC_FRAME, AGA_D_BEAMCON0_COP,
    /* $DFF005 je gdje živi bit 8 broja linije, i igra ga čeka s btst #0:
       svaki bit koji sam ikad tamo vratio, i koliko puta */
    AGA_D_BEAM005_OR, AGA_D_BEAM005_READS, AGA_D_BEAM005_SET,
    /* bitplane DMA: igra s ugašenim displayom crta u chip RAM i ne pokazuje
       ništa, pa je KADA je BPLEN otišao - i tko ga je ugasio - cijelo pitanje */
    AGA_D_DMACON_NOW, AGA_D_BPLEN_CHANGES, AGA_D_BPLEN_OFF_FRAME,
    AGA_D_BPLEN_ON_FRAME, AGA_D_DMACON_W, AGA_D_DMACON_OFF_W,
    AGA_D_DMACON_OFF_PC, AGA_D_DMACON_OFF_VAL, AGA_D_DMACON_LAST_VAL,
    /* the display is on and fetches nothing: so is the memory it points at
       empty (the game draws into the other buffer of a pair and I never
       follow the damn flip), or is the window shut? */
    AGA_D_BPL1PT_NOW, AGA_D_BPL1PT_CHANGES, AGA_D_BPL1_NONZERO,
    AGA_D_BPL1PT_MIN, AGA_D_BPL1PT_MAX,
    AGA_D_DIWSTRT_NOW, AGA_D_DIWSTOP_NOW, AGA_D_DDFSTRT_NOW, AGA_D_DDFSTOP_NOW,
    AGA_D_BPLCON0_NOW,
    /* zadnjih osam DMACON writeova: vrijednost, tko ga je pisao, koji frame */
    AGA_D_DMALOG0, AGA_D_DMALOG1, AGA_D_DMALOG2, AGA_D_DMALOG3,
    AGA_D_DMALOG4, AGA_D_DMALOG5, AGA_D_DMALOG6, AGA_D_DMALOG7,
    /* fetch odluke zadnjeg framea, jedan bucket po razlogu */
    AGA_D_FETCH_EVAL, AGA_D_FETCH_NOWIN, AGA_D_FETCH_NODMA,
    AGA_D_FETCH_NOPLANES, AGA_D_FETCH_TOOMANY, AGA_D_FETCH_OK,
    /* heartbeat: ako se mašina zaustavi sa zaglavljenom glazbom, chipset
       core je također stao, i ovi kažu gdje je bio kad je stao */
    AGA_D_CHUNK_SEQ, AGA_D_CHUNK_VPOS, AGA_D_CHUNK_HPOS,
    AGA_D_LAST_TRAP_REG, AGA_D_LAST_TRAP_VAL, AGA_D_LAST_TRAP_RW,
    AGA_D_BC0LOG0, AGA_D_BC0LOG1, AGA_D_BC0LOG2, AGA_D_BC0LOG3,
    AGA_D_BC0LOG4, AGA_D_BC0LOG5, AGA_D_BC0LOG6, AGA_D_BC0LOG7,
    /* 68k je prestao razgovarati s chipsetom: kada, i u kojem frameu */
    AGA_D_CPU_STALL, AGA_D_CPU_STALL_FRAME,
    /* at the stall: the 68k status register, what the hell I last asked of it,
       and what the chipset thinks is pending and enabled */
    AGA_D_STALL_SR, AGA_D_STALL_INTF, AGA_D_STALL_INTENA,
    AGA_D_STALL_INTREQ, AGA_D_STALL_IPL,
    AGA_D_CIA_WAKE,   /* pravi CIA interrupti mirrorani u virtualni INTREQ */
    AGA_D_STALL_INSNS,/* 68k instrukcije retired, samplane pri stallu */
    AGA_D_STALL_IRQCHK, /* level<<16 | mask<<8 | INTF.AGA, iz Emu68-ove vlastite provjere */
    AGA_D_ARM_HELD,   /* Emu68 ARM interrupti (USB, mreža) držani od igre */
    /* igra koja polla INTREQR: gdje (JIT blok u kojem je), zadnja vrijednost
       koja joj je dana, i koji bitovi su joj ikad / uvijek pokazani */
    AGA_D_INTREQR_PC, AGA_D_INTREQR_VAL, AGA_D_INTREQR_OR, AGA_D_INTREQR_AND,
    AGA_D_INTREQR_N,
    AGA_D_MOUSE_QUIT, /* koliko puta su oba mouse buttona tražila WHDLoad da izađe */
    /* sandboxani program čita INTREQR: koliko često je EXTER došao od prave
       Paule (CIA-B interrupt) i koliko često od Emu68-ovog vlastitog ARM
       interrupta */
    AGA_D_EXTER_REAL, AGA_D_EXTER_ARM,
    AGA_D_AUDIRQ_DATIDLE, AGA_D_AUDIRQ_WRAP, AGA_D_AUDIRQ_FIRST,
    AGA_D_AUDIRQ_MANUAL, AGA_D_AUDIRQ_PLAYOUT,
    AGA_D_REAL_INTENA_FIX, AGA_D_REAL_INTENA_STRAY,  /* real Paula enables put right */
    /* the CPU writing BPLxPT in a game: how many, how many while the display
       was being fetched (vertical window open), the last line and the latest
       line one arrived on, and where in the frame they land, 40 lines a bucket */
    AGA_D_BPLPT_W, AGA_D_BPLPT_W_LATE, AGA_D_BPLPT_LINE_LAST, AGA_D_BPLPT_LATE_MAX,
    AGA_D_BPLPT_H0, AGA_D_BPLPT_H1, AGA_D_BPLPT_H2, AGA_D_BPLPT_H3,
    AGA_D_BPLPT_H4, AGA_D_BPLPT_H5, AGA_D_BPLPT_H6, AGA_D_BPLPT_H7,
    /* blits finished, acknowledges that swallowed a completion, frame of the last */
    AGA_D_BLIT_FIN, AGA_D_BLIT_LOST, AGA_D_BLIT_LOST_FRAME, AGA_D_BLIT_HELD,
    /* real-bus writeovi koje je 68k trap put napravio dok drži chipset lock:
       mikrosekunde po frameu, najgori pojedinačni, i koliko ih je */
    AGA_D_TRAPBUS_US_FRAME, AGA_D_TRAPBUS_US_WORST, AGA_D_TRAPBUS_N_FRAME,
    /* koliko dugo je trap put držao chipset lock: po frameu, najgori hold */
    AGA_D_TRAPHOLD_US_FRAME, AGA_D_TRAPHOLD_US_WORST,
    /* writeovi forwardani pravim chipovima, po registru, i koliko bi ih tamo
       promijenilo ništa (1.105 mjerenje prije preskakanja ijednog) */
    AGA_D_FWD_INTENA, AGA_D_FWD_INTENA_NOP, AGA_D_FWD_INTREQ, AGA_D_FWD_ADKCON,
    AGA_D_FWD_ADKCON_NOP, AGA_D_FWD_POTGO, AGA_D_FWD_POTGO_NOP, AGA_D_FWD_JOYTEST,
    AGA_D_FWD_SER, AGA_D_FWD_DSK, AGA_D_FWD_OTHER,
    /* PiStorm bus mutex po coreu (0 68k, 1 serial, 2 housekeeper, 3 chipset):
       SESSION totali - mikrosekunde čekane, mikrosekunde držane, akvizicije -
       i najgori pojedinačni wait. Podijeli s frameovima za per-frame brojke. */
    AGA_D_BUSW_US_C0, AGA_D_BUSW_US_C1, AGA_D_BUSW_US_C2, AGA_D_BUSW_US_C3,
    AGA_D_BUSH_US_C0, AGA_D_BUSH_US_C1, AGA_D_BUSH_US_C2, AGA_D_BUSH_US_C3,
    AGA_D_BUSN_C0, AGA_D_BUSN_C1, AGA_D_BUSN_C2, AGA_D_BUSN_C3,
    AGA_D_BUSW_WORST_US_C0, AGA_D_BUSW_WORST_US_C2, AGA_D_BUSW_WORST_US_C3,
    /* 68k-ovi chip RAM pristupi kroz abort handler: session totali */
    AGA_D_ABORT_US_TOTAL, AGA_D_ABORTS_TOTAL,
    /* SoC status mailbox call pri sandbox leaveu, sirovo: napravljeni callovi,
       response code zadnjeg ($80000000 = success), i temperatura i throttle
       flagovi uzeti pri sandbox ENTRYju za usporedbu */
    AGA_D_SOC_CALLS, AGA_D_SOC_CODE, AGA_D_SOC_TEMP_ENTER_MC, AGA_D_SOC_THROTTLED_ENTER,
    /* level-6 storms broken (exter_storm_check) and the CIA-B ICR the last one read */
    AGA_D_EXTER_STORMS, AGA_D_EXTER_STORM_ICR,
    /* moji mailbox callovi: odgovori koji nisu bili moji (preskočeni),
       callovi koji su timeoutali */
    AGA_D_MBOX_FOREIGN, AGA_D_MBOX_TIMEOUTS,
    /* 128-bitni register pristupi (MOVEM od četiri ili više longworda), i svaka
       veličina koju trap još ne zna */
    AGA_D_TRAP_W16, AGA_D_TRAP_R16, AGA_D_TRAP_ODD_SIZE,
    /* 68k-ovi BPLCON0 writeovi: koliko, i zadnjih osam kao vrijednost i PC */
    AGA_D_BC0W_N,
    AGA_D_BC0W_VAL0, AGA_D_BC0W_VAL1, AGA_D_BC0W_VAL2, AGA_D_BC0W_VAL3,
    AGA_D_BC0W_VAL4, AGA_D_BC0W_VAL5, AGA_D_BC0W_VAL6, AGA_D_BC0W_VAL7,
    AGA_D_BC0W_PC0, AGA_D_BC0W_PC1, AGA_D_BC0W_PC2, AGA_D_BC0W_PC3,
    AGA_D_BC0W_PC4, AGA_D_BC0W_PC5, AGA_D_BC0W_PC6, AGA_D_BC0W_PC7,
    /* copper: restart pointovi (najnoviji prvi) sa svojim frameovima, restart
       promjene, i njegovi vlastiti BPLCON0 writeovi */
    AGA_D_COPRS_A0, AGA_D_COPRS_A1, AGA_D_COPRS_A2, AGA_D_COPRS_A3,
    AGA_D_COPRS_A4, AGA_D_COPRS_A5, AGA_D_COPRS_A6, AGA_D_COPRS_A7,
    AGA_D_COPRS_F0, AGA_D_COPRS_F1, AGA_D_COPRS_F2, AGA_D_COPRS_F3,
    AGA_D_COPRS_F4, AGA_D_COPRS_F5, AGA_D_COPRS_F6, AGA_D_COPRS_F7,
    AGA_D_COPRS_N, AGA_D_COPBC0_N, AGA_D_COPBC0_PC, AGA_D_COPBC0_VAL, AGA_D_COPBC0_FRAME,
    /* 68k-ovi writeovi u COP1LC/COP2LC/COPJMP1/COPJMP2: ring od osam kao
       reg<<16|value, s PC i frameom */
    AGA_D_COPW_N,
    AGA_D_COPW_RV0, AGA_D_COPW_RV1, AGA_D_COPW_RV2, AGA_D_COPW_RV3,
    AGA_D_COPW_RV4, AGA_D_COPW_RV5, AGA_D_COPW_RV6, AGA_D_COPW_RV7,
    AGA_D_COPW_PC0, AGA_D_COPW_PC1, AGA_D_COPW_PC2, AGA_D_COPW_PC3,
    AGA_D_COPW_PC4, AGA_D_COPW_PC5, AGA_D_COPW_PC6, AGA_D_COPW_PC7,
    AGA_D_BC0W_FRAME,
    /* BPLCON0 event log: count, pa 48 eventova kao A,B parovi */
    AGA_D_EVLOG_N, AGA_D_EVLOG0, AGA_D_EVLOG_LAST = AGA_D_EVLOG0 + 95,
    /* JIT uniti prevedeni dok je sandbox gore, i oni od njih čiji je kod u
       chip RAMu; i je li ova sesija čitala chip RAM kod mimo Emu68-ovog
       softverskog cachea (aga_code_direct, $594N) */
    AGA_D_JIT_NEW, AGA_D_JIT_NEW_CHIP, AGA_D_CODE_DIRECT,
    /* agaboot BUSTEST: mode zadnjeg runa, pomaknuti bajtovi, ukupno vrijeme i
       najsporija pojedinačna transakcija - što pravi bus može nositi za sliku
       nacrtanu u pravi chip RAM (HAM6 na RGB portu) */
    AGA_D_BUSTEST_MODE, AGA_D_BUSTEST_BYTES, AGA_D_BUSTEST_US, AGA_D_BUSTEST_WORST_NS,
    /* HAM6 on the RGB port (aga_rgbout.c, core 1): state (0 off, 1 running,
       2 no core 1, 3 block refused, 4 Position frame, 5 black: the OS's own
       display held back in a WHDLoad game), frames shown, frames a second x10 over
       the last second, words written for the last frame and KB in all, the
       last frame's snapshot / encode / write / flip-wait times, snapshots
       lost to a page core 3 was already redrawing, palette changes, and
       presented frames that went by while core 1 was busy */
    AGA_D_RGB_STATE, AGA_D_RGB_FRAMES, AGA_D_RGB_FPS10, AGA_D_RGB_WORDS_LAST, AGA_D_RGB_KB_TOTAL,
    AGA_D_RGB_SNAP_US, AGA_D_RGB_ENCODE_US, AGA_D_RGB_WRITE_US, AGA_D_RGB_WAIT_US,
    AGA_D_RGB_TORN, AGA_D_RGB_PALETTES, AGA_D_RGB_SKIPPED,
    /* ...i Position offseti (x << 16 | y, svaki 16-bit signed), veličina slike
       (width << 16 | lines), k-means runovi, i 1 dok je HDMI crn za RGB port */
    AGA_D_RGB_WIN_POS, AGA_D_RGB_WIN_SIZE, AGA_D_RGB_KMEANS, AGA_D_RGB_HDMI_OFF,
    /* HDMI audio (aga_hdmi.c): state (0 off, 1 running, 2 nema HDMI linka ili
       trenutno nema HDMI slike, 3 nema HDMI clocka pri bootu - na Pi 3 i HSM
       zaustavljen ili ne iz PLLD, frekvencija nepoznata - 4 packet slot zauzet, 5
       nema slobodnog DMA kanala, 6 neiskorišten od 1.112 (bio: ne na Pi 3),
       7 aga.nohdmiaudio, 8 drugi driverov MAI enabled; 2, 4, 5 i 8 - i 3 na
       Pi 3 - pokušavaju se opet jednom u sekundi dok se zvuk želi, 5 samo dok
       je još koji kanal slobodan), DMA kanal + 1, lead ispred DMA u 48 kHz
       frameovima i rate trim u ppm (signed), resyncovi, koliko puta je ring
       utihnuo jer je mix stao, i popravci: registri vraćeni nakon što ih je
       firmware ispustio, infoframe nije poslan, DMA error, kanal uzet */
    AGA_D_HDMI_STATE, AGA_D_HDMI_DMA_CH, AGA_D_HDMI_LEAD, AGA_D_HDMI_PPM,
    AGA_D_HDMI_RESYNCS, AGA_D_HDMI_IDLES, AGA_D_HDMI_FIXUPS,
    /* ...i HDMI state machine clock pri bootu u kHz, izmjeren i kako ga je ARM
       postavio (drugi je 0 pod Emu68: nitko ga nije postavio); na Pi 3 MAI
       clock iz CPRMANovog HSM i njegov sirovi CM_HSMCTL, držan ažurnim */
    AGA_D_HDMI_HSM_KHZ, AGA_D_HDMI_HSM_SET_KHZ,
    /* ...i pogledi jednom u sekundi koji su našli MAI error flagove postavljene
       (late data, underflow, overflow) i clearali ih */
    AGA_D_HDMI_MAI_ERRS,
    /* ...i status HVS kanala 1 kako ga je HDMI guard zadnje samplao (maskiran
       na polja kanala 1), i promjene koje je vidio: penje se oko 1000 u
       sekundi dok se slika miče i zvuk je uključen ili željen */
    AGA_D_HDMI_SCAN_STAT, AGA_D_HDMI_SCAN_MOVES,
    /* ...i filter odabran za njega: 0 off, 1 A500, 2 A1200, +16 dok je power
       LED upaljen; postavlja se jednom po frameu dok sandbox radi */
    AGA_D_HDMI_FILTER,
    /* ...i Sound windowov mix na oba outputa: stereo width u % (bitovi 0-7),
       +256 sa zamijenjenim left i right; postavlja se pri svakom streamu */
    AGA_D_AUDIO_MIX,
    /* ...i custom crop (aga_hvs.c crop_state): bit 0 understood, bit 1 crop
       screen pokazuje sliku zadnje igre, bit 2 frame koji se reže je NTSC,
       bit 3 postoji slika zadnje igre, bit 4 ona je NTSC */
    AGA_D_CROP,
    /* ...i Size Autov window, kako je praćen: x << 16 | width, y << 16 |
       height (+$8000 uzet u NTSC frameu), hires pixeli i linije u frameu;
       zadnje igre ostaju dok sljedeća ne pokaže svoje */
    AGA_D_AUTO_X, AGA_D_AUTO_Y,
    /* ...and HDMI sync (aga_hvs.c, aga_sync.h): bit 0 on, bit 1 the PixelValve
       good / bit 2 refused, bits 4-5 reach + 1 (0 out of reach, 1 learning,
       2 steering), bits 8-15 the porch lines over the mode's own, bits 16-31
       the last phase (HVS line, $FFFF none); the need, lines 16.16; frames
       repeated or dropped since it began; the mode's porch << 16 | its active height */
    AGA_D_HDMI_SYNC, AGA_D_HDMI_SYNC_NEED, AGA_D_HDMI_SYNC_SLIPS, AGA_D_HDMI_SYNC_PV,
    /* ...i AHI ring (aga_hdmi.c, tools/ahi): bit 0 understood, bit 1 ring
       uključen, bit 2 HDMI zvuk radi, bitovi 8-15 frameovi koji čekaju / 64,
       bitovi 16-31 frameovi u kojima je ring bio prazan od kad je počeo.
       Zadnji od 512 countera koje agaboot i agastat mogu čitati: ono što
       dolazi sljedeće treba novi selector range. */
    AGA_D_AHI,
    AGA_DIAG_COUNT
};
extern uint32_t aga_diag_ctr[AGA_DIAG_COUNT];
void aga_diag_init(void);
void aga_diag_set(int idx, uint32_t v);
void aga_diag_inc(int idx);
void aga_diag_max(int idx, uint32_t v);
void aga_diag_beam_byte(uint8_t v, uint32_t us, int is_hpos);
void aga_diag_beam_line(uint16_t line, uint32_t us);
void aga_diag_beam_reset(void);
void aga_diag_irq(int level, int from_virtual);   /* ExecutionLoop.c: an exception was taken */
void aga_diag_jit_new(uint32_t m68k_addr);        /* M68k_Translator.c: a unit is being translated */
void aga_set_code_direct(int on);                 /* $594N: chip RAM code past the soft cache, from the next game */
void aga_bustest_set_buffer(uint32_t addr);       /* $DFF1EC (long): agaboot's chip RAM for BUSTEST */
void aga_bustest_run(int mode);                   /* $596N: one timed run over it */
/* HAM6 on the Amiga's own RGB port while a game runs (aga_rgbout.c): two
   384 x 286 six-plane frames (the whole overscan area) and their copper lists */
#define AGA_RGB_BLOCK_BYTES_MAX (2 * 6 * 24 * 2 * 286 + 1024) /* FRF: only DOUBLE/FULL needs the maximum */
#define AGA_RGB_BLOCK_BYTES AGA_RGB_BLOCK_BYTES_MAX              /* compatibility for 1.114 diagnostics */
void aga_rgbout_main(void);                       /* core 1, never returns */
void aga_rgbout_set_block(uint32_t addr);         /* $DFF1F4 (long), outside the sandbox: the chip RAM, 0 = off */
void aga_rgbout_set_os_list(uint32_t addr);       /* $DFF1F8 (long), outside the sandbox: GfxBase->copinit */
void aga_rgbout_set_hdmi_off(int off);            /* $5950/$5951: the next game on the RGB port only, HDMI black */
void aga_rgbout_set_offset(int axis, int v);      /* $54nn (0, right) / $55nn (1, down): the RGB picture's position */
void aga_rgbout_set_size(int s);                  /* $599S: 0 384x286, 1 368x280, 2 352x272, 3 320x256 */
void aga_rgbout_set_fps25(int on);                /* $59A1 / $59A0: a steady 25 fps, or as many as get through */
void aga_rgbout_set_doublebuf(int on);            /* FRF $59A3/$59A2: DOUBLE / SINGLE physical HAM buffer */
/* v1.115 framebuffer delivery and scheduling ABI: retain FRF's buffer policy. */
void aga_rgbout_set_auto(int on);
void aga_rgbout_set_c32(int on);
void aga_rgbout_bus_service(void);

void aga_rgbout_set_dither(int on);               /* $59A5 / $59A4: the colours dithered into 4 bits, or rounded */
void aga_rgbout_calibrate(int on);                /* $5953/$5952: agaboot's Position frame on the RGB port */
int  aga_rgbout_hdmi_off(void);                   /* core 3: this game is RGB port only */
void aga_rgbout_start(uint32_t chip_real);        /* sandbox entry, once the chipset loop runs */
void aga_rgbout_stop(void);                       /* sandbox leave, before chip RAM is copied back */
/* core 3: this frame was just presented - AGA_OUT_H rows AGA_OUT_W wide,
   `pitch` words apart (twice AGA_OUT_W: a woven frame's own field) */
void aga_rgbout_frame(const uint32_t *frame, int pitch, int page);   /* page -1: a woven frame */
void aga_rgbout_line(int y, int page, const uint32_t *line);          /* core 3, each drawn line */
void aga_rgbout_set_tone(const uint8_t *inv, int on, int key);   /* the picture's Brightness and Gamma, taken back out for the RGB port */
/* core 1, from aga_glue.c: where the running copper list starts (COP1LC as
   last written, 0 with copper DMA off), whether WHDLoad has taken the machine,
   and a word of the virtual chip RAM */
uint32_t aga_glue_copper_list(void);
int  aga_glue_game_taken(void);
uint16_t aga_glue_chip_word(uint32_t addr);
void aga_diag_log(const char *line);

/* Pi 3 (VideoCore IV, 19.2 MHz generic timer) protiv Pi 4 / CM4 (54 MHz) -
   isti test koji koriste Emu68 i aga_hvs.c. Dvije ploče dobiju različite
   tolerancije gdje Pi 3 manji cluster treba. */
static inline int aga_is_pi3(void)
{
    uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
    return f <= 20000000;
}

/* 1 u Pi 3 kernelu (cmake -DAGA_PI3=ON), 0 u Pi 4. Compile-time konstanta,
   pa `if (AGA_PI3_BUILD)` ne ostavlja ništa u Pi 4 kernelu. */
#ifdef AGA_PI3
#define AGA_PI3_BUILD 1
#else
#define AGA_PI3_BUILD 0
#endif
/* Pi 3 tolerancije vrijede samo gdje se oba slažu: Pi 3 kernel na Pi 3.
   Pi 3 kernel na Pi 4 ponaša se kao Pi 4 kernel. */
static inline int aga_pi3_mode(void) { return AGA_PI3_BUILD && aga_is_pi3(); }

/* 1 in test kernels (cmake -DAGA_PROBES=ON, or AGA_DIAG), 0 in published ones:
   the per-frame measurements that cost time while a game runs. */
#ifdef AGA_PROBES
#define AGA_PROBES_ON 1
#else
#define AGA_PROBES_ON 0
#endif
void aga_diag_select(uint16_t v);
uint16_t aga_diag_read_word(void);
const char *aga_diag_name(int idx);
void aga_diag_trap(uint32_t reg, uint16_t val, int write, uint32_t pc);  /* AGA_DIAG only */
void aga_diag_snapshot(int had_data);   /* per frame: record the display state, watch for a blank one */
int  aga_diag_is_frozen(void);  /* the game's display went away: state kept, recording stopped */
void aga_diag_reg(uint32_t reg, int write);   /* per-register access histogram */
void aga_diag_beam_value(uint16_t v);         /* what a beam read handed back */
void aga_diag_reg_reset(void);                /* zero both, when the blank display starts */
void aga_diag_cia(uint32_t far, int write);   /* CIA traffic, which bypasses every other counter */
void aga_diag_cia_value(uint32_t far, int write, uint8_t v);   /* the bytes on CIA-B port A */
void aga_diag_serial(uint8_t c);              /* a byte the 68k wrote to SERDAT */
uint32_t aga_diag_serial_count(void);
uint8_t  aga_diag_serial_get(uint32_t i);
void aga_diag_capture_code(uint32_t pc);      /* 68k code around the PC I froze at */
void aga_diag_capture_code2(uint32_t pc);     /* and where the last interrupt landed */
void aga_diag_frame_trace(uint32_t sr, uint32_t traps, uint32_t chipw,
                          uint32_t l3, uint32_t l6, uint32_t ticks, uint32_t pc);
void aga_diag_wave(uint8_t l, uint8_t r);     /* točni bajtovi predani DACu */
void aga_diag_wave_freeze(int on);            /* sačuvaj igrinih zadnjih 15 s; reset pri entryju */
void aga_diag_triggers(const uint32_t *v, int n);   /* zadnji audio channel startovi */
void aga_diag_audw(uint32_t reg, int size, uint32_t value, uint32_t vpos, uint32_t hpos);   /* sound-register writeovi, po redu */
void aga_diag_env(const uint32_t *nrg_delta);       /* per-channel glasnoća, ovaj frame */
void aga_diag_wrapinfo(const uint32_t *v, int n);   /* u što se svaki kanal vraća */
void aga_diag_env_freeze(int on);
void aga_diag_pc_sample(uint32_t pc);         /* pravi 68k PC, samplano pri interrupt dispatchu */
uint32_t aga_diag_hot_pc(void);               /* bucket s najviše sampleova */
void aga_diag_freeze_here(void);              /* zaustavi snimanje, zadrži kod */
void aga_diag_persist_setup(void *area);      /* start.c: zadrži kopiju prošle sesije */
void aga_diag_persist(void);                  /* housekeeper: spremi je, Pi se reboota */
void aga_diag_frame_lag(uint32_t ms);         /* koliko je kasno ovaj frame izašao, za profil */
int  aga_diag_have_prev(void);                /* sesija je sačuvana: ostavi chip RAM na miru */
void aga_diag_set_chip(const uint8_t *va, uint32_t size);
void aga_glue_snapshot_regs(uint16_t *regs, uint32_t *colors);
#define AGA_PERSIST_SIZE (64u << 10)          /* at the top of the guard below chip RAM */
void aga_diag_intreqr_pc(uint32_t pc);        /* a sandboxed game read INTREQR here */
void aga_diag_intreqr_val(uint32_t v);        /* ...and was handed this */
void aga_audio_mask(unsigned m);              /* $5D0N: which channels reach the mix */
extern int aga_game_mode;                     /* armed by the WHDLoad hook at sandbox entry */
/* FRF PROGRAM/RUN presentation ownership. The virtual chipset remains alive
   while P96 owns HDMI; physical HAM6 takes over only after the child Copper. */
extern volatile int aga_program_mode;
extern volatile int aga_program_rtg_yield;
void aga_program_rtg_surface(int on);
void aga_program_seed_display(void);
extern int aga_exter_locked;                  /* WHDLoad has taken over: never re-arm EXTER */

#ifdef AGA_DIAG
#define AGA_DIAG_INC(i) aga_diag_inc(i)
#define AGA_DIAG_TRAP(r, v, w, p) aga_diag_trap(r, v, w, p)
#else
#define AGA_DIAG_INC(i) ((void)0)
#define AGA_DIAG_TRAP(r, v, w, p) ((void)0)
#endif

#endif

/* AGA-Storm 1.121 HDMI driver handoff */
void aga_hdmi_sandbox(int up);

/* AGA-Storm 1.121 Workbench HDMI routing */
void aga_hdmi_wb_other(int on);
