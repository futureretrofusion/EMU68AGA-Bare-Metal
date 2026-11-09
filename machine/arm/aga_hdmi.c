/* AGA-STORM — emulirani Paulin mix i na HDMI-ju.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Pi-jev HDMI enkoder ima vlastiti audio ulaz, MAI: 32-riječni FIFO IEC958
 * subframeova koje šalje zaslonu kao audio sample pakete. Pod Emu68 firmware
 * posjeduje HDMI izlaz i ostavlja MAI besposlenim - hvala mu, imam što
 * raditi. Ovaj file ga pokreće kako to radi Linux -
 * drivers/gpu/drm/vc4/vc4_hdmi.c (GPL-2.0-only):
 * vc4_hdmi_audio_startup, _prepare, _set_mai_clock, _set_n_cts i
 * vc4_hdmi_write_infoframe, s vrijednostima koje Linux koristi za Pi 4 - i
 * hrani ga legacy DMA engineom, tempiranim MAI-jevim DREQ-om, iz ringa koji
 * puni chipset jezgra.
 *
 * Što firmware ostavlja postavljeno prvo je ispitano (docs/HDMI-AUDIO.md,
 * tools/hdmi): HDMI link s upaljenim audio clockom, ali bez audio infoframea -
 * njegovi packet slotovi su 0 General Control, 2 AVI i 4 SPD, ne Linuxovo
 * numeriranje po tipu - i bez N/CTS. Zato audio infoframe ide u slot 7, koji
 * ostavlja isključenim, a N se postavlja; CTS hardver mjeri sam
 * (EXTERNAL_CTS_EN): CTS pogrešan za pola svirao je točno isto.
 *
 * Ring je cijeli broj 192-frame IEC958 blokova, pa mjesto svakog framea u
 * njegovom bloku slijedi iz njegovog indeksa i DMA ga loopa bez šava: tišina
 * upisana jednom svira zauvijek. Jezgra 3 resampla mix s Paulinih 27,928 Hz
 * na 48 kHz fiksni lead ispred DMA, trima rate po leadu koji mjeri (DMA-jev
 * SOURCE_AD točna je play pozicija), i drži tišinu upisanu još malo dalje.
 * Kad mix stane - sandbox napušten, igra pauzirana u debuggeru - housekeeper
 * puni ring tišinom prije nego što bi DMA mogao naići na stari zvuk.
 *
 * Isključeno dok se ne zatraži: Chipset menu - Sound - HDMI On šalje $5821
 * (agaboot SOUND, koji sprema izbor i ponovno ga šalje pri svakom bootu i
 * igri), $5820 ga zaustavlja i vraća svaki registar koji je promijenio kako
 * ga je firmware imao. Dok je željen i ne može svirati (nema linka, slot ili
 * MAI su zauzeti, nema slobodnog DMA kanala) housekeeper pokušava ponovno
 * jednom u sekundi.
 *
 * Pi 4-jev HDMI0 i Pi 3-jev HDMI, svaki iz svoje tablice (struct hdmi_hw),
 * odabrane prema ploči na kojoj se izvodi: Pi 4-jev MAI radi na fiksna
 * 108 MHz, Pi 3-jev na svom HSM-u, čiji rate dolazi iz CPRMAN-ova djelitelja.
 *
 * Čitanje HDMI registra dok je clock HDMI state machinea (HSM) zaustavljen
 * blokira jezgru koja ga čita - 68k-ovu, ili housekeeperovu, koja uzorkuje
 * Amigine interrupt linije. Zato boot ne čita nijedan HDMI registar, samo
 * clock (firmwareov izmjereni rate na Pi 4, CPRMAN na Pi 3); firmwareove
 * vrijednosti spremaju se pri prvom startu. Nakon boota svaki krug koji dira
 * HDMI registar prvo treba da se HDMI slika u tom trenutku kreće - HVS kanal
 * 1, koji je hrani, viđen kako mijenja status između dva čitanja neposredno
 * prije (HVS radi na core clocku, pa to čitanje nikad ne blokira) - i na Pi 3
 * HSM koji radi. Guardovi iste vrste kao u RGB2RTG-ovom portu ovog filea
 * (HDMI slika koja se kreće, i na Pi 3 HSM), čiji su revieweri našli što
 * zatvaraju; detalj se razlikuje (njegov Pi 4 guard je blank koji je vidio
 * njegov vlastiti HVS poll, a njegov aga.hdmiforce preskače Pi 3 provjeru,
 * gdje moj samo odustaje od izvora). Dvije praznine ostaju, naravno. Na Pi 4
 * pokretna slika zamjenjuje HSM koji radi, što ovdje ništa ne može pročitati
 * nakon boota: pretpostavka, još neprovjerena na hardveru (isključi dok zvuk
 * svira, sa i bez hdmi_force_hotplug). I clock još može stati između
 * guardovih čitanja i pristupa registru, prozor od najviše oko 100
 * mikrosekundi (200 ms u prvom portu popravaka). Link se smatra izgubljenim
 * tek nakon 200 ms bez
 * promjene; do tada pogled samo čeka sliku. Pi koji je bootao bez HDMI clocka
 * (nema zaslona, nema hdmi_force_hotplug) ostavljen je na miru - Pi 3 gleda
 * ponovno dok je zvuk željen, jer se CPRMAN može čitati bilo kada. cmdline.txt
 * aga.nohdmiaudio isključuje ga zauvijek.
 *
 * 1.114: 24 bita i rate na kojem AHI program svira. Svaki subframe nosi 24
 * bita uzorka: Paulin mix puni gornjih 16 kao prije, AHI program umiksan u 32
 * bita (tools/ahi/aga-hdmi.c, HiFi) cijela 24. I HDMI radi na 48 kHz dok AHI
 * program ne svira na 44.1, 88.2, 96, 176.4 ili 192 kHz: tada na tom rateu
 * (set_rate) - glazba s CD-a ili MP3 reproducirana kako je napravljena, ne
 * resamplana na 48 kHz - s Paulom resamplanom na njega. Samo ono što zaslon
 * prihvaća: njegov EDID, pročitan pri bootu (aga_edid.h), kaže koji rateovi i
 * prima li 24 bita; zaslon koji prima 16 bita dobiva gornjih 16, zaokruženih.
 * Novi rate je kratka praznina dok se zaslon ne zaključa; ostaje dok drugi
 * program ne zatraži drugi, ili igra ne pokrene sandbox (opet 48 kHz).
 */
#include <stdint.h>
#include "aga_glue.h"
#include "aga_hdmi_filter.h"
#include "aga_mix.h"
#include "aga_edid.h"
#include "aga_fx.h"
#include "aga_blep.h"
#include "aga/chipset.h"                                 /* aga_step_t */
#include "devicetree.h"

extern void kprintf(const char *fmt, ...);
extern uint32_t get_clock_rate(uint32_t clock_id);     /* Emu68 support_rpi.c: mailbox */
extern uint32_t get_clock_rate_measured(uint32_t clock_id);
extern int get_edid_block(uint32_t block, uint8_t *out);

int aga_hdmi_audio_off;                                  /* cmdline aga.nohdmiaudio */
int aga_hdmi_force;                                      /* cmdline aga.hdmiforce: bez provjere clocka */

#define LE32(x)  __builtin_bswap32(x)                    /* ARM radi big-endian */
#define PERI(b)  ((volatile uint32_t *)(0xF2000000UL + ((uint32_t)(b) - 0x7E000000UL)))
static inline uint32_t rd(uint32_t b)             { return LE32(*PERI(b)); }
static inline void     wr(uint32_t b, uint32_t v) { *PERI(b) = LE32(v); }

/* HDMI blok svakog SoC-a: Linux vc4_hdmi_regs.h (vc5_hdmi_hdmi0_fields za
   Pi 4-jev HDMI0, vc4_hdmi_fields za Pi 3), adrese i DREQ-ovi iz
   bcm2711.dtsi i bcm2835-common.dtsi, te threshold i channel-map vrijednosti
   koje vc4_hdmi.c daje svakoj generaciji. I Pi 3-jev firmware je ispitan
   (2026-10-03, 3 A+): isti packet slotovi kao na Pi 4, isti besposleni MAI,
   bez N/CTS. */
struct hdmi_hw {
    const char *name;
    uint32_t mai_ctl, mai_thr, mai_fmt, mai_data, mai_smp;   /* MAI ("HD") blok */
    uint32_t ch_map, mai_cfg, aud_pkt, pkt_cfg, pkt_stat, crp, cts0, cts1, sched, pkt_ram;
    uint32_t phy, phy_rng;                       /* registar s RNG power-down bitom */
    uint32_t dreq, thr, chmap, dma_mask;         /* dma_mask: DT-ov default */
};
static const struct hdmi_hw hw_pi4 = { "HDMI0 (Pi 4)",
    0x7EF20010, 0x7EF20014, 0x7EF20018, 0x7EF2001C, 0x7EF20020,
    0x7EF0079C, 0x7EF007A0, 0x7EF007B8, 0x7EF007BC, 0x7EF007C4, 0x7EF007C8, 0x7EF007CC, 0x7EF007D0, 0x7EF007E0,
    0x7EF01B00,
    0x7EF00F04, 1u << 4,                         /* TX_PHY_POWERDOWN_CTL, RNDGEN_PWRDN */
    10, 0x10101C1C, 0x10, 0x07F5 };              /* 4 bita po kanalu */
static const struct hdmi_hw hw_pi3 = { "HDMI (Pi 3)",
    0x7E808014, 0x7E808018, 0x7E80801C, 0x7E808020, 0x7E80802C,
    0x7E902090, 0x7E902094, 0x7E90209C, 0x7E9020A0, 0x7E9020A4, 0x7E9020A8, 0x7E9020AC, 0x7E9020B0, 0x7E9020C0,
    0x7E902400,
    0x7E9022C4, 1u << 25,                        /* TX_PHY_CTL_0, RNG_PWRDN */
    17, 0x08080608, 0x08, 0x7F35 };              /* 3 bita po kanalu; thresholdi sniženi Linuxom 1c594eeccf92 */
static const struct hdmi_hw *hw;
/* Tablica ide po ploči, ne po kernelu: Pi 3 kernel na Pi 4 radi kao Pi 4
   kernel (aga_glue.h), a Pi 4-jev CPRMAN nema HSM - tamo čita 0x636D, što bi
   izgledalo kao zaustavljen clock (otkrio RGB2RTG). */
static int pi3;
static uint32_t mai_smp;                         /* MAI clock / rate, kao N << 8 | M - 1 */
static uint32_t mai_hz;                          /* ... taj clock */
static uint32_t hsm_div;                         /* Pi 3: HSM djelitelj iz kojeg je mai_smp izračunat */
static int smp_new;                              /* ... promijenjen pod mojim MAI-jem koji radi: MAI_SMP za upis */

#define HSM_CLOCK 13                             /* Pi 4: firmware clock "hdmi" (M2MC) */
#define CM_HSMCTL 0x7E101088u                    /* Pi 3: CPRMAN-ov HSM, MAI clock */
#define CM_HSMDIV 0x7E10108Cu
#define SLOT      7                              /* firmware koristi 0, 2 i 4 */

/* Rateovi na kojima HDMI smije raditi, redoslijed aga_edid.h bitova i
   aga_hdmi_filter.h hf_k: N za sink (HDMI-jeva tablica za 148.5 MHz i
   većinu drugih pixel clockova; CTS mjeri hardver), MAI-jev kod za rate
   (vc4_hdmi-jev VC4_HDMI_MAI_SAMPLE_RATE_*), i IEC 60958-ov u channel status
   bajtu 3. */
#define N_RATES 6
#define R_48K   1
static const uint32_t rate_hz[N_RATES]  = { 44100, 48000, 88200, 96000, 176400, 192000 };
static const uint32_t rate_n[N_RATES]   = { 6272, 6144, 12544, 12288, 25088, 24576 };
static const uint8_t  rate_mai[N_RATES] = { 8, 9, 11, 12, 14, 15 };
static const uint8_t  rate_cs[N_RATES]  = { 0x0, 0x2, 0x8, 0xA, 0xC, 0xE };
_Static_assert(N_RATES == HF_RATES, "a filter set for every rate");
static volatile int rate_i = R_48K;              /* za što su MAI, ring i tablice postavljeni */
static struct edid_eld eld;                      /* zaslonov ELD iz njegovog EDID-a (aga_edid.h), za Sound monitor */
static uint32_t sink_rates = 3;                  /* bit i: zaslon prihvaća rate_hz[i] (EDID); 44.1 i 48 kHz bez EDID-a */
static int sink_24;                              /* ... i 24-bitne uzorke */

/* HDMI sound rate u Sound prozoru (agaboot SOUND RATE), $5868 + E2(m) s
   E2(m) = (m & 1) | (m & 2) << 1: 0 prati program koji svira unutar onoga što
   zaslonov EDID navodi, 1 prati ga na bilo kojem rateu (EDID koji podcjenjuje
   svoj receiver), 2 uvijek 96 kHz, 3 uvijek 192 kHz - Paula i AHI jednako.
   Što se nudi (driveru, kroz AGA_D_AHI) slijedi iz toga, a iza EDID-a se
   smatra da receiver želi i 24 bita. */
static volatile int rate_mode;
static const int fixed_ri[4] = { -1, -1, 3, 5 };
static uint32_t offer_rates(void)
{
    int m = rate_mode;
    return m == 0 ? sink_rates : m == 1 ? 0x3Fu : 1u << fixed_ri[m];
}
static int out_24(void) { return sink_24 || rate_mode != 0; }
#define SLOT_AT   (hw->pkt_ram + SLOT * 0x24)

/* HVS kanal 1, onaj kojim firmware prikazuje HDMI (i u koji aga_hvs.c crta):
   SCALER_DISPSTAT1 (vc4_regs.h). Samo polja kanala 1 - mode, FIFO full/empty,
   frame count, line; riječ nosi i frame count kanala 0 (bitovi 23:18 na Pi 3,
   25:20 na Pi 4), koji bi zaslon na DSI portu držao pokretnim. */
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
#define M_FLAGS   (M_DLATE | M_ERRORE | M_ERRORF)        /* upiši 1 za čišćenje */
/* MAI radi: dva kanala, cijeli uzorci, poravnati. Upis koji čisti flagove dok
   svira nosi i ove - bez ENABLE staje. */
#define M_RUN     ((2u << 4) | M_WHOLSMP | M_CHALIGN | M_ENABLE)

/* legacy DMA engine (bcm2835-dma.c) */
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
/* AXI prioritet kanala, i onaj koji dobiva dok MAI signalizira panic (njegov
   FIFO ispod MAI_THR panic razine): s 0, defaultom, ti thresholdi ne dižu
   ništa. Još nije uspoređeno na hardveru: MAI-jeve error flagove
   (hdmi_mai_errs) treba gledati. Vjerujem Linuxu, zasad. */
#define CS_PRIO     ((8u << 16) | (15u << 20))
/* DEBUG-ovi error flagovi - read last not set, FIFO error, read error - upiši
   1 za čišćenje. CS-ov ERROR samo pokazuje da je jedan od njih postavljen;
   reset kanala nije dokumentiran da ih čisti, pa se čiste ovdje. */
#define DBG_ERRORS  7u
#define TI_WAIT_RESP (1u << 3)
#define TI_DEST_DREQ (1u << 6)
#define TI_SRC_INC   (1u << 8)
#define TI_PERMAP(n) ((uint32_t)(n) << 16)

/* Ring i lead, po ploči. Cijeli 192-frame blokovi, dva subframea po frameu.
   Pi 4 piše 21 ms ispred u 80 ms ringu i nije se resyncao ni jednom u 5
   minuta na CM4 (2026-10-02). Pi 3-jev chipset loop staje do 31 ms - memory
   starvation A53 klastera (2026-09-14) - i na 21 ms resyncao se 14 puta u 8
   minuta, Paula put sa svojih 37 ms leada 6 puta u istom runu (2026-10-03):
   piše 48 ms ispred u 120 ms ringu. Lead i silence horizon zajedno ostaju
   dulji od idle waita, pa DMA susreće tišinu, nikad stari zvuk, kad mix
   stane.

   I idle wait završava unutar pauza koje lead check hvata sam. Feed nakon
   pauze dulje od (lead_target - lead_low) / 48 ms resynca se na svom leadu,
   dok pauza nije toliko duga da writer, sada IZA DMA, pokaže lead ispod
   lead_high opet: na (ring_frames - lead_high + lead_target) / 48 ms. S 3000
   to je bilo 38.8 ms na Pi 4, ispod njegovih 40 ms waita: feed 39 ms kasno
   našao je writera 870 frameova iza DMA s leadom koji je izgledao dobro, i
   DMA je odsvirao ostatak tišine pa 48 ms zvuka starog 80 ms (otkrio review).
   2700 čini to 45 ms; Pi 3-jev 4800 čini 68 ms prema svojih 60. Ni na jednoj
   ploči lead u igri, za sada, ne dolazi blizu granice.

   To su brojevi na 48 kHz (base_*). Na drugom rateu svaki broj frameova
   skalira se na isto vrijeme (rate_params), ring zaokružen na cijele blokove
   - nekoliko ms dulje, što samo proširuje margine iznad. Na 192 kHz Pi 3-jevih
   120 ms je 23,040 frameova: RING_MAX, 184 KB. */
#define RING_MAX    (120 * 192)                          /* najviše što ijedna ploča koristi, na 192 kHz */
static uint32_t base_ring = 20 * 192, base_lead = 1024, base_high = 2700;   /* na 48 kHz */
static uint32_t ring_frames = 20 * 192;                  /* 3840: 80 ms */
static uint32_t lead_target = 1024;                      /* 21 ms ispred DMA */
static uint32_t lead_high   = 2700;                      /* izvan lead_low..lead_high: resync */
static uint32_t lead_low    = 192;
static uint32_t horizon     = 1536;                      /* tišina još 32 ms dalje */
static uint32_t idle_ms     = 40;                        /* nema mixa toliko: ring prelazi u tišinu */

static uint32_t at_rate(uint32_t frames_48k)
{
    return (uint32_t)(((uint64_t)frames_48k * rate_hz[rate_i] + 24000) / 48000);
}

/* gornji brojevi za rate_i - bez ičega što svira */
static void rate_params(void)
{
    ring_frames = (at_rate(base_ring) + 191) / 192 * 192;
    if (ring_frames > RING_MAX) ring_frames = RING_MAX;
    lead_target = at_rate(base_lead);
    lead_high   = at_rate(base_high);
    lead_low    = at_rate(192);
    horizon     = at_rate(1536);
}

static volatile uint32_t *ring;                          /* bez cachea, identity-mapped */
static uint32_t ring_bus, cb_bus;                        /* što DMA vidi */
static uint32_t dma_mask;                                /* kanali koje firmware ostavlja ARM-u */
static volatile int dma_ch = -1;
static uint32_t dma_enable_was;                          /* njegov DMA_ENABLE bit prije mog starta: vraća se pri stopu */
static volatile int state = -1;                          /* AGA_D_HDMI_STATE; -1 prije boota */
/* 6 je neiskorišten: prije 1.112 značio je "još ne na Pi 3", a agaboot čita
   stanja po broju, pa ostaje slobodan */
enum { H_OFF, H_RUNNING, H_NO_LINK, H_NO_CLOCK, H_SLOT_BUSY, H_NO_DMA, H_UNUSED_6, H_SWITCHED_OFF,
       H_BUSY_OTHER,                                     /* BUSY_OTHER: nečiji tuđi MAI je uključen */
       H_WB_OTHER };                                     /* željen, ali Workbenchov HDMI zvuk je drugog drivera */
static volatile int wanted;                               /* zadnji $582N: 1 zvuk željen */
/* $5825 / $5824 (1.121): Workbenchov HDMI zvuk je drugog drivera - Emu68-ov
   videocore.audio, kroz firmwareov audio service - i moj svira samo dok je
   sandbox podignut. Svaki start videocore.audio streama pušta firmware da
   postavi MAI na svoj način (byte order, djelitelj, thresholdi, vlastiti
   infoframe u slotu 3) s istim N kao mojim, pa ga pogled jednom u sekundi ne
   vidi: oba DMA tada hrane jedan FIFO, i stream siječe (čulo se na A500,
   2026-11-05). S ovim uključenim, moj staje na Workbenchu, počinje pri ulasku
   u igru preko svega što je firmware ostavio (spremajući to, kao svaki start),
   a pri izlasku vraća firmwareove vrijednosti - stream držan otvorenim kroz
   igru nastavlja svirati. */
static volatile int wb_other, in_game;
static inline int want_now(void) { return wanted && (!wb_other || in_game); }
static int ready;                                        /* boot je našao clock (ili Pi 3 koji treba čekati) i DMA kanale */
/* MAI registri i slot 7 drže moje vrijednosti: vraćaju se pri stopu - ili,
   kad je clock stao pod njima, kad me guardovi puste. I sam slot 7: nakon što
   je drugi driver preuzeo MAI dok je moj svirao (check), moj infoframe ostaje
   uključen do Sound Off ili mog sljedećeg starta. */
static volatile int mai_ours, slot_ours;
static inline int ours(void) { return mai_ours || slot_ours; }
static int info_pending;                                 /* infoframe čeka da njegov slot utihne */
/* firmwareove vrijednosti svega što mijenjam, pročitane pri prvom startu,
   vraćene pri stopu (PHY-jev RNG bit i MAI_CTL svaki na svoj način) */
enum { S_THR, S_FMT, S_SMP, S_CHMAP, S_CFG, S_PKT, S_CRP, S_CTS0, S_CTS1, S_PHY, S_CTL, S_N };
static uint32_t saved[S_N], saved_reg[S_N];
/* start, stop (jezgra 0, iz 68k-ove control riječi) i housekeeperove popravke
   svaki pišu cijeli niz registara: jedan po jedan */
static volatile int idle = 1;            /* ring drži tišinu: jezgra 3 resynca se na svom sljedećem feedu */
static volatile int reg_lock;
static inline void lock_regs(void)   { while (__atomic_exchange_n(&reg_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("yield"); }
static inline int  try_regs(void)    { return !__atomic_exchange_n(&reg_lock, 1, __ATOMIC_ACQUIRE); }
static inline void unlock_regs(void) { __atomic_store_n(&reg_lock, 0, __ATOMIC_RELEASE); }

static inline uint64_t cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t cntfrq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* ---- IEC958 subframeovi -------------------------------------------------------
   Jedna little-endian riječ po kanalu, lijevi prvi: bitovi 3:0 preambula (8 za
   lijevi subframe koji počinje 192-frame blok, 2 za svaki drugi lijevi, 4 za
   desni), 24-bitni uzorak u 27:4, bit 30 ovaj frameov bit channel status
   bloka, bit 31 parna parnost nad bitovima 4-30. Channel status kako ga šalje
   Raspberry Pi OS: consumer PCM, rate (bajt 3), i duljina riječi (bajt 4): 24
   bita za zaslon koji ih prima, inače 16 - čiji subframeovi tada nose 16
   bita, zaokruženih, a 8 ispod njih nula. */
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

/* s: 24 bita, signed */
static inline uint32_t subframe(uint32_t f, int ch, int32_t s)
{
    uint32_t smp = (uint32_t)s & 0xFFFFFFu;
    uint32_t w = base_w[f][ch] | (smp << 4);
    if (base_p[f][ch] ^ __builtin_parity(smp)) w |= 1u << 31;
    return LE32(w);
}

/* k uvijek ispod RING_MAX - ringove memorije - čak i ako broj iz vremena
   prije novog ratea susretne novi ring_frames */
static void put_frame(uint32_t k, int32_t l, int32_t r)
{
    if (k >= RING_MAX) return;
    if (!out_24()) {                                     /* 16 bita, zaokruženo */
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

/* Cijeli ring je tišina i ništa ga ne piše: start smije početi odmah.
   Jezgra 0 ga utišava u jednom potezu; housekeeper u komadima (prep_*), kao
   što radi idle fill. */
static int ring_silent;
static uint32_t prep_at, prep_left;

static void set_state(int s)
{
    __atomic_store_n(&state, s, __ATOMIC_RELEASE);
    aga_diag_set(AGA_D_HDMI_STATE, (uint32_t)s);
}

/* ---- guardovi -------------------------------------------------------------- */

/* MAI uzima uzorak svakih N/(M+1) clockova: najbolji N/M za rate s M+1
   najviše 256 (vc4_hdmi_audio_set_mai_clockova racionalna aproksimacija).
   108 MHz daje 2250/1 za 48 kHz točno; Pi 3-jevih 163,682,864 Hz
   228474/67. */
static uint32_t smp_for(uint32_t clock_hz, uint32_t rate)
{
    uint64_t best_n = 0, best_m = 1, best_err = ~0ULL;
    for (uint64_t m = 1; m <= 256; m++) {
        uint64_t n = ((uint64_t)clock_hz * m + rate / 2) / rate;
        if (!n || n >= (1u << 24)) continue;
        uint64_t got = n * rate, want = (uint64_t)clock_hz * m;
        uint64_t err = (got > want ? got - want : want - got) * 256 / m;    /* usporedivo po m */
        if (err < best_err) { best_err = err; best_n = n; best_m = m; }
    }
    return (uint32_t)(best_n << 8) | (uint32_t)(best_m - 1);
}

/* Pi 3: CPRMAN-ov HSM, MAI clock. 1 kad radi (ENAB i BUSY): HDMI registri se
   smiju čitati. 0 kad ne radi: tada nijedan ne smije. CPRMAN nikad ne blokira
   čitanje, pa je ovo guard u svakom trenutku. Je li mu rate poznat je
   odvojeno pitanje (hsm_known): iz PLLD - source 6, koji firmware vrti na
   500 MHz - rate slijedi iz djelitelja (12.12): CM_HSMCTL $2D6, CM_HSMDIV
   $30E0, 163,682,864 Hz na 3 A+. Iz drugog izvora rate je nepoznat: moj MAI
   se tada ne pokreće ni popravlja, osim ako aga.hdmiforce kaže nastaviti na
   zadnjem poznatom rateu, ali se može vratiti. Firmware dimenzionira HSM po
   pixel clocku, pa novi mode može promijeniti rate: SMP se ponovno izračuna,
   a MAI koji već radi dobiva ga pri sljedećem pogledu (smp_new). S držanim
   reg_lockom, ili pri bootu. */
static int hsm_known = 1;
static int hsm_pi3(void)
{
    uint32_t ctl = rd(CM_HSMCTL), div = rd(CM_HSMDIV) & 0xFFFFFF;
    aga_diag_set(AGA_D_HDMI_HSM_SET_KHZ, ctl);           /* sirova control riječ, za agastat */
    if ((ctl & 0x90) != 0x90)
        return 0;                                        /* nije ENAB+BUSY: zaustavljen */
    int plld = (ctl & 0x9F) == 0x96 && div >= 0x1000;    /* inače drugi izvor, ili besmislica */
    hsm_known = plld || aga_hdmi_force;
    if (plld && div != hsm_div) {
        hsm_div = div;
        mai_hz = (uint32_t)((500000000ULL * 4096) / div);
        mai_smp = smp_for(mai_hz, rate_hz[rate_i]);
        smp_new = mai_ours;                              /* MAI radi na starom */
        aga_diag_set(AGA_D_HDMI_HSM_KHZ, mai_hz / 1000);
    }
    return 1;
}

/* Kreće li se HDMI slika? Housekeeper uzorkuje status HVS kanala 1 najviše
   jednom u milisekundi dok bi mogao trebati znati; svaka promjena - linija,
   mode, frame count - bilježi vrijeme. Dva uzorka razmaknuta više od 2 ms ne
   dokazuju ništa (stari uzorak prema svježem), pa prvi nakon praznine samo
   postavlja bazu. agastat pokazuje maskiranu riječ i viđene promjene
   (hdmi_scan_stat, hdmi_scan_moves): state 2 sa slikom na zaslonu i brojem
   koji stoji znači da se to polje ne kreće na toj ploči, a ne da je link
   pao. */
static uint32_t scan_stat;
static volatile uint64_t scan_at;                        /* HDMI slika zadnji put viđena pokretnom, 0 nikad */
static uint64_t scan_prev;                               /* vrijeme zadnjeg uzorka */

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

/* Promjena između ovog čitanja i jednog najviše 100 us prije: slika se kreće
   sada, ne samo prije milisekunde. Čita se jednom po posjetu dok pogled čeka
   - linija traje 15 do 64 us, vertical blank najviše oko 1.6 ms - pa pogled
   slijedi unutar linije ili blanka. */
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

/* Housekeeperov guard. 1: slika se kreće ovog trenutka i, na Pi 3, HSM radi
   - HDMI registri se smiju dirati sada. 0: link je izgubljen - nema promjene
   200 ms, ili je Pi 3-jev HSM stao. -1: nije viđena pokretnom u zadnjih 5 ms,
   ili još nije u ovom pogledovom čitanju - čekaj sljedeći posjet, bez
   odustajanja od linka. Vrijeme između viđene promjene i pristupa je ono što
   je ostalo od starog 200 ms prozora: clock koji stane u njemu još blokira
   jezgru. */
static int guard_ok(uint64_t now, uint64_t hz)
{
    uint64_t at = __atomic_load_n(&scan_at, __ATOMIC_ACQUIRE);
    if (!at || now - at > hz / 5) return 0;
    if (now - at > hz * 5 / 1000) return -1;
    if (pi3 && !hsm_pi3()) return 0;
    return moving_now(now, hz) ? 1 : -1;
}

/* jezgre 0: sama gleda HVS do 3 ms i nastavlja pri prvoj promjeni između dva
   čitanja (bez cacheiranog pečata: slika viđena pokretnom prije nekog vremena
   ne govori ništa o clocku sada) */
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

/* ---- HDMI strana: sve iza guardova, s držanim reg_lockom ------------------ */

/* Audio infoframe u slot 7: tip 0x84 v1 duljina 10, checksum 0x70, dva
   kanala. Njegov packet RAM piše se tek kad je slot isključen i enkoder ga
   prestao slati (STATUS clear): do tada je pending i kasniji posjet gleda
   ponovno - Linux čeka do 100 ms na to, a ni 68k-ova jezgra ni housekeeper ne
   smiju stajati toliko. Dolazi li STATUS nakon toga stvar je pogleda jednom u
   sekundi (check): slot ostavljen taman piše se ponovno i broji kao popravak. */
static void infoframe(void)
{
    slot_ours = 1;                                       /* moj od sada, pending ili poslan */
    uint32_t cfg = rd(hw->pkt_cfg);
    if (cfg & (1u << SLOT))
        wr(hw->pkt_cfg, cfg & ~(1u << SLOT));
    if (rd(hw->pkt_stat) & (1u << SLOT)) {
        info_pending = 1;
        return;
    }
    wr(SLOT_AT + 0, 0x000A0184);
    wr(SLOT_AT + 4, 0x00000170);
    for (int w = 2; w < 9; w++) wr(SLOT_AT + 4 * w, 0);  /* ostatak nula, ili checksum ne valja */
    wr(hw->pkt_cfg, rd(hw->pkt_cfg) | (1u << SLOT));
    info_pending = 0;
}

/* MAI kako ga ostavljaju vc4_hdmi_audio_startup i _prepare: stereo PCM na
   rate_i rateu s MAI clocka (mai_smp), i N za sink */
static void program_mai(void)
{
    wr(hw->mai_ctl, M_RESET | M_FLUSH | M_FLAGS);
    wr(hw->phy, rd(hw->phy) & ~hw->phy_rng);             /* RNG napajan */
    wr(hw->mai_smp, mai_smp);
    wr(hw->mai_ctl, M_RUN);
    wr(hw->mai_fmt, (2u << 16) | ((uint32_t)rate_mai[rate_i] << 8));   /* PCM, rate */
    wr(hw->mai_thr, hw->thr);
    wr(hw->mai_cfg, (1u << 27) | (1u << 26) | 3);        /* format i bit reverse, kanali 0-1 */
    wr(hw->ch_map, hw->chmap);
    wr(hw->aud_pkt, (1u << 29) | (1u << 24) | (8u << 10) | 3);
    wr(hw->crp, (1u << 24) | rate_n[rate_i]);            /* EXTERNAL_CTS_EN: CTS se mjeri */
    wr(hw->cts0, 148500);                                /* 1080p50, za prikaz */
    wr(hw->cts1, 148500);
    mai_ours = 1;
    smp_new = 0;
}

/* vc4_hdmi_audio_shutdown i _reset, pa firmwareove vrijednosti natrag. Moj
   infoframe slot isključen je; enkoder ga prestaje slati sam, ništa to ne
   čeka. Od PHY registra samo je RNG bit moj, vraćen sam po sebi kao što to
   radi Linuxov phy_rng_disable: ostatak je firmwareov link (lane power, i na
   Pi 3 drive), koji se postavlja iznova na hotplug ili novi mode - riječ
   pročitana pri startu to bi poništila (otkrio RGB2RTG). MAI koji je drugi
   driver preuzeo je njegov: samo moj slot odlazi. */
static void restore(void)
{
    if (mai_ours) wr(hw->mai_ctl, M_FLAGS);              /* zaustavljen: ENABLE clear */
    wr(hw->pkt_cfg, rd(hw->pkt_cfg) & ~(1u << SLOT));
    info_pending = 0;
    slot_ours = 0;
    if (!mai_ours) return;
    wr(hw->mai_ctl, M_RESET);
    wr(hw->mai_ctl, M_ERRORF);
    wr(hw->mai_ctl, M_FLUSH);
    for (int i = 0; i < S_PHY; i++) wr(saved_reg[i], saved[i]);
    wr(hw->phy, (rd(hw->phy) & ~hw->phy_rng) | (saved[S_PHY] & hw->phy_rng));
    /* njegov channel count, ne enabled - ili, kad je Workbenchov HDMI zvuk drugog
       drivera (wb_other) i firmware je držao MAI u radu za njega, ponovno radi
       kako ga je firmware ostavio: videocore.audio stream držan otvorenim kroz
       igru nastavlja svirati */
    if (wb_other && (saved[S_CTL] & M_ENABLE))
        wr(hw->mai_ctl, saved[S_CTL] & ~(M_FLAGS | M_FLUSH | M_RESET));
    else
        wr(hw->mai_ctl, saved[S_CTL] & (15u << 4));
    mai_ours = 0;
}

/* ---- DMA strana: njegovi registri su uvijek clockani ---------------------- */

/* Kanal koji firmware ostavlja ARM-u (brcm,dma-channel-mask) i koji sada
   nitko ne vrti - nije ACTIVE, nema učitan control block - ili moj iz
   prije. Sam mask nije dovoljan: 68k driver (SD kartica, Wi-Fi) mogao je
   uzeti jedan od tih kanala (otkrio RGB2RTG). CONBLK_AD 0 je idle kanal, kako
   ga čita Linuxov bcm2835_dma_abort. */
static int dma_free(int c)
{
    uint32_t cb = rd(DMA_CB(c));
    return cb == cb_bus || (cb == 0 && !(rd(DMA_CS(c)) & CS_ACTIVE));
}

/* 0 kad kanal više nije slobodan: 68k driver na jezgri 0 može ga uzeti bilo
   kada, pa se gleda ponovno neposredno prije reseta. Zauzimanje između tog
   pogleda i reseta - nekoliko bus ciklusa - ne može se isključiti bez locka
   koji drže oba drivera: moj reset završio bi taj driverov transfer. Race,
   fuck - i to je cijela priča. */
static int start_dma(void)
{
    int c = dma_ch;
    volatile uint32_t *cb = ring + 2 * RING_MAX;         /* jedan blok: ring, pa on sam, na bilo kojem rateu */
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
    uint64_t end = cntvct() + cntfrq() / 10000;          /* najviše 100 us */
    while ((rd(DMA_CS(c)) & CS_RESET) && cntvct() < end) ;
    wr(DMA_DEBUG(c), DBG_ERRORS);                        /* stara greška izgledala bi kao moja */
    wr(DMA_CB(c), cb_bus);
    wr(DMA_CS(c), CS_ACTIVE | CS_PRIO);
    return 1;
}

/* onaj koji imam, dok je još slobodan; inače prvi slobodan. Kanal koji mi je
   drugi driver uzeo napustio je dma_mask (check): između tog driverovog
   transfera izgledao bi opet slobodan. */
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

/* moj DMA stao - samo dok kanal još vrti moj control block: kanal koji je
   netko drugi uzeo njegov je za vrtnju (otkrio RGB2RTG). Prvo pauziran, i
   resetiran kad je njegov zadnji upis u MAI_DATA odgovoren (TI-jev WAIT_RESP),
   kako Linux 6.6 bcm2835_dma_abort čeka WAITING_FOR_OUTSTANDING_WRITES - 50 us
   najviše, za periferiju koja nikad ne odgovori. */
static void dma_stop(void)
{
    int c = dma_ch;
    if (c < 0 || rd(DMA_CB(c)) != cb_bus) return;
    wr(DMA_CS(c), 0);                                    /* pauziraj, pa zaboravi blok */
    uint64_t end = cntvct() + cntfrq() / 20000;
    while ((rd(DMA_CS(c)) & CS_WAIT_WR) && cntvct() < end) ;
    wr(DMA_CS(c), CS_RESET);
    if (!dma_enable_was)
        wr(DMA_ENABLE, rd(DMA_ENABLE) & ~(1u << c));
}

/* Kanal više nije moj, nađeno pri startu ili pogledu: moj MAI i slot vraćeni,
   state 5, a retry sekundu kasnije traži drugi. Kanal koji vrti tuđi control
   block (68k driver ga je uzeo nakon mene) napušta dma_mask do kraja boota:
   između tog driverovog transfera izgleda slobodan, a retry bi ga uzimao
   natrag od tog drivera iznova i iznova. CONBLK_AD 0 tamo je samo idle kanal i
   ostaje u masku. S držanim reg_lockom, iza guardova. */
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

/* ---- start i stop ---------------------------------------------------------- */

static uint64_t last_check;                              /* housekeeperov zadnji pogled jednom u sekundi */

/* s držanim reg_lockom, iza guardova, ring sva tišina (ring_silent); core0:
   68k-ova naredba, koja može i printati */
static void start(int core0)
{
    if (pi3 && !hsm_known) { set_state(H_NO_CLOCK); return; }   /* HSM-ov rate nepoznat: pokušava se ponovno */
    uint32_t sched = rd(hw->sched), pkt = rd(hw->pkt_cfg), ctl = rd(hw->mai_ctl);
    if ((sched & 3) != 3 || !(pkt & 0x10000)) { set_state(H_NO_LINK); return; }   /* DVI, ili nema linka */
    if ((pkt & (1u << SLOT)) && !ours()) { set_state(H_SLOT_BUSY); return; }
    /* MAI koji je već uključen i nije moj: drugi driver svira HDMI audio (68k
       driver koji sam vozi MAI) - tuđe se ne dira, jebote */
    if ((ctl & M_ENABLE) && !mai_ours && !(wb_other && in_game)) { set_state(H_BUSY_OTHER); return; }
    if (!dma_pick()) { set_state(H_NO_DMA); return; }
    if (!mai_ours) {                                     /* firmwareov, nije moj ostavljen od izgubljenog clocka */
        const uint32_t regs[S_N] = { hw->mai_thr, hw->mai_fmt, hw->mai_smp, hw->ch_map, hw->mai_cfg, hw->aud_pkt,
                                     hw->crp, hw->cts0, hw->cts1, hw->phy, hw->mai_ctl };
        for (int i = 0; i < S_N; i++) saved[i] = rd(saved_reg[i] = regs[i]);
    }
    __atomic_store_n(&idle, 1, __ATOMIC_RELEASE);
    program_mai();
    infoframe();
    dma_enable_was = rd(DMA_ENABLE) & (1u << dma_ch);
    if (!start_dma()) { dma_lost(dma_ch); return; }      /* zauzet od odabira */
    /* MAI je uključen prije nego DMA radi, kao u 1.112: ono što je flagao dok je
       čekao prvu riječ odlazi, da hdmi_mai_errs broji što se događa dok svira */
    wr(hw->mai_ctl, M_RUN | M_FLAGS);
    ring_silent = 0;
    last_check = cntvct();                               /* prvi pogled cijelu sekundu kasnije */
    set_state(H_RUNNING);
    if (core0)
        kprintf("[AGA] HDMI audio: on - %u Hz stereo, %d bit, on %s, DMA channel %d, ring at %08x\n",
                rate_hz[rate_i], out_24() ? 24 : 16, hw->name, dma_ch, ring_bus);
    else
        aga_diag_log("[AGA] HDMI audio: on (tried again once a second)");   /* konzola bi zadržala ovu jezgru */
}

/* $5821 uključeno, $5820 isključeno - iz agaboot SOUND, na jezgri 0. Djeluje
   odmah kad ga guardovi puste, a ostalo ostavlja housekeeperovom pogledu sekundu
   kasnije. */
/* moje zaustavljeno, firmwareove vrijednosti natrag - u state `to` (H_OFF,
   ili H_WB_OTHER dok je željen samo za igre). S držanim reg_lockom, na
   jezgri 0. */
static void stop_ours(int to)
{
    int s = state;
    if (s == H_RUNNING) {
        set_state(to);
        /* jezgra 3 i housekeeper gledaju state prije nego što išta dotaknu; daj
           onome koji je već prošao taj pogled vremena da završi */
        uint64_t t = cntvct() + cntfrq() / 10000;
        while (cntvct() < t) ;
        /* Prvo producer, zatim MAI (1.112 imao je obrnuto): DMA-jevi registri su
           uvijek clockani, pa staje što god guard kaže, dok se MAI vraća samo
           kad me guard pusti. Underflow koji MAI flagne između odlazi s
           restoreovim resetom. */
        dma_stop();
    } else if (s != H_NO_CLOCK) {
        set_state(to);                                   /* više ne čeka link */
    }
    if (ours() && guard_now()) restore();                /* inače housekeeperov, kad smije */
    if (s == H_RUNNING) kprintf("[AGA] HDMI audio: off%s\n", to == H_WB_OTHER ? " (Workbench's is another driver's)" : "");
}

/* sada, na jezgri 0: pokrenuto ako je sada željeno i ne svira */
static void start_now(void)
{
    int s = state;
    if (s == H_RUNNING) return;
    if (!guard_now()) {
        set_state(s == H_NO_CLOCK ? H_NO_CLOCK : H_NO_LINK);
    } else {
        if (!ring_silent) { silence(0, ring_frames); ring_silent = 1; }
        prep_left = 0;
        start(1);
    }
}

void aga_hdmi_enable(int on)
{
    /* nije ready: isključeno, Pi 4 bez clocka pri bootu, nema kanala u DT-ovu
       masku. Kanali kasnije ispušteni iz maska ne računaju se ovdje: Off mora
       ipak završiti želju i retryje (state 5 s praznim maskom se ne ponavlja) */
    if (!ready) return;
    wanted = on;
    lock_regs();
    if (want_now()) start_now();
    else stop_ours(on ? H_WB_OTHER : H_OFF);
    unlock_regs();
}

/* $5825 uključeno / $5824 isključeno, jezgra 0: Workbenchov HDMI zvuk drugog
   drivera (agaboot SOUND WORKBENCH VIDEOCORE|OURS) - vidi wb_other */
void aga_hdmi_wb_other(int on)
{
    if (!ready) return;
    wb_other = on;
    lock_regs();
    if (want_now()) start_now();
    else if (wanted) stop_ours(H_WB_OTHER);
    unlock_regs();
}

/* sandbox gore (1) ili dolje (0), jezgra 0, iz svog ulaska i izlaska: u
   wb_other modu moj počinje pri ulasku i staje pri izlasku */
void aga_hdmi_sandbox(int up)
{
    in_game = up;
    if (!ready || !wb_other || !wanted) return;
    lock_regs();
    if (up) start_now();
    else stop_ours(H_WB_OTHER);
    unlock_regs();
}

/* Što EDID kaže, u diagnostic log (agastat LOG): čiji je - ime zaslona, koje
   razlikuje AV receiver od TV-a iza njega - koliko je blokova došlo, i svaki
   short audio descriptor kao svoja tri bajta (format i kanali, rateovi,
   veličine; aga_edid.h). */
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

/* ---- boot, jezgra 0, prije 68k: clock i DMA kanal; nijedan HDMI registar */

static void ahi_report(uint32_t fill);     /* brojač AHI ringa (ispod) */

void aga_hdmi_boot(uint8_t *fb_va, uintptr_t fb_phys)
{
    if (aga_hdmi_audio_off) { set_state(H_SWITCHED_OFF); return; }
    ahi_report(0);                       /* "shvaćeno": aga-hdmi.audio traži ga prije nego zatraži ring */
    pi3 = aga_is_pi3();
    hw = pi3 ? &hw_pi3 : &hw_pi4;
    _Static_assert(RING_MAX * 8 + 32 <= AGA_HDMI_AUDIO_SIZE, "the ring and its control block fit AGA_HDMI_AUDIO_OFF's room");
    _Static_assert(AGA_D_HDMI_MAI_ERRS == 499 && AGA_D_HDMI_SCAN_MOVES == 501, "agastat names these by index");
    if (pi3) {                                           /* vidi bilješku o ringu iznad */
        base_ring   = 30 * 192;                          /* 120 ms */
        base_lead   = 2304;                              /* 48 ms */
        base_high   = 4800;
        idle_ms     = 60;                                /* 48 + 32 ms leada i tišine ostaju ispred njega */
    }
    rate_params();
    ring = (volatile uint32_t *)(fb_va + AGA_HDMI_AUDIO_OFF);
    ring_bus = 0xC0000000u | (uint32_t)(fb_phys + AGA_HDMI_AUDIO_OFF);
    cb_bus = ring_bus + RING_MAX * 8;                    /* mjesto control bloka na svakom rateu */

    /* Što zaslon prihvaća (aga_edid.h): njegov EDID, koji daje samo mailbox -
       sada samo moj, pri bootu. Blok 0, pa ekstenzije koje najavljuje (najviše
       3). Zaslon koji ne kaže ništa o audiu, ili nema EDID, dobiva 44.1 i
       48 kHz u 16 bita, kao prije. */
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
        edid_eld(ed, nb, &eld);                          /* Sound monitor ga pokazuje */
        if (a & EDID_SEEN) {
            sink_rates = (a & EDID_RATES) | (1u << R_48K);   /* 48 kHz uvijek: s čime je HDMI zvuk počeo */
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
    ahi_report(0);                                       /* sa zaslonovim rateovima u sebi */

    /* Radi li clock HDMI state machinea, i koliko brzo MAI clock? Pi 3: CPRMAN
       (hsm_pi3). Pi 4: HSM je firmwareov clock 13, IZMJEREN - GET_CLOCK_RATE
       daje rate koji je ARM tražio, a pod Emu68 nitko nije tražio: čitao je 0
       na CM4 koji je prikazivao sliku (2026-10-02). Sam MAI radi na DVP-ovom
       fiksnom 108 MHz. Mailbox je sada samo moj, pa je ovo Pi 4-jev jedini
       pogled. */
    uint32_t clk;
    if (pi3) {
        clk = hsm_pi3() && hsm_div && hsm_known ? mai_hz : 0;
    } else {
        uint32_t hsm = get_clock_rate_measured(HSM_CLOCK);
        aga_diag_set(AGA_D_HDMI_HSM_SET_KHZ, get_clock_rate(HSM_CLOCK) / 1000);
        aga_diag_set(AGA_D_HDMI_HSM_KHZ, hsm / 1000);
        clk = hsm ? 108000000 : 0;
    }
    if (!clk && !aga_hdmi_force) {                       /* aga.hdmiforce: korisnik zna bolje */
        set_state(H_NO_CLOCK);
        kprintf("[AGA] HDMI audio: no HDMI clock (no screen at boot)%s\n",
                pi3 ? " - looked at again while sound is wanted" : " - left alone");
        if (!pi3) return;
    }
    if (!clk) clk = pi3 ? 163682864 : 108000000;        /* forsirano, ili Pi 3 koji čeka: uobičajeni rate */
    mai_hz = clk;
    mai_smp = smp_for(clk, rate_hz[rate_i]);

    /* kanal koji firmware ostavlja ARM-u: brcm,dma-channel-mask */
    dma_mask = hw->dma_mask;
    of_node_t *n = dt_find_node("/soc/dma@7e007000");
    if (!n) n = dt_find_node("/soc/dma-controller@7e007000");
    if (n) dma_mask = dt_get_property_value_u32(n, "brcm,dma-channel-mask", dma_mask, 0);
    dma_pick();                                          /* opet pri svakom startu: jedan može biti zauzet do tada */
    if (!dma_mask) { set_state(H_NO_DMA); return; }
    /* sada samo adrese: vrijednosti se čitaju pri prvom startu, iza guardova - s
       clockom koji ne radi prvo bi čitanje objesilo boot */
    const uint32_t regs[S_N] = { hw->mai_thr, hw->mai_fmt, hw->mai_smp, hw->ch_map, hw->mai_cfg, hw->aud_pkt,
                                 hw->crp, hw->cts0, hw->cts1, hw->phy, hw->mai_ctl };
    for (int i = 0; i < S_N; i++) saved_reg[i] = regs[i];
    ready = 1;
    if (state != H_NO_CLOCK) set_state(H_OFF);
    kprintf("[AGA] HDMI audio: %s on %s (off until the Chipset menu's Sound turns it on), "
            "MAI clock %u Hz, SMP %08x, DMA channel %d\n",
            state == H_NO_CLOCK ? "waiting for the HSM" : "ready", hw->name, mai_hz, mai_smp, dma_ch);
}

/* ---- jezgra 3: mix ulazi -------------------------------------------------- */

static uint32_t wpos;                    /* sljedeći ring frame za upis */
static uint32_t sil_end;                 /* tišina je upisana dovde */
static int32_t  hist[4][2];              /* zadnja četiri ulazna framea */
static uint64_t ph;                      /* 32.32: gdje sam prošao hist[1] */
static uint64_t step_nom;                /* 27,928 / 48,000 u 32.32 (step_rate / step_out) */
static uint32_t step_rate, step_out;     /* ulazni i izlazni rate za koje je step_nom napravljen */
extern uint32_t aga_audio_rate(void);    /* aga_audio.c: sample rate prave Paule */
static int32_t  lead_q4;                 /* lead, izglađen, u 1/16 framea */
static volatile uint64_t last_feed;
/* jezgra 3 i housekeeperov idle fill, koji oba pišu ring: feed se broji
   unutra i van (neparno dok radi), a komad filla piše se samo pod fill_busy,
   nakon fenced pogleda na taj broj - pa ili fill vidi da je feed počeo i ne
   piše ništa, ili feed čeka nekoliko mikrosekundi koliko komad traje. Fill je
   nekada gledao samo last_feed, zabilježen kad feed ZAVRŠAVA, i mogao je
   položiti komad tišine preko zvuka koji je feed pisao (otkrio RGB2RTG). */
static volatile uint32_t feed_seq;
static volatile int fill_busy;

static inline uint32_t dma_frame(int c)
{
    uint32_t off = rd(DMA_SRC(c)) - ring_bus;
    return off < (ring_frames * 8) ? off / 8 : 0;
}

/* A500/A1200 filteri (aga_hdmi_filter.h), isključeni osim ako agaboot SOUND
   FILTER nije odabrao jedan: $5830 off, $5831 A500, $5834 A1200 iz 68k trapa
   na jezgri 0 (tamo obični store), power LED iz chipset loopovog pogleda na
   CIA-A jednom po frameu. Filteri rade na jezgri 3 u feedu, na HDMI rateu
   (hf_k). */
static volatile int filt_mode, filt_led;
static hf_state_t filt[2];
static int filt_seen;

void aga_hdmi_filter(int mode) { filt_mode = (mode == HF_A500 || mode == HF_A1200) ? mode : HF_OFF; }
void aga_hdmi_led(int lit)                 /* jezgra 3, jednom po frameu */
{
    filt_led = lit ? 1 : 0;
    aga_diag_set(AGA_D_HDMI_FILTER, (uint32_t)(filt_mode | (lit ? 16 : 0)));
}

/* Catmull-Rom između b i c; t u 1/65536 */
static inline int32_t cubic(int32_t a, int32_t b, int32_t c, int32_t d, int32_t t)
{
    int64_t t1 = t, t2 = (t1 * t1) >> 16, t3 = (t2 * t1) >> 16;
    int64_t v = 2 * (int64_t)b
              + (((int64_t)(c - a) * t1) >> 16)
              + (((int64_t)(2 * a - 5 * b + 4 * c - d) * t2) >> 16)
              + (((int64_t)(3 * (b - c) + d - a) * t3) >> 16);
    return (int32_t)(v >> 1);
}

/* HDMI volume u Sound prozoru (agaboot SOUND VOLUME, $584n iz 68k trapa na
   jezgri 0: obični store): 100, 150, 200, 300 ili 400 %. 200 je GAIN 2 iz
   1.112 i 1.113; iznad 100 % peakovi se zaokružuju soft kneeom iz aga_mix.h
   umjesto da se clipaju. */
static volatile int32_t hdmi_gain16 = 2 << 16;
void aga_hdmi_volume(int i) { if (i >= 0 && i < 5) hdmi_gain16 = (int32_t)((mix_vol_pc(i) << 16) / 100); }

/* ---- AHI (1.114): stereo drivera aga-hdmi.audio, umiksan ----------------------
   Zvuk AHI programa, koji AHI sam miksa u stereo - 16 bita, ili 32 u svojim
   HiFi modovima - ide u ring u 68k-ovom fast RAM-u (tools/ahi/aga-hdmi.c) -
   Pi memorija, identity mapped, cached i coherent, pa se čita ovdje gdje
   leži. HDMI radi na rateu ringa (set_rate), a feed uzima jedan ring frame za
   svaki HDMI frame koji upiše, dodan Paulinom mixu nakon njegove glasnoće i
   filtera (AHI-jev zvuk je masteran na punoj skali, a A500/A1200 filteri su
   Amigin analogni izlaz, ne ovo), a izvan sandboxa jezgra 3 vrti feed za njega
   s tihom Paulom (aga_hdmi_pump). Ringov header, big-endian kao sve što 68k
   piše: magic, frames (potencija dvojke), frameovi upisani i frameovi
   pročitani od početka - driver piše prvo, ja drugo, svaki brojeći dalje bez
   wrapanja na ringu - rate, i flagovi: bit 0 frameovi su dva 32-bitna uzorka
   (inače dva 16-bitna). Driver objavljuje svoj broj CAS-om, koji JIT zatvara
   barijerom, nakon uzoraka; ja čitam broj acquiring, pa su uzorci prije
   njega tu. Riječi: $58A0 + E4(n) još četiri bita adrese ringa, najviši prvi;
   $5895 ring s te adrese, $5894 nijedan (oba iz jezgre 0 trapa).

   Brojač AGA_D_AHI (511), koji driver čita: bit 0 kernel prima ringove, bit 1
   jedan je uključen, bit 2 HDMI zvuk svira, bit 3 zaslon prima 24 bita, bitovi
   4-9 rateovi koje prima (redoslijed rate_hz; driver nudi one), bitovi 10-12
   rate na kojem HDMI radi, bit 13 ringovi frameovi su 32 bita, bit 14 Paula
   band-limited, bit 15 zna AHI_LOWLAT (1.116); bitovi 16-23 frameovi koji
   čekaju / 64, bitovi 24-31 koliko je puta ring ostao bez podataka (najviše
   255). */
extern uintptr_t mmu_virt2phys(uintptr_t virt);
#define AHI_MAGIC 0x41484931u                    /* "AHI1" */
#define AHI_S32   1u                             /* flagovi: 32-bitni uzorci */
#define AHI_LOWLAT 2u                            /* ... low latency: 10 ms start buffer (1.116, driver 6.2) */
struct ahi_hdr { uint32_t magic, frames, wr, rd, rate, flags, res[2]; };
static struct ahi_hdr *volatile ahi_live __attribute__((aligned(64)));   /* vlastita linija: jezgra 3 polla ga parkirana */
static uint32_t ahi_latch;
static struct ahi_hdr *ahi_cur;                  /* jezgre 3: ring koji feed čita */
static uint32_t ahi_rd, ahi_mask, ahi_under;
static const int16_t *ahi_pcm;
static const int32_t *ahi_pcm32;                 /* ... kad su njegovi frameovi 32-bitni */
static int ahi_seen;                             /* driver je nešto upisao */
/* Start buffer (1.116, RGB2RTG-ov 0.75-dev53): ring se reproducira kad je
   driver 40 ms ispred, ili 100 ms nakon što je došao njegov prvi frame - nov,
   ili nakon što je ostao bez podataka. Odmah pročitan, player koji je stao
   odmah nakon prvog buffera (onaj koji otvara file na SD kartici, čiji
   driver vrti dok čita) ostavljao je rupe, čulo se kao krckanje; zadržan,
   njegov start dolazi 40 ms kasnije, cijeli. Ring koji ostane bez podataka
   čeka isto: jedna praznina, ne krckanje mnogih. 40 ms: driver 6.2 drži 42 ms
   više od svojih buffera; stariji, nikad toliko ispred s malim bufferima,
   počinje nakon 100 ms. Low latency ring (AHI_LOWLAT, driver 6.2 low latency
   modovi, oko buffer i 12 ms) počinje na 10 ms: zvučni efekti igre čuju se
   ranije, zastoj 68k-a preko te margine praznina. */
static int ahi_primed;                           /* feed reproducira ring */
static uint64_t ahi_wait_t0;                     /* došao je prvi frame čekanja (0 još nijedan) */
static int ahi_lowlat;                           /* ring ima AHI_LOWLAT */
static volatile int paula_bl;                    /* Paula na HDMI band-limited (ispod) */

static void ahi_report(uint32_t fill)
{
    struct ahi_hdr *h = ahi_live;
    uint32_t u = ahi_under > 255 ? 255 : ahi_under;
    aga_diag_set(AGA_D_AHI, 1u | (h ? 2u : 0u) | (state == H_RUNNING ? 4u : 0u) | (out_24() ? 8u : 0u)
                            | (offer_rates() & 0x3F) << 4 | (uint32_t)rate_i << 10
                            | (h && (h->flags & AHI_S32) ? 1u << 13 : 0u) | (paula_bl ? 1u << 14 : 0u)
                            | 1u << 15                    /* 1.116: zna AHI_LOWLAT */
                            | (fill / 64 > 255 ? 255u : fill / 64) << 16 | u << 24);
}

/* Drugi rate (jezgra 0: AHI ring koji ga traži, sandboxov start). Zvuk koji
   svira staje kako ga Off zaustavlja - prvo state, koji feed i housekeeperovi
   fillovi gledaju, zatim DMA - a feed koji je već u tijeku pušta se da
   završi: broji se sa starim ringom. Zatim ring, tablice i MAI clock za novi
   rate, ring sva tišina, i start: MAI programiran iznova. Zaslon se
   zaključava na novi rate, kratka praznina - neizbježno, jebiga. Start koji
   guard sada ne pušta je housekeeperov, sekundu kasnije, kao nakon
   izgubljenog linka. force: isto na istom rateu, za nove tablice (16 ili 24
   bita). */
static void set_rate(int i, int force)
{
    if (!ready || (i == rate_i && !force)) return;
    lock_regs();
    int was = state == H_RUNNING;
    if (was) {
        set_state(H_OFF);
        uint64_t t = cntvct() + cntfrq() / 10000;
        while (cntvct() < t) ;
        t = cntvct() + cntfrq() / 50;                    /* feed je dobro ispod milisekunde: najviše 20 ms */
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

/* sandbox počinje: opet 48 kHz, osim ako AHI program svira dalje - ili fiksni
   rate koji je Sound prozor odabrao */
void aga_hdmi_rate_default(void)
{
    int f = fixed_ri[rate_mode];
    if (f >= 0) set_rate(f, 0);
    else if (!__atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE)) set_rate(R_48K, 0);
    ahi_report(0);
}

/* $5868 + E2(m), jezgra 0: HDMI sound rate u Sound prozoru (rate_mode). HDMI
   ide na fiksni rate, ili ostaje na onom koji se još nudi, inače na 48 kHz;
   AHI ring na rateu koji se više ne nudi pušta se - njegov driver tada sam
   drži vrijeme, tiho, dok program ne počne opet. */
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

/* cijeli raspon [a, a + n) identity-mapped Pi memorije (68k fast RAM) */
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
    /* longword-aligned je sve što header treba: AllocVec dijeli blokove četiri
       bajta iza osam-bajtne granice (nađeno na A500, 2026-10-11) */
    int why = a < 0x200000u ? 1 : (a & 3) ? 2 : a > 0xFFF00000u ? 3 : !ahi_mapped(a, sizeof(struct ahi_hdr)) ? 4 : 0;
    struct ahi_hdr *h = (struct ahi_hdr *)(uintptr_t)a;
    uint32_t n = why ? 0 : h->frames;
    int ri = -1;                                  /* ringov rate, jedan koji zaslon prima */
    for (int i = 0; !why && i < N_RATES; i++)
        if (h->rate == rate_hz[i] && (offer_rates() & (1u << i))) ri = i;
    uint32_t fsize = !why && (h->flags & AHI_S32) ? 8 : 4;
    if (!why) why = h->magic != AHI_MAGIC ? 5 : ri < 0 ? 6 : (n < 256 || n > 65536 || (n & (n - 1))) ? 7
                  : !ahi_mapped(a, sizeof(struct ahi_hdr) + n * fsize) ? 8 : (h->flags & ~(AHI_S32 | AHI_LOWLAT)) ? 9 : 0;
    if (why) {
        /* "[AGA] AHI: ring at XXXXXXXX refused (N)": 1 chip RAM, 2 alignment, 3
           range, 4 nije mapiran, 5 magic, 6 rate koji zaslon ne prima, 7
           veličina, 8 frameovi nisu mapirani, 9 flagovi koje ovaj kernel ne
           zna */
        static char line[] = "[AGA] AHI: ring at XXXXXXXX refused (N)";
        for (int i = 0; i < 8; i++) line[19 + i] = "0123456789abcdef"[(a >> (28 - 4 * i)) & 15];
        line[37] = (char)('0' + why);
        aga_diag_log(line);
        return;
    }
    set_rate(ri, 0);                              /* HDMI na rateu ringa - prije nego ga feed pročita */
    __atomic_store_n(&ahi_live, h, __ATOMIC_RELEASE);
    ahi_report(0);
    __asm__ volatile("sev");                      /* jezgra 3, parkirana: vrti feed za njega */
    aga_diag_log("[AGA] AHI: ring on");
}

int aga_hdmi_ahi_live(void) { return __atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE) != 0; }

/* ---- HiFi volume (1.118): zvuk AHI programa, koji HDMI volume ostavlja na miru -
   HDMI volume ($5840) je Paulin: AHI-jev zvuk dolazi masteran na punoj skali i
   dodaje se nakon njega. Ovo AHI-jev stišava zasebno, d dB, iz agabootovog
   Sound monitora - 0 do 62, 63 tišina. 65536 je 0 dB: na njemu ringovi
   frameovi dolaze na HDMI kao prije, do bita. $5180 + E6(d), iz jezgre 0
   trapa; feed ga uzima pri svom startu. */
static const int32_t hifi_gain_tab[63] = {
    65536, 58409, 52057, 46396, 41350, 36854, 32846, 29274, 26090, 23253, 20724, 18471, 16462, 14672, 13076, 11654,
    10387, 9257, 8250, 7353, 6554, 5841, 5206, 4640, 4135, 3685, 3285, 2927, 2609, 2325, 2072, 1847,
    1646, 1467, 1308, 1165, 1039, 926, 825, 735, 655, 584, 521, 464, 414, 369, 328, 293,
    261, 233, 207, 185, 165, 147, 131, 117, 104, 93, 83, 74, 66, 58, 52 };
static volatile int32_t hifi_gain16 = 65536;
static volatile uint32_t hifi_db;

void aga_hdmi_hifi_volume(int d)
{
    if (d < 0 || d > 63) return;
    hifi_db = (uint32_t)d;
    __atomic_store_n(&hifi_gain16, d >= 63 ? 0 : hifi_gain_tab[d], __ATOMIC_RELEASE);
}

/* ---- Sound monitor (1.118): što HDMI reproducira, za agabootov prozor ------
   agabootov Sound monitor (tools/agaboot/monitor.c) posuđuje ring u 68k-ovom
   fast RAM-u, kako to čini i aga-hdmi.audio: header, pa frameovi od dva
   16-bitna uzorka - gornjih 16 od 24 koliko HDMI ring dobiva, nakon volumea,
   filtera i AHI mixa: što reproduciraju zvučnici zaslona. Jezgra 3 ih piše
   kako feed piše HDMI ring, a prozor crta svoje metre, scope i spektar iz
   njih svojim tempom, bez trapa po uzorku. Header, big-endian kako ga 68k
   čita. Prozor piše magic i frames (potencija dvojke, 2048 do 65536) i
   nulira dva peaka nakon što ih pročita; ostatak je moj, upisan pri svakom
   kraju feeda: frameovi upisani od početka (brojeći dalje preko ringa), rate,
   najveći uzorak od zadnjeg pogleda prozora, lijevo i desno (24 bita), state
   (MON_* ispod), lead u frameovima, resynci, AHI frameovi koji čekaju i
   koliko je puta AHI ring ostao bez podataka, rateovi koje zaslon prima
   (redoslijed rate_hz) i HiFi volume. Nema feeda - nema željenog zvuka na
   HDMI - i broj stoji, po čemu prozor to i prepoznaje. Riječi: $5140 + E4(n)
   još četiri bita adrese ringa, najviši prvi; $5161 ring s te adrese, $5160
   nijedan, što vraća tek kad nijedan feed ne piše stari ring, pa ga prozor
   smije odmah osloboditi.
   Prozorovi count beatovi tuku nekoliko puta u sekundi, pauziran ili ne. Ring
   čiji beat stoji 3 s, ili čiji je magic otišao, pušta se prije nego feed piše
   u njega - prozor koji je umro, ili memorija koju je soft reboot predao
   (68k-ov RESET pušta ga također, aga_machine_reset). State 0 tada kaže
   prozoru da je samo zadržan da bi ga ponovno posudio.
   Header je 256 bajtova: nakon brojeva, limiterov najmanji gain od kad je
   prozor pogledao (Q16; prozor vraća 65536), efekti kako ih kernel ima
   (fx_word), i zaslonov ELD iz njegovog EDID-a - ime, short audio descriptore
   i zvučnici (aga_edid.h), postavljeno kad se ring uzme. Zatim Paulini
   glasovi: prozor koji ih traži (vframes, potencija dvojke, 1024 do 65536; 0
   nijedan) dobiva drugi ring nakon frameova, jedna riječ po frameu mixovog
   vlastitog ratea (vrate) - glas 0 u gornjem bajtu (core/src/audio.c,
   aga_audio_voices) - sa svojim brojem (vwr), registrima svakog glasa, panom
   i levelom, i vstate: 1 glasovi dolaze (igra u sandboxu), 2 voice mixer je
   uključen. */
#define MON_MAGIC   0x4D4F4E31u                  /* "MON1" */
#define MON_TAKEN   0x80000000u                  /* kernel je uzeo ring */
#define MON_HDMI    1u                           /* HDMI zvuk svira */
#define MON_AHI     2u                           /* AHI ring je uključen */
#define MON_AHIPLAY 4u                           /* ... i feed ga sada reproducira */
#define MON_AHI32   8u                           /* ... 32-bitni frameovi (HiFi) */
#define MON_LOWLAT  16u                          /* ... low latency */
#define MON_OUT24   32u                          /* zaslon dobiva 24 bita */
#define MON_PAULABL 64u                          /* Paula na HDMI band-limited */
#define MON_PAULA   128u                         /* zvuk emuliranog chipseta u njemu (sandbox je gore) */
/* 256 i 512 su RGB2RTG-ovi (MON_TONE, MON_LEDF - njegov monitor dijeli ovaj
   header, EMM #2188): drži ih slobodnima ovdje */
struct mon_voice { uint32_t lc; uint16_t len, per; uint8_t vol, flags, pan, level; };
struct mon_hdr {
    uint32_t magic, frames, wr, rate, peak_l, peak_r, state, lead, resyncs, ahi_fill, ahi_under, offer, hifi_db, beat, lim, fx;
    struct edid_eld eld;                         /* 49 bajtova */
    uint8_t pad[64 - sizeof(struct edid_eld)];
    uint32_t vframes, vwr, vrate, vstate;
    struct mon_voice v[4];
    uint8_t pad2[64];
};
_Static_assert(sizeof(struct mon_voice) == 12, "monitor.c's voice is 12 bytes");
_Static_assert(sizeof(struct mon_hdr) == 256, "monitor.c's header is 256 bytes");
static struct mon_hdr *volatile mon_live;
static uint32_t mon_latch;
static struct mon_hdr *mon_cur;                  /* jezgre 3: ring koji feed piše */
static uint32_t mon_wr, mon_mask, mon_pk_l, mon_pk_r, mon_resyncs, mon_beat;
static uint32_t *mon_vring, mon_vmask, mon_vwr, mon_vrd;   /* Paulini glasovi u drugi ring prozora */
static uint64_t mon_beat_t;                      /* kad se beat zadnji put pomaknuo */
static int16_t *mon_pcm;

void aga_hdmi_monitor_nibble(int d) { mon_latch = (mon_latch << 4) | ((uint32_t)d & 15u); }

void aga_hdmi_monitor(int on)
{
    if (!on) {
        __atomic_store_n(&mon_live, (struct mon_hdr *)0, __ATOMIC_RELEASE);
        __asm__ volatile("dmb ish" ::: "memory");
        /* feed koji je u tijeku može još držati stari ring: pusti ga da završi
           (dobro ispod milisekunde; najviše 20 ms, kako čeka set_rate) */
        uint64_t t = cntvct() + cntfrq() / 50;
        while ((__atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST) & 1) && cntvct() < t) __asm__ volatile("yield");
        return;
    }
    uint32_t a = mon_latch;
    int why = a < 0x200000u ? 1 : (a & 3) ? 2 : a > 0xFFF00000u ? 3 : !ahi_mapped(a, sizeof(struct mon_hdr)) ? 4 : 0;
    struct mon_hdr *m = (struct mon_hdr *)(uintptr_t)a;
    uint32_t n = why ? 0 : m->frames, vn = why ? 0 : m->vframes;
    if (!why) why = m->magic != MON_MAGIC ? 5 : (n < 2048 || n > 65536 || (n & (n - 1))) ? 7
                  : (vn && (vn < 1024 || vn > 65536 || (vn & (vn - 1)))) ? 6
                  : !ahi_mapped(a, sizeof(struct mon_hdr) + n * 4 + vn * 4) ? 8 : 0;
    if (why) {
        static char line[] = "[AGA] monitor: ring at XXXXXXXX refused (N)";
        for (int i = 0; i < 8; i++) line[23 + i] = "0123456789abcdef"[(a >> (28 - 4 * i)) & 15];
        line[41] = (char)('0' + why);
        aga_diag_log(line);
        return;
    }
    m->wr = 0;
    m->peak_l = m->peak_r = 0;
    m->rate = rate_hz[rate_i];
    m->offer = offer_rates();
    m->hifi_db = hifi_db;
    m->lim = 65536;
    m->fx = 0;
    m->eld = eld;
    m->vwr = 0;
    m->vstate = 0;
    m->state = MON_TAKEN | (state == H_RUNNING ? MON_HDMI : 0);
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(&mon_live, m, __ATOMIC_RELEASE);
}

/* ---- efekti (1.118, aga_fx.h): equalizer, preamp, crossfeed, balance,
   limiter - agabootov Equalizer prozor. Dvije riječi po postavci: $5200 + E5(v)
   vrijednost (0-31), pa $5240 + E4(k) daje je postavci k (aga_fx.h redoslijed:
   bendovi 0-9, preamp, on, limiter, balance, crossfeed). Iz jezgre 0 trapa
   samo se brojevi pamte; jezgra 3 izračuna filtere iz njih pri sljedećem feedu
   (floating point, nikad u trapu). Off i flat dok ih agaboot ne pošalje, kao
   prije. */
static uint32_t fx_latch;
static volatile int8_t fx_req[FX_NPARAM];
static volatile uint32_t fx_seq;
static uint32_t fx_seen = ~0u;
static int fx_ri = -1;
static fx_t fx;

void aga_hdmi_fx_value(int v) { fx_latch = (uint32_t)v & 31u; }

void aga_hdmi_fx_param(int k)
{
    if (k < 0 || k >= FX_NPARAM) return;
    fx_req[k] = fx_decode(k, fx_latch);
    __atomic_add_fetch(&fx_seq, 1, __ATOMIC_RELEASE);
}

/* efekti kako ih kernel ima, za monitor: on, limiter, crossfeed (2 bita),
   preamp + 12 (5), balance + 12 (5), bendovi koji nisu flat (4) */
/* ---- Paulini glasovi (1.118): voice mixer, i što svaki glas reproducira za
   Sound monitor. Mixerove riječi: vrijednosna riječ iznad ($5200 + E5(v)), pa
   $5260 + E4(k): k 0-3 pan glasa k (v - 8: -8 lijevo .. 8 desno), 4-7 level
   glasa k-4 (0 tiho, 1-16: -30 dB .. 0 dB u koracima od 2 dB), 8 mixer
   uključen (v & 1). Isključeno, ili svaki glas na svojoj strani na 0 dB, mix
   je kao prije do bita (host/tests/test_audio.c, test_blep.c). Glue daje
   gainove jezgri (oba izlaza), a band-limited put ovdje ih također uzima. */
static volatile int8_t vreq_pan[4] = { -8, 8, 8, -8 }, vreq_lev[4] = { 16, 16, 16, 16 };
static volatile int vreq_on;
static volatile uint32_t vseq;
static uint32_t blep_vseen = ~0u;
static const int16_t pan_cos[17] = { 256, 255, 251, 245, 237, 226, 213, 198, 181, 162, 142, 121, 98, 74, 50, 25, 0 };
static const int16_t lev_q8[17] = { 0, 8, 10, 13, 16, 20, 26, 32, 41, 51, 64, 81, 102, 128, 162, 203, 256 };

void aga_hdmi_voice_param(int k)
{
    int v = (int)fx_latch;
    if (k >= 0 && k < 4) vreq_pan[k] = (int8_t)(v > 16 ? 8 : v - 8);
    else if (k >= 4 && k < 8) vreq_lev[k - 4] = (int8_t)(v > 16 ? 16 : v);
    else if (k == 8) vreq_on = v & 1;
    else return;
    __atomic_add_fetch(&vseq, 1, __ATOMIC_RELEASE);
}

/* gain svakog glasa u lijevo i desno, Q8: konstantna snaga preko pana */
void aga_hdmi_voice_gains(int *on, int32_t gl[4], int32_t gr[4])
{
    *on = vreq_on;
    for (int n = 0; n < 4; n++) {
        int i = vreq_pan[n] + 8, g = lev_q8[vreq_lev[n]];
        gl[n] = (pan_cos[i] * g) >> 8;
        gr[n] = (pan_cos[16 - i] * g) >> 8;
    }
}

/* što su glasovi odsvirali od zadnjeg poziva, iz gluea (jezgra 3) - drži se
   ovdje dok ih feed ne preda monitorovom ringu */
#define VBUF 8192
static uint32_t vbuf[VBUF], vbuf_wr, vrate_now;
static struct aga_voice vinfo[4];
static uint64_t vlast_t;

void aga_hdmi_voices(const uint32_t *v, int n, const struct aga_voice *info, uint32_t rate)
{
    for (int i = 0; i < n; i++) vbuf[vbuf_wr++ & (VBUF - 1)] = v[i];
    for (int i = 0; i < 4; i++) vinfo[i] = info[i];
    vrate_now = rate;
    vlast_t = cntvct();
}

static uint32_t fx_word(void)
{
    return (uint32_t)(fx_req[FX_ON] & 1) | (uint32_t)(fx_req[FX_LIM] & 1) << 1 | (uint32_t)(fx_req[FX_XFEED] & 3) << 2
         | (uint32_t)(fx_req[FX_PRE] + 12) << 4 | (uint32_t)(fx_req[FX_BAL] + 12) << 9 | (uint32_t)fx.nb << 14;
}

/* ---- feed: što dijele oba Paulina puta ------------------------------------- */

struct feed {
    int ri, fm, fl, resynced;
    const int32_t *fk;
    int32_t gain16, ppm;
    struct ahi_hdr *ah;
    uint32_t ahi_wr;
    int ahi_hold;                        /* ring je ostao bez podataka i čeka: ovi frameovi su rupe */
    int32_t ahi_g;                       /* HiFi volume, 65536 = 0 dB */
    uint32_t lead;
    struct mon_hdr *mon;                 /* Sound monitorov ring, ili nijedan */
    int fx;                              /* efekti rade (aga_fx.h) */
};

/* Feedov start: 0 kad sada ne smije raditi. Broji se unutra, resynca na
   pauzu ili lead izvan svog raspona (f->resynced: i pozivateljev resampler
   počinje iznova), izračuna rate trim, i uzima filtere, volume i AHI ring
   kakvi su sada. */
static int feed_begin(struct feed *f)
{
    /* acquiring: ringovi brojevi koje je set_rate upisao prije starta se vide */
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != H_RUNNING) return 0;
    int c = dma_ch;                      /* housekeeper može ispustiti kanal uzet od mene */
    if (c < 0) return 0;
    __atomic_add_fetch(&feed_seq, 1, __ATOMIC_SEQ_CST);  /* neparno: feed radi */
    __asm__ volatile("dmb ish" ::: "memory");
    while (__atomic_load_n(&fill_busy, __ATOMIC_SEQ_CST)) __asm__ volatile("yield");
    f->ri = rate_i;
    f->resynced = 0;
    uint32_t rpos = dma_frame(c);
    uint32_t lead = (wpos + ring_frames - rpos) % ring_frames;
    /* Pauza se procjenjuje ovdje kao i housekeeperovim flagom: feed koji je
       počeo baš kad je housekeeper odlučio o fillu mogao je pročitati flag
       prije nego je postavljen (otkrio review). Signed: last_feed je moj. */
    int stale = (int64_t)(cntvct() - last_feed) > (int64_t)(cntfrq() * idle_ms / 1000);
    int was_idle = __atomic_load_n(&idle, __ATOMIC_SEQ_CST) || stale;
    if (was_idle || lead < lead_low || lead > lead_high) {
        /* (re)start leada ispred DMA, tišina između i dalje */
        if (!was_idle) { aga_diag_inc(AGA_D_HDMI_RESYNCS); mon_resyncs++; }
        wpos = (rpos + lead_target) % ring_frames;
        silence((rpos + 8) % ring_frames, lead_target - 8 + horizon);
        sil_end = (wpos + horizon) % ring_frames;
        lead = lead_target;
        lead_q4 = lead_target << 4;
        /* filteri također: ring prije wpos je tišina, pa nastavljaju iz tišine, ne
           iz zvuka prije pauze (otkrio review) */
        for (int c2 = 0; c2 < 2; c2++)
            for (int k = 0; k < 5; k++) filt[c2].rc[k] = 0;
        f->resynced = 1;
        __atomic_store_n(&idle, 0, __ATOMIC_RELEASE);
    }
    /* rate: Paulin clock prema HDMI-jevom, triman leadom - 4 ppm po frameu
       greške, najviše 0.3% (5 centi, nečujno) */
    lead_q4 += ((int32_t)(lead << 4) - lead_q4) >> 5;
    int32_t ppm = ((lead_q4 >> 4) - lead_target) * 4;
    if (ppm > 3000) ppm = 3000;
    if (ppm < -3000) ppm = -3000;
    f->ppm = ppm;
    aga_diag_set(AGA_D_HDMI_LEAD, lead);
    aga_diag_set(AGA_D_HDMI_PPM, (uint32_t)ppm);
    f->lead = lead;
    f->fm = filt_mode;
    f->fl = filt_led;
    f->fk = hf_k[f->ri];
    f->gain16 = hdmi_gain16;
    f->ahi_g = __atomic_load_n(&hifi_gain16, __ATOMIC_ACQUIRE);
    /* efekti: izračunati ponovno kad se postavka ili rate promijenio */
    uint32_t fq = __atomic_load_n(&fx_seq, __ATOMIC_ACQUIRE);
    if (fq != fx_seen || f->ri != fx_ri) {
        int8_t set[FX_NPARAM];
        for (int k = 0; k < FX_NPARAM; k++) set[k] = fx_req[k];
        fx_design(&fx, set, rate_hz[f->ri]);
        fx_seen = fq;
        fx_ri = f->ri;
    }
    f->fx = fx.on;
    /* Sound monitor: njegov ring sada (novi se broji odakle ga je njegov prozor
       postavio) */
    struct mon_hdr *mh = __atomic_load_n(&mon_live, __ATOMIC_ACQUIRE);
    if (mh != mon_cur) {
        mon_cur = mh;
        if (mh) {
            mon_mask = mh->frames - 1;
            mon_pcm = (int16_t *)(mh + 1);
            mon_vmask = mh->vframes ? mh->vframes - 1 : 0;
            mon_vring = mh->vframes ? (uint32_t *)((uint8_t *)(mh + 1) + mh->frames * 4) : (uint32_t *)0;
            mon_vwr = mh->vwr;
            mon_vrd = vbuf_wr;                   /* od sada */
            mon_wr = mh->wr;
            mon_pk_l = mon_pk_r = 0;
            mon_beat = mh->beat;
            mon_beat_t = cntvct();
        }
    }
    if (mh) {                            /* je li njegov prozor još tu? */
        uint64_t now = cntvct();
        if (mh->beat != mon_beat) { mon_beat = mh->beat; mon_beat_t = now; }
        if (mh->magic != MON_MAGIC || now - mon_beat_t > 3 * cntfrq()) {
            struct mon_hdr *e = mh;
            /* prvo pusti, pa state 0: prozor koji ga posudi opet na 0 nije od mene
               pušten ni trenutak kasnije */
            if (__atomic_compare_exchange_n(&mon_live, &e, (struct mon_hdr *)0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
                && mh->magic == MON_MAGIC)
                __atomic_store_n(&mh->state, 0u, __ATOMIC_RELEASE);
            aga_diag_log("[AGA] monitor: its window went quiet - ring let go");
            mon_cur = (struct mon_hdr *)0;
            mh = (struct mon_hdr *)0;
        }
    }
    f->mon = mh;
    /* AHI: ring sada, i što je driver upisao u njega */
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
    if (ah && f->ahi_wr - ahi_rd > ahi_mask + 1) ahi_rd = f->ahi_wr - (ahi_mask + 1) / 2;   /* zaostao sam cijeli krug: pola ringa */
    f->ahi_hold = 0;
    if (ah && !ahi_primed) {                             /* start buffer */
        uint64_t at = cntvct();
        uint32_t fill = f->ahi_wr - ahi_rd;
        if (!fill)
            ahi_wait_t0 = 0;
        else if (!ahi_wait_t0)
            ahi_wait_t0 = at;
        if (fill >= at_rate(ahi_lowlat ? 480 : 1920) || (fill && at - ahi_wait_t0 >= cntfrq() / 10)) {
            ahi_primed = 1;
        } else {
            f->ah = 0;                                   /* još ne: samo Paula, ring zadržan */
            f->ahi_hold = ahi_seen;                      /* nakon što je ostao bez podataka: rupa */
        }
    }
    if (f->fm != filt_seen) {                            /* novi model počinje iz tišine */
        for (int c2 = 0; c2 < 2; c2++)
            for (int k = 0; k < 5; k++) filt[c2].rc[k] = 0;
        filt_seen = f->fm;
    }
    return 1;
}

/* Jedan HDMI uzorak van: Paulin, na 16-bitnoj skali i prije filtera, kroz
   filtere i volume u gornjih 16 od 24, s dodanim frameom AHI ringa. */
static inline void feed_emit(struct feed *f, int32_t pl, int32_t pr)
{
    int32_t l = 256 * mix_level(hf_run_k(&filt[0], f->fm, f->fl, pl, f->fk), f->gain16);
    int32_t r = 256 * mix_level(hf_run_k(&filt[1], f->fm, f->fl, pr, f->fk), f->gain16);
    if (f->ah) {                                         /* jedan AHI frame za svaki HDMI frame, isti rate */
        if (ahi_rd != f->ahi_wr) {
            uint32_t k = ahi_rd & ahi_mask;
            if (ahi_pcm32) {                             /* 32 bita: gornjih 24, na HiFi volumeu */
                l += (int32_t)(((int64_t)ahi_pcm32[2 * k] * f->ahi_g) >> 24);
                r += (int32_t)(((int64_t)ahi_pcm32[2 * k + 1] * f->ahi_g) >> 24);
            } else {                                     /* 16 bita: 256 * s na 0 dB, kao prije */
                l += (int32_t)(((int64_t)ahi_pcm[2 * k] * f->ahi_g) >> 8);
                r += (int32_t)(((int64_t)ahi_pcm[2 * k + 1] * f->ahi_g) >> 8);
            }
            ahi_rd++;
            ahi_seen = 1;
        } else if (ahi_seen) {
            ahi_under++;
            if (ahi_primed) {                            /* bez podataka: opet start buffer */
                ahi_primed = 0;
                ahi_wait_t0 = 0;
            }
        }
    } else if (f->ahi_hold) {
        ahi_under++;
    }
    if (f->fx) fx_run(&fx, &l, &r);                     /* efekti, na cijeli mix */
    l = clamp24(l);                                      /* već u rasponu bez AHI-ja ili efekata */
    r = clamp24(r);
    if (f->mon) {                                        /* Sound monitor: gornjih 16 od onoga što izlazi */
        int32_t ml = l, mr = r;                          /* clampano iznad */
        uint32_t k = mon_wr & mon_mask;
        mon_pcm[2 * k] = (int16_t)(ml >> 8);
        mon_pcm[2 * k + 1] = (int16_t)(mr >> 8);
        mon_wr++;
        uint32_t al = (uint32_t)(ml < 0 ? -ml : ml), ar = (uint32_t)(mr < 0 ? -mr : mr);
        if (al > mon_pk_l) mon_pk_l = al;
        if (ar > mon_pk_r) mon_pk_r = ar;
    }
    put_frame(wpos, l, r);
    if (++wpos >= ring_frames) wpos = 0;
}

/* Feedov kraj: tišina držana ispred, pročitani broj AHI ringa objavljen, i
   feed odbrojan van. */
static void feed_end(struct feed *f)
{
    /* drži horizon frameova tišine iza zvuka, u slučaju da stane ovdje */
    uint32_t have = (sil_end + ring_frames - wpos) % ring_frames;
    if (have > horizon) have = 0;                        /* prešao sam ga */
    silence((wpos + have) % ring_frames, horizon - have);
    sil_end = (wpos + horizon) % ring_frames;
    if (f->ah) {
        __atomic_store_n(&f->ah->rd, ahi_rd, __ATOMIC_RELEASE);   /* driver se njime tempira */
        ahi_report(f->ahi_wr - ahi_rd);
    }
    if (f->mon) {                                        /* Sound monitor: broj nakon frameova */
        struct mon_hdr *m = f->mon;
        struct ahi_hdr *ah = __atomic_load_n(&ahi_live, __ATOMIC_ACQUIRE);
        if (mon_pk_l > m->peak_l) m->peak_l = mon_pk_l;
        if (mon_pk_r > m->peak_r) m->peak_r = mon_pk_r;
        mon_pk_l = mon_pk_r = 0;
        m->rate = rate_hz[f->ri];
        m->lead = f->lead;
        m->resyncs = mon_resyncs;
        m->ahi_fill = f->ah ? f->ahi_wr - ahi_rd : 0;
        m->ahi_under = ahi_under;
        m->offer = offer_rates();
        m->hifi_db = hifi_db;
        uint32_t lq = (uint32_t)fx.lmin >> 14;           /* Q30 u Q16 */
        if (lq < m->lim) m->lim = lq;
        fx.lmin = FX_ONE;
        m->fx = fx_word();
        if (mon_vring) {                                 /* Paulini glasovi, kako ih je glue dao */
            if (vbuf_wr - mon_vrd > VBUF) mon_vrd = vbuf_wr - VBUF;
            while (mon_vrd != vbuf_wr) mon_vring[mon_vwr++ & mon_vmask] = vbuf[mon_vrd++ & (VBUF - 1)];
            for (int i = 0; i < 4; i++) {
                m->v[i].lc = vinfo[i].lc;
                m->v[i].len = vinfo[i].len;
                m->v[i].per = vinfo[i].per;
                m->v[i].vol = vinfo[i].vol;
                m->v[i].flags = vinfo[i].flags;
                m->v[i].pan = (uint8_t)(vreq_pan[i] + 8);
                m->v[i].level = (uint8_t)vreq_lev[i];
            }
            m->vrate = vrate_now;
            m->vstate = ((int64_t)(cntvct() - vlast_t) < (int64_t)(cntfrq() / 10) ? 1u : 0u) | (vreq_on ? 2u : 0u);
            __atomic_store_n(&m->vwr, mon_vwr, __ATOMIC_RELEASE);
        }
        m->state = MON_TAKEN | MON_HDMI | (ah ? MON_AHI : 0) | (f->ah ? MON_AHIPLAY : 0)
                 | (ah && (ah->flags & AHI_S32) ? MON_AHI32 : 0) | (ah && (ah->flags & AHI_LOWLAT) ? MON_LOWLAT : 0)
                 | (out_24() ? MON_OUT24 : 0) | (paula_bl ? MON_PAULABL : 0) | (aga_enabled ? MON_PAULA : 0);
        __atomic_store_n(&m->wr, mon_wr, __ATOMIC_RELEASE);
    }
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(&last_feed, cntvct(), __ATOMIC_RELEASE);
    __atomic_add_fetch(&feed_seq, 1, __ATOMIC_SEQ_CST);  /* parno: gotovo */
}

/* Classic: isti frameovi koje dobiva aga_audio_stream - stereo, na rateu
   prave Paule - resamplani na HDMI-jev Catmull-Romom */
void aga_hdmi_feed(const int16_t *lr, int frames)
{
    struct feed f;
    if (frames <= 0 || !feed_begin(&f)) return;
    /* rate kojim frameovi dolaze (aga_audio_rate): prave Paule, 27,928 u sekundi
       na PAL kristalu - držan na 1.112-inih 3546895 / 127 do bita - i 27,965
       na NTSC-ovom, gdje glue sada traži toliko od mixa - i rate na kojem HDMI
       radi */
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

/* ---- Paula band-limited (aga_blep.h): "Paula on HDMI" u Sound prozoru ----
   $5899 band-limited, $5898 classic (iz jezgre 0 trapa: obični store); glue
   traži od jezgre Pauline korake dok je uključeno i predaje ih ovdje umjesto
   classic frameova. */
static blep_t blep;
static int blep_inited, blep_live;
static uint64_t blep_now;                /* colour clock do kojeg je zadnji feed stigao */

void aga_hdmi_paula(int bl)
{
    paula_bl = bl ? 1 : 0;
    ahi_report(0);
    aga_diag_log(bl ? "[AGA] HDMI audio: Paula band-limited" : "[AGA] HDMI audio: Paula classic");
}
int aga_hdmi_paula_bl(void) { return paula_bl; }

/* Paulini koraci od zadnjeg poziva (aga_audio_steps), svaki korak prije
   colour clocka sada među njima; cck_hz colour clockovi koje emulacija vrti
   u sekundi */
void aga_hdmi_feed_steps(const aga_step_t *s, int n, uint64_t now, uint32_t cck_hz)
{
    struct feed f;
    if (!feed_begin(&f)) { blep_live = 0; return; }
    if (!blep_inited) { blep_init(&blep, f.ri, now, 0); blep_inited = 1; blep_vseen = ~0u; }
    /* HDMI uzoraka po colour clocku, 32.32: manje dok je lead dug */
    uint64_t rnom = ((uint64_t)rate_hz[f.ri] << 32) / cck_hz;
    uint64_t r = rnom - (uint64_t)(((int64_t)rnom * f.ppm) / 1000000);
    /* Iznova nakon resynca (tišina prije wpos), na novom rateu, ili kad je
       colour clock skočio: od kraja zadnjeg feeda kad se nastavlja na njega,
       inače od prvog koraka ili sada. Inače dalje od kraja zadnjeg feeda, na
       novom trimu. */
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
    uint32_t vq = __atomic_load_n(&vseq, __ATOMIC_ACQUIRE);
    if (vq != blep_vseen) {                              /* voice mixer, kakav je sada */
        int on;
        int32_t gl[4], gr[4], w[4][2];
        aga_hdmi_voice_gains(&on, gl, gr);
        for (int c = 0; c < 4; c++) { w[c][0] = gl[c]; w[c][1] = gr[c]; }
        if (blep_is_paula(w)) on = 0;                    /* Paulino vlastito ožičenje: obične sume, do bita */
        blep_weights(&blep, on, w);
        blep_vseen = vq;
    }
    for (int i = 0; i < n; i++) blep_step(&blep, s[i].t, s[i].ch, s[i].level);
    int32_t w16;
    int sw;
    aga_audio_width_swap(&w16, &sw);
    for (uint32_t k = blep_ready(&blep, now); k; k--) {
        int32_t l, r2;
        blep_out(&blep, &l, &r2);
        l *= 2;                                          /* dva kanala po strani, kao coreov mix: 32,512 pun */
        r2 *= 2;
        if (w16 != 0x10000 || sw) {                      /* stereo Sound prozora, kako to radi aga_audio_stream */
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

/* Jezgra 3, parkirana (sandbox dolje) dok je AHI ring uključen: feed s tihom
   Paulom, svakih 2 ms onoliko frameova koliko daje rate prave Paule, pa AHI
   stream svira i na običnom desktopu, kroz isti lead i rate control. Čekanje
   čita samo clock i dva flaga na vlastitim linijama (aga_glue.c park
   objašnjava zašto pollana linija vrijedi - da, pollana, i da, vrijedi).
   Natrag u park čim chipset zatreba ili ring isključi. */
void aga_hdmi_pump(void)
{
    static int16_t zeros[2 * 64];
    static uint64_t last, frac;
    uint64_t f = cntfrq(), now = cntvct();
    if (!last || now - last > f / 20) { last = now; frac = 0; }     /* (re)start nakon pauze */
    while (cntvct() - last < f / 500) {
        if (aga_enabled || !aga_hdmi_ahi_live()) return;
        aga_nap_before(last + f / 500); /* s aga.sleep: spava između 2 ms koraka (aga_glue.h) */
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

/* ---- housekeeper: tišina kad mix stane, i pogled jednom u sekundi - */

/* Dok svira, jednom u sekundi, iza guardova: je li DMA još moj i radi, je li
   MAI programiran (hotplug pušta firmware da postavi link iznova), je li moj
   infoframe poslan? MAI-jevi i DMA-jevi error flagovi se čiste, i novi HSM
   rate upisuje. S držanim reg_lockom. */
static void check(void)
{
    int c = dma_ch;
    uint32_t cb = rd(DMA_CB(c)), cs = rd(DMA_CS(c));
    if (cb != cb_bus) {
        /* Tuđi control block (68k driver uzeo je kanal nakon mene): njegov sada, ne
           resetira se pod njim (dma_lost). CONBLK_AD 0 i ne radi je samo idle
           kanal: netko ga je resetirao i nitko ga ne vrti - opet moj na mjestu,
           od početka ringa, kako je 1.112 restartao zaustavljen kanal. */
        if (cb || (cs & CS_ACTIVE) || !start_dma()) { dma_lost(c); return; }
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        aga_diag_log("[AGA] HDMI audio: its DMA channel was reset - started again");
        cs = rd(DMA_CS(c));
    }
    /* DMA greška (AXI read error, njegov FIFO) čisti se u DEBUG. Kanal koji još
       radi nastavlja gdje je; samo onaj koji je stao pokreće se ponovno, od
       početka ringa - skok, i jezgra 3 se resynca. */
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
        /* Uključen s vlastitim N: drugi driver resetirao je MAI i pokrenuo ga pod
           mojim. Njegov sada (state 8, kao pri startu): moj DMA staje, njegovi
           registri ostaju kako ih je taj driver postavio, i samo moj slot 7
           čeka Sound Off ili moj sljedeći start. Hotplug izgleda drugačije -
           firmware ostavlja MAI isključen, ili N 0. */
        set_state(H_BUSY_OTHER);
        dma_stop();
        mai_ours = 0;
        info_pending = 0;
        aga_diag_log("[AGA] HDMI audio: another driver took the MAI over - waiting for it to stop");
        return;
    }
    if (err) aga_diag_inc(AGA_D_HDMI_MAI_ERRS);          /* program_mai reset ih također čisti */
    if (n != our_n || !(ctl & M_ENABLE)) {
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        program_mai();
    } else {
        if (err)
            wr(hw->mai_ctl, M_RUN | err);                /* očišćeno, i još uključeno */
        if (smp_new) {
            aga_diag_inc(AGA_D_HDMI_FIXUPS);
            wr(hw->mai_smp, mai_smp);                    /* Pi 3: HSM ima novi rate */
            smp_new = 0;
        }
    }
    if (info_pending || !(rd(hw->pkt_cfg) & (1u << SLOT)) || !(rd(hw->pkt_stat) & (1u << SLOT))) {
        aga_diag_inc(AGA_D_HDMI_FIXUPS);
        infoframe();
    }
}

/* stanja koja želja za zvukom sama napušta, sekundu kasnije */
static inline int retryable(int s)
{
    return s == H_OFF || s == H_NO_LINK || s == H_SLOT_BUSY || (s == H_NO_DMA && dma_mask) ||
           s == H_BUSY_OTHER || (s == H_NO_CLOCK && pi3);
}

/* Tišina ide u komadu po posjetu, počinjući gdje bi DMA prvi susreo stari
   zvuk: ova jezgra uzorkuje i Amigine interrupt linije, a cijeli ring odjednom
   zadržao bi to. Komad je oko mikrosekunde; DMA je još 13 ms od starog zvuka
   kad fill počinje. Ring koji treba start na ovoj jezgri (prep_*) ide istim
   putem. */
static uint32_t fill_at, fill_left;
static uint32_t fill_seq;                /* feed_seq kad je fill počeo (parno: nijedan feed ne radi) */
static int start_soon;                   /* ring je spreman: pokušaj start pri sljedećem posjetu */

void aga_hdmi_service(void)
{
    static uint64_t info_next;
    int s = state;
    if (s != H_RUNNING && !ours() && !prep_left && !(want_now() && retryable(s))) return;
    uint64_t now = cntvct(), hz = cntfrq();
    scan_sample(now, hz);
    if (s == H_RUNNING && fill_left) {
        /* komad samo pod fill_busy, nakon fenced pogleda na feedov broj (vidi
           feed_seq) */
        __atomic_store_n(&fill_busy, 1, __ATOMIC_SEQ_CST);
        __asm__ volatile("dmb ish" ::: "memory");
        if (__atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST) != fill_seq) {
            fill_left = 0;                               /* mix se vratio: jezgra 3 se resynca */
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
        /* prvo broj, pa vrijeme: feed koji počne nakon ovog pogleda mijenja broj, a
           onaj koji sada radi čini ga neparnim. Clock se čita nakon oba, i
           uspoređuje signed: pročitan prije njih, feed koji je došao i otišao
           između ostavio je last_feed NAKON njega, i unsigned razlika
           wrapala se u "idle zauvijek" (otkrio review). */
        uint32_t seq = __atomic_load_n(&feed_seq, __ATOMIC_SEQ_CST);
        uint64_t seen = __atomic_load_n(&last_feed, __ATOMIC_ACQUIRE);
        uint64_t t = cntvct();
        if (!(seq & 1) && (int64_t)(t - seen) > (int64_t)(hz * idle_ms / 1000)) {
            fill_seq = seq;
            fill_at = sil_end;
            fill_left = ring_frames;
            /* idle od sada: sljedeći feed se resynca bez obzira na to je li fill stigao
               do kraja ili ga je prekinuo. Postavljeno samo na kraju, feed
               koji ga je prekinuo našao je writera 40 ms iza DMA i, na Pi 4,
               lead još unutar svojih granica - i odsvirao krug starog ringa
               (otkrio review).

               Zatim broj još jednom, oba sequentially consistent: feed koji je
               počeo nakon gornjeg pogleda ili vidi ovaj flag, ili je ovdje
               viđen. Viđen, sam je našao pauzu (svojim clockom, ili leadom
               koji je pauza stavila izvan raspona - vidi lead_high) i resyncao
               se; flag bi samo natjerao feed nakon njega da se resynca opet,
               nasred zvuka, pa se opet miče (otkrio review). */
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
        /* s lockom: jezgra 0 može ga u međuvremenu pokrenuti, i tada je ring njezin */
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
    if (!try_regs()) return;                             /* jezgra 0 ga prebacuje: sljedeći posjet */
    int g = guard_ok(now, hz);
    if (g < 0) { unlock_regs(); return; }                /* pogled čeka da se slika vidi kako se kreće */
    if (second) last_check = now;
    if (info) info_next = now + hz / 100;                /* pending infoframe: svakih 10 ms */
    start_soon = 0;
    s = state;
    if (s == H_RUNNING) {
        if (!g) {
            /* nema slike na HDMI-ju 200 ms, ili je Pi 3-jev HSM stao: nijedan HDMI
               registar ne smije se sada čitati. Moj DMA staje (njegovi
               registri su uvijek clockani); MAI čeka clock (mai_ours), a retry
               ga pokreće opet. */
            set_state(H_NO_LINK);
            dma_stop();
            aga_diag_log("[AGA] HDMI audio: the HDMI picture stopped - waiting for it");
        } else if (second && pi3 && !hsm_known) {
            /* Pi 3-jev HSM radi, ali više ne iz PLLD: njegov rate je nepoznat, pa
               MAI-jev djelitelj može biti pogrešan. Zaustavljen i vraćen
               (clock radi, pa smije biti), state 3, pokušava se opet. */
            set_state(H_NO_CLOCK);
            dma_stop();
            restore();
            aga_diag_log("[AGA] HDMI audio: the HSM left PLLD - waiting for it");
        } else if (second) {
            check();
        } else if (info_pending) {
            infoframe();
        }
    } else if (want_now() && retryable(s)) {
        if (!g) {
            if (s != H_NO_CLOCK) set_state(H_NO_LINK);
        } else if (!ring_silent) {
            prep_at = 0;                                 /* prvo tišina, pa start */
            prep_left = ring_frames;
        } else {
            start(0);
        }
    } else if (!want_now() && ours() && g) {
        restore();                                       /* što je izgubljeni clock ili drugi driver ostavio */
    }
    unlock_regs();
}
