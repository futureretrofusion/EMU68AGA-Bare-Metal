/* AGA-STORM - CRT izgled na 3D GPU Pi 4, V3D 4.2 (1.119):
 * zakrivljeni zaslon, Glow, Afterglow i Frame blend koje crtaju GPU programi
 * između kadra jezgre i HDMI-ja. RGB2RTG-ov (0.78-dev2 do dev30,
 * kernel/rgb_v3d.h, docs/V3D-SHADERS.md, Astair86, 2026), prvi compute
 * poslovi koje je itko pokrenuo na V3D-u Pi 4 bez Linuxa - i gle, rade.
 * Njegovi programi za moje 32-bitne piksele u aga_v3d_code.h
 * (tools/v3d/mkcrt.py).
 *
 * Passovi, svaki compute posao (CSD), H međuspremnik između njih:
 *   G  samo Glow: piksel glow međuspremnika za svakih 8 x 4 izvornih piksela
 *   A  work-group po retku cropa: redak skaliran po širini slike (Smooth
 *      ili Sharp), na zakrivljenom zaslonu kroz barrel, pomiješan s
 *      prethodnim kadrom (Frame blend), dodan glow
 *   C  samo Afterglow: svaki piksel barem kao u prethodnom kadru, izblijedio
 *   B  work-group po retku slike: dva retka H pomiješana s težinama
 *      scanlinea, puta stupac CRT maske - na zakrivljenom zaslonu redak
 *      nađen kroz barrel, scanlinei iz zrake
 * u jednu od dvije slike u GPU prostoru (AGA_GPU_RESERVE), koju aga_hvs.c
 * prikazuje kao 1:1 ravninu.
 *
 * Što je moje: jezgra 3 sama vrti chipset i nikad ne smije čekati 5 do 9 ms
 * koliko passovi traju - RGB2RTG-ova jezgra 3 samo konvertira kadrove i
 * čeka, lijepo se njima. Ovdje aga_v3d_start šalje prvi pass, a aga_v3d_poll,
 * pozvan iz line hooka i pri svakom kadru, šalje sljedeći kad je jedan
 * gotov: nekoliko pristupa registrima svaki put. Buđenje V3D-a također ništa
 * ne čeka: budi se na jezgri 0 prvi put kad ga neka postavka zatraži, nikad
 * pri podizanju - podizanje je i bez toga dovoljno bolno.
 *
 * Bring-up, po Linuxovim bcm2835-power.c i v3d driveru (GPL-2.0+), kako to
 * radi RGB2RTG: V3D-ov reset otpušten u power bloku (PM_GRAFX bit 6, s
 * lozinkom bloka), njegova dva bus mosta otvorena master prije slavea;
 * prije posla L2T ispran i slice cachevi invalidirani, nakon njega TMU write
 * combiner ispran i L2T očišćen; GPU-ov MMU isključen, fizičke adrese.
 * Njegov takt je firmwareov (500 MHz). ARM radi big-endian, GPU čita
 * little-endian: svaka riječ programa, uniforma i tablica koju čita je
 * byte-swapped (lekcija RGB2RTG-a iz dev18: ARM i GPU, jebote, ne govore
 * isti jezik).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdint.h>
#include "aga_glue.h"
#include "aga_v3d.h"
#include "aga_v3d_code.h"

extern uintptr_t mmu_virt2phys(uintptr_t virt);
extern void arm_flush_cache(uintptr_t addr, uint32_t len);
extern void *tlsf;
extern void *tlsf_malloc(void *handle, uintptr_t size);

#define V3D_PERI        0xF2000000UL                    /* ARM_PERIIOBASE: fizički 0xFE000000 */
#define V3D_HUB         (V3D_PERI + 0xC00000)
#define V3D_CORE0       (V3D_PERI + 0xC04000)
#define V3D_ASB         (V3D_PERI + 0xC11000)           /* V3D-ovi bus mostovi na BCM2711 */
#define PM_GRAFX        (V3D_PERI + 0x10010C)
#define PM_PASSWORD     0x5A000000u
#define PM_V3DRSTN      (1u << 6)
#define ASB_S_CTRL      0x08
#define ASB_M_CTRL      0x0C
#define ASB_BRDG_ID     0x20                            /* čita "brdg" */
#define ASB_REQ_STOP    (1u << 0)
#define ASB_ACK         (1u << 1)
#define V3D_CTL_IDENT0  0x00                            /* "V3D" i njegova verzija: 0x04443356 */
#define V3D_CTL_SLCACTL     0x24
#define V3D_CTL_L2TCACTL    0x30
#define V3D_CTL_L2TFLSTA    0x34
#define V3D_CTL_L2TFLEND    0x38
#define V3D_CSD_STATUS      0x900                       /* dovršeni dispatchevi 11:4 */
#define V3D_CSD_CFG(i)      (0x904 + 4 * (i))           /* CFG0, zapisan zadnji, pokreće ga */
/* 16 work-grupa po supergrupi, 16 batcheva, 16 invokacija po work-grupi, kako
   to bira Mesa (RGB2RTG dev11: jedan po supergrupi strpao je svaki batch na
   jedan QPU - i gle, radi) */
#define V3D_ROW_CFG3    (15u << 12 | 0u << 8 | 16)

static inline uint32_t v3d_rd(uintptr_t a) { return __builtin_bswap32(*(volatile uint32_t *)a); }
static inline void v3d_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint64_t v3d_now(void) { uint64_t t; __asm__ volatile("isb; mrs %0, CNTPCT_EL0" : "=r"(t)); return t; }
static inline uint64_t v3d_freq(void) { uint64_t f; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f)); return f; }

/* ---- buđenje -------------------------------------------------------------- */
enum { V3D_TRIED = 1, V3D_BRIDGE = 2, V3D_RELEASED = 4, V3D_NO_ACK = 8, V3D_ANSWERS = 16, V3D_MEMORY = 32 };
static volatile int v3d_state;

static int bridge_open(uint32_t reg)
{
    uint32_t v = v3d_rd(V3D_ASB + reg);
    if (v & ASB_REQ_STOP) v3d_wr(V3D_ASB + reg, PM_PASSWORD | (v & 0x00ffffffu & ~ASB_REQ_STOP));
    for (int i = 0; i < 100000; i++)
        if (!(v3d_rd(V3D_ASB + reg) & ASB_ACK)) return 1;
    return 0;
}

/* prostori programa (svaki sa svojim uniformama na fiksnom mjestu nakon
   njega), H međuspremnici, glow međuspremnik: kernelov heap, ispod 4 GB,
   samo GPU-ov - jednom očišćen iz ARM-ovih cacheva, da se nijedna linija ne
   upiše natrag preko njih. Cache i GPU ne dijele ništa osim nevolje. */
#define UNIF_FLAT   1024                /* kod do 1 KB: ravni passovi */
#define UNIF_CURVE  2048                /* zakrivljeni i blendajući */
#define UNIF_FX     3072                /* Glowov pass A */
_Static_assert(V3D_LINES_CODE_N * 8 <= UNIF_FLAT && V3D_SCREEN_CODE_N * 8 <= UNIF_FLAT &&
               V3D_AFTERGLOW_CODE_N * 8 <= UNIF_FLAT, "a flat V3D program runs into its uniforms");
_Static_assert(V3D_LINES_CURVE_CODE_N * 8 <= UNIF_CURVE && V3D_SCREEN_CURVE_CODE_N * 8 <= UNIF_CURVE &&
               V3D_LINES_BLEND_CODE_N * 8 <= UNIF_CURVE && V3D_LINES_CURVE_BLEND_CODE_N * 8 <= UNIF_CURVE &&
               V3D_GLOW_CODE_N * 8 <= UNIF_CURVE, "a curved or blending V3D program runs into its uniforms");
_Static_assert(V3D_LINES_GLOW_CODE_N * 8 <= UNIF_FX && V3D_LINES_CURVE_GLOW_CODE_N * 8 <= UNIF_FX &&
               V3D_LINES_BLEND_GLOW_CODE_N * 8 <= UNIF_FX && V3D_LINES_CURVE_BLEND_GLOW_CODE_N * 8 <= UNIF_FX,
               "a Glow V3D program runs into its uniforms");
#define H_PITCH     (1920u * 4)         /* najšira slika koja se crta */
#define H_ROWS      288u                /* najviše redaka cropa: 286 običnog kadra */
#define GLOW_COLS   128u                /* 800 / 8 */
#define GLOW_ROWS   72u                 /* 286 / 4 */
#define GLOW_PITCH  (GLOW_COLS * 4)

/* [curved][blend][glow] za pass A, [curved] za pass B */
static uint8_t *pass_a[2][2][2], *pass_b[2], *pass_g, *pass_c;
static uint8_t *hbuf[2], *gbuf;

static uint8_t *heap(uint32_t bytes)
{
    uint8_t *m = tlsf_malloc(tlsf, bytes + 64);
    if (!m || (mmu_virt2phys((uintptr_t)m) >> 32)) return 0;
    m = (uint8_t *)(((uintptr_t)m + 63) & ~(uintptr_t)63);
    arm_flush_cache((uintptr_t)m, bytes);
    return m;
}

static uint8_t *prog(const uint64_t *code, unsigned n, unsigned room)
{
    uint8_t *a = heap(room + 256);
    if (!a) return 0;
    uint64_t *c = (uint64_t *)a;
    for (unsigned i = 0; i < n; i++) c[i] = __builtin_bswap64(code[i]);
    arm_flush_cache((uintptr_t)a, n * 8);
    return a;
}

static uintptr_t gpu_va, gpu_pa;        /* GPU prostor: dvije slike */
static uint32_t gpu_bytes;

void aga_v3d_room(uint8_t *va, uintptr_t pa, uint32_t bytes)
{
    gpu_va = (uintptr_t)va;
    gpu_pa = pa;
    gpu_bytes = bytes;
}

/* jezgra 0, prvi put kad neka postavka zatraži GPU: probuđen, njegovi
   programi i međuspremnici izrađeni. 1 kad može crtati; 0 kad ne - i tada
   ga se više ne pita, dovraga. */
static int v3d_wake(void)
{
    if (v3d_state & V3D_MEMORY) return 1;
    if (v3d_state & V3D_TRIED) return 0;            /* prije pokušano i nije uspjelo - nema reprize */
    v3d_state = V3D_TRIED;
    if (v3d_freq() <= 20000000 || !gpu_bytes) return 0;    /* Pi 3 (19.2 MHz timer), ili nema prostora - kraj priče */
    if (v3d_rd(V3D_ASB + ASB_BRDG_ID) != 0x62726467u) return 0;
    v3d_state |= V3D_BRIDGE;
    uint32_t g = v3d_rd(PM_GRAFX);
    if (!(g & PM_V3DRSTN)) {
        v3d_wr(PM_GRAFX, PM_PASSWORD | (g & 0x00ffffffu) | PM_V3DRSTN);
        v3d_state |= V3D_RELEASED;
        for (int i = 0; i < 1000; i++) (void)v3d_rd(PM_GRAFX);     /* nekoliko taktova za reset */
    }
    if (!bridge_open(ASB_M_CTRL) || !bridge_open(ASB_S_CTRL)) {
        v3d_state |= V3D_NO_ACK;
        aga_diag_log("[AGA] GPU: the V3D's bridges did not open - the CRT look stays the display's own");
        return 0;
    }
    if (v3d_rd(V3D_CORE0 + V3D_CTL_IDENT0) != 0x04443356u) {
        aga_diag_log("[AGA] GPU: no V3D 4.2 answers - the CRT look stays the display's own");
        return 0;
    }
    v3d_state |= V3D_ANSWERS;
    static const uint64_t *const ac[2][2][2] = {
        { { v3d_lines_code, v3d_lines_glow_code }, { v3d_lines_blend_code, v3d_lines_blend_glow_code } },
        { { v3d_lines_curve_code, v3d_lines_curve_glow_code },
          { v3d_lines_curve_blend_code, v3d_lines_curve_blend_glow_code } } };
    static const unsigned an[2][2][2] = {
        { { V3D_LINES_CODE_N, V3D_LINES_GLOW_CODE_N }, { V3D_LINES_BLEND_CODE_N, V3D_LINES_BLEND_GLOW_CODE_N } },
        { { V3D_LINES_CURVE_CODE_N, V3D_LINES_CURVE_GLOW_CODE_N },
          { V3D_LINES_CURVE_BLEND_CODE_N, V3D_LINES_CURVE_BLEND_GLOW_CODE_N } } };
    int ok = 1;
    for (int c = 0; c < 2; c++)
        for (int b = 0; b < 2; b++)
            for (int gl = 0; gl < 2; gl++) {
                unsigned room = gl ? UNIF_FX : (c || b) ? UNIF_CURVE : UNIF_FLAT;
                ok &= (pass_a[c][b][gl] = prog(ac[c][b][gl], an[c][b][gl], room)) != 0;
            }
    ok &= (pass_b[0] = prog(v3d_screen_code, V3D_SCREEN_CODE_N, UNIF_FLAT)) != 0;
    ok &= (pass_b[1] = prog(v3d_screen_curve_code, V3D_SCREEN_CURVE_CODE_N, UNIF_CURVE)) != 0;
    ok &= (pass_g = prog(v3d_glow_code, V3D_GLOW_CODE_N, UNIF_CURVE)) != 0;
    ok &= (pass_c = prog(v3d_afterglow_code, V3D_AFTERGLOW_CODE_N, UNIF_FLAT)) != 0;
    ok &= (hbuf[0] = heap(H_PITCH * H_ROWS)) != 0;
    ok &= (hbuf[1] = heap(H_PITCH * H_ROWS)) != 0;
    ok &= (gbuf = heap(GLOW_PITCH * GLOW_ROWS)) != 0;
    if (!ok) {
        aga_diag_log("[AGA] GPU: no room on the kernel's heap - the CRT look stays the display's own");
        return 0;
    }
    v3d_state |= V3D_MEMORY;
    aga_diag_log("[AGA] GPU: V3D 4.2 woken, programs ready");
    return 1;
}

int aga_v3d_ready(void) { return (v3d_state & V3D_MEMORY) != 0; }

/* ---- postavke (jezgra 0, iz upravljačkih riječi aga_hvs.c) ------------------ */
static volatile int set_curve, set_glow, set_after;
static const uint16_t after_decay[4] = { 0, 90, 141, 192 };

int aga_v3d_set(int what, int v)
{
    if (what == 0) set_curve = v < 0 ? 0 : v > 9 ? 9 : v;
    else if (what == 1) set_glow = v < 0 ? 0 : v > 100 ? 100 : v;
    else set_after = v < 0 ? 0 : v > 3 ? 3 : v;
    if (set_curve || set_glow || set_after) v3d_wake();
    return aga_v3d_wanted();
}

int aga_v3d_wanted(void)
{
    return (set_curve || set_glow || set_after) && aga_v3d_ready();
}

/* ---- tablice koje čita pass B (jezgra 3) ---------------------------------------
   rowB, dva retka H jednog HDMI retka i njihove težine s faktorom scanlinea
   tog retka u njima, L | dl << 10 | wa << 12 | wb << 21; colt, faktor CRT
   maske jednog stupca - oba 0..256 zadržanih piksela, dok HVS miješa crno
   preko njega pri alphi ravnina: (255 - alpha) / 255. U GPU-ovu byte redu,
   jer bi bilo previše očekivati da red bude isti. */
#define CRT_ROWS 1600
#define CRT_COLS 2560
static uint32_t crt_rowb[CRT_ROWS] __attribute__((aligned(64)));
static uint32_t crt_colt[CRT_COLS] __attribute__((aligned(64)));
static uint32_t tab_ow, tab_oh;

/* dva izvorna piksela izlaznog piksela i od njih n pri koraku step (16.16):
   indeks prvog u *x0, težina drugog 0..256 - Smooth bilinearno u središtu
   piksela, Sharp samo gdje ga rub između dva prelazi */
static uint32_t crt_tap(uint32_t i, uint32_t step, uint32_t n, int look, uint32_t *x0)
{
    int64_t c = (int64_t)(2 * i + 1) * step / 2 - 32768;       /* (i + 0.5) x step - 0.5 */
    if (c < 0) c = 0;
    uint32_t x = (uint32_t)(c >> 16), f = (uint32_t)(c & 0xffff) >> 8;
    if (n < 2 || x >= n - 1) { *x0 = n ? n - 1 : 0; return 0; }
    *x0 = x;
    if (look == 1 || step >= 0x10000) return f;
    int32_t t = (((int32_t)f - 128) * 65536) / (int32_t)step + 128;
    return t < 0 ? 0 : t > 256 ? 256 : (uint32_t)t;
}

static inline uint32_t crt_factor(uint32_t alpha) { return ((255 - (alpha & 0xff)) * 256 + 127) / 255; }

int aga_v3d_tables(const uint8_t *row_dark, int flip, const uint8_t *col_dark, uint32_t ow, uint32_t oh,
                   uint32_t sh, uint32_t vstep, int look)
{
    if (ow > CRT_COLS || oh > CRT_ROWS || !sh) return 0;
    for (uint32_t y = 0; y < oh; y++) {
        uint32_t y0, f = crt_tap(y, vstep, sh, look, &y0);
        uint32_t rf = row_dark ? crt_factor(row_dark[flip ? oh - 1 - y : y]) : 256;
        uint32_t wa = ((256 - f) * rf + 128) >> 8, wb = (f * rf + 128) >> 8;
        crt_rowb[y] = __builtin_bswap32(y0 | (y0 + 1 < sh ? 1u : 0u) << 10 | wa << 12 | wb << 21);
    }
    for (uint32_t x = 0; x < ow; x++) crt_colt[x] = __builtin_bswap32(col_dark ? crt_factor(col_dark[x]) : 256);
    arm_flush_cache((uintptr_t)crt_rowb, oh * 4);
    arm_flush_cache((uintptr_t)crt_colt, ow * 4);
    tab_ow = ow;
    tab_oh = oh;
    return 1;
}

/* ---- crtanje: njegovi passovi redom -------------------------------------------- */
static inline uint32_t f32_bits(float f) { union { float f; uint32_t u; } c = { .f = f }; return c.u; }
static inline uint32_t phys(const void *p) { return (uint32_t)mmu_virt2phys((uintptr_t)p); }
static inline uint32_t gpu_pitch(uint32_t w) { return (w + 15) / 16 * 64; }      /* blokovi od 16 piksela */

uint32_t aga_v3d_slot(int s, uint32_t *pitch, uint32_t ow)
{
    if (pitch) *pitch = gpu_pitch(ow);
    return (uint32_t)gpu_pa + (uint32_t)s * (gpu_bytes / 2);
}

struct pass { uint32_t code, unif, groups; };
static struct pass passes[4];
static int npass, cur;                  /* passovi crtanja, onaj koji se izvodi; -1 nijedan */
static volatile int busy;
static uint32_t n0, t_started;
static uint64_t t0, t_pass;
static uint32_t us_pass[4], us_draw, us_max, need_us;
static int ag_cur;                      /* H međuspremnik u koji piše pass A ovog crtanja (Afterglow: naizmjenično) */
static uint32_t ag_key;                 /* visina cropa zadnjeg crtanja | H pitch << 12, 0 nijedno */
static uint64_t ag_t;
static uint32_t drawn, late, skipped, blended;
static int draw_slot;

static void send(const struct pass *p)
{
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TFLSTA, 0);            /* cijeli L2T, svaki put */
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TFLEND, 0xffffffffu);
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TCACTL, 1);            /* L2T flush (GPU zastaje na njemu) */
    v3d_wr(V3D_CORE0 + V3D_CTL_SLCACTL, 0x0F0F0F0Fu);   /* slice cachevi: instrukcije, uniforme, TMU */
    n0 = (v3d_rd(V3D_CORE0 + V3D_CSD_STATUS) >> 4) & 0xff;
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(1), 1u << 16);       /* jedna work-grupa u Y */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(2), 1u << 16);       /* ... i Z */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(3), V3D_ROW_CFG3);
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(4), p->groups - 1);  /* batchevi - 1 */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(5), p->code);        /* jedna nit po QPU: do 57 registara */
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(6), p->unif);
    t_pass = v3d_now();
    v3d_wr(V3D_CORE0 + V3D_CSD_CFG(0), p->groups << 16);  /* work-grupe u X: počinje */
}

/* nakon passa: što je zapisao iz TMU-ova write combinera i L2T-a, za
   sljedeći pass i HVS - svaki po nekoliko mikrosekundi; 0 nije na vrijeme */
static int clean(uint64_t limit)
{
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TCACTL, 1u << 8);
    uint64_t t = v3d_now();
    while (v3d_rd(V3D_CORE0 + V3D_CTL_L2TCACTL) & (1u << 8)) if (v3d_now() - t > limit) return 0;
    v3d_wr(V3D_CORE0 + V3D_CTL_L2TCACTL, 2u << 1 | 1);
    while (v3d_rd(V3D_CORE0 + V3D_CTL_L2TCACTL) & 1) if (v3d_now() - t > limit) return 0;
    return 1;
}

static void put_unif(uint8_t *room, unsigned at, const uint32_t *u, int n)
{
    uint32_t *d = (uint32_t *)(room + at);
    for (int i = 0; i < n; i++) d[i] = __builtin_bswap32(u[i]);
    arm_flush_cache((uintptr_t)d, (uint32_t)n * 4);
}

int aga_v3d_start(const struct aga_v3d_frame *f, int slot)
{
    if (!aga_v3d_wanted()) return 0;
    if (busy) { skipped++; return 0; }
    uint32_t ow = f->ow, oh = f->oh;
    uint32_t nblk = (ow + 15) / 16, opitch = nblk * 64;
    if (ow != tab_ow || oh != tab_oh || ow < 16 || oh < 8 || f->sh > H_ROWS || f->sh < 2 || opitch > H_PITCH ||
        (uint64_t)opitch * oh > gpu_bytes / 2)
        return 0;
    int curve = set_curve, glow = set_glow > 0, after = set_after;
    int blend = f->prev != 0;
    uint64_t frq = v3d_freq(), now = v3d_now();
    uint32_t out = aga_v3d_slot(slot, 0, ow);
    /* Afterglow: dva H međuspremnika naizmjenično, prethodni kadar kad je
       zadnji nacrtan, iste visine cropa i pitcha, unutar 60 ms - stara slika
       nikad se ne vraća kao duh */
    uint32_t ag_k = (f->sh & 0xfff) | opitch << 12;
    int ag_prev = after && ag_key == ag_k && now - ag_t < frq * 60 / 1000;
    uint8_t *h = hbuf[after ? ag_cur : 0];
    uint32_t hb = phys(h);
    uint32_t k = (f->look == 1 || f->stepx >= 0x10000 || !f->stepx) ? 256 : (uint32_t)((65536ull << 8) / f->stepx);
    uint32_t swm1 = f->sw ? f->sw - 1 : 0, shm1 = f->sh - 1;
    npass = 0;
    if (glow) {                         /* pass G: work-grupa po retku glow međuspremnika */
        uint32_t gw = (f->sw + 7) / 8, gh = (f->sh + 3) / 4;
        if (gw > GLOW_COLS) gw = GLOW_COLS;
        if (gh > GLOW_ROWS) gh = GLOW_ROWS;
        const uint32_t ug[8] = { gh - 1, f->src, f->pitch, phys(gbuf), GLOW_PITCH, (gw + 15) / 16, swm1, shm1 };
        put_unif(pass_g, UNIF_CURVE, ug, 8);
        passes[npass++] = (struct pass){ phys(pass_g), phys(pass_g + UNIF_CURVE), gh };
    }
    {   /* pass A: shm1, src, spitch, hbuf, hpitch, nblk, zatim step ravnog
           passa, start, k, swm1 - ili k zakrivljenog, swm1 i njegovi floatovi -
           zatim dsrc Frame blenda, Glowovih sedam */
        uint32_t a[28];
        int n = 0;
        if (curve) {
            float kx = 0.02f * (float)curve, sw = (float)f->sw;
            const uint32_t c[14] = { shm1, f->src, f->pitch, hb, opitch, nblk, k, swm1,
                                     f32_bits(1.0f / (float)ow), f32_bits(sw * 65536.0f),
                                     f32_bits((0.5f * sw - 0.5f) * 65536.0f), f32_bits(1.0f / (float)f->sh),
                                     f32_bits(kx), f32_bits(1.0f / (1.0f + 0.25f * kx)) };
            for (int i = 0; i < 14; i++) a[n++] = c[i];
        } else {
            const uint32_t c[10] = { shm1, f->src, f->pitch, hb, opitch, nblk, f->stepx, (f->stepx >> 1) - 32768u, k,
                                     swm1 };
            for (int i = 0; i < 10; i++) a[n++] = c[i];
        }
        if (blend) a[n++] = f->prev - f->src;
        if (glow) {
            uint32_t gw = (f->sw + 7) / 8, gh = (f->sh + 3) / 4;
            if (gw > GLOW_COLS) gw = GLOW_COLS;
            if (gh > GLOW_ROWS) gh = GLOW_ROWS;
            const uint32_t c[7] = { phys(gbuf), GLOW_PITCH, gh - 1, (uint32_t)set_glow * 256 / 100, gw - 1,
                                    (uint32_t)-229376, 0x01000100u };
            for (int i = 0; i < 7; i++) a[n++] = c[i];
        }
        uint8_t *am = pass_a[curve ? 1 : 0][blend][glow];
        unsigned at = glow ? UNIF_FX : (curve || blend) ? UNIF_CURVE : UNIF_FLAT;
        put_unif(am, at, a, n);
        passes[npass++] = (struct pass){ phys(am), phys(am + at), f->sh };
    }
    if (ag_prev) {                      /* pass C: shm1, hcur, hprev, hpitch, nblk, decay */
        const uint32_t uc[6] = { shm1, hb, phys(hbuf[ag_cur ^ 1]), opitch, nblk, after_decay[after] };
        put_unif(pass_c, UNIF_FLAT, uc, 6);
        passes[npass++] = (struct pass){ phys(pass_c), phys(pass_c + UNIF_FLAT), f->sh };
    }
    if (curve) {
        /* pass B zakrivljeni: ohm1, hbuf, hpitch, out, opitch, nblk, colt, shm1,
           zatim 1/ow, 1/oh, ky, bsy, sh, sh / 2 - 1/2, kv (Sharp: reci po retku),
           jačina scanlinea 0..1 puta 16 */
        float ky = 0.03f * (float)curve, sh = (float)f->sh;
        float kv = f->look == 1 || oh <= f->sh ? 1.0f : (float)oh / sh;
        const uint32_t ub[16] = { oh - 1, hb, opitch, out, opitch, nblk, phys(crt_colt), shm1,
                                  f32_bits(1.0f / (float)ow), f32_bits(1.0f / (float)oh), f32_bits(ky),
                                  f32_bits(1.0f / (1.0f + 0.25f * ky)), f32_bits(sh), f32_bits(0.5f * sh - 0.5f),
                                  f32_bits(kv), f32_bits((float)(f->scan_pct > 100 ? 100 : f->scan_pct) * 0.16f) };
        put_unif(pass_b[1], UNIF_CURVE, ub, 16);
        passes[npass++] = (struct pass){ phys(pass_b[1]), phys(pass_b[1] + UNIF_CURVE), oh };
    } else {                            /* pass B: ohm1, hbuf, hpitch, out, opitch, nblk, rowB, colt */
        const uint32_t ub[8] = { oh - 1, hb, opitch, out, opitch, nblk, phys(crt_rowb), phys(crt_colt) };
        put_unif(pass_b[0], UNIF_FLAT, ub, 8);
        passes[npass++] = (struct pass){ phys(pass_b[0]), phys(pass_b[0] + UNIF_FLAT), oh };
    }
    if (after) {                        /* H ovog kadra kao prethodni za sljedeći */
        ag_key = ag_k;
        ag_t = now;
        ag_cur ^= 1;
    } else {
        ag_key = 0;
    }
    if (blend) blended++;
    __asm__ volatile("dsb sy" ::: "memory");
    draw_slot = slot;
    busy = 1;
    cur = 0;
    t0 = now;
    send(&passes[0]);
    return 1;
}

/* jezgra 3: crtanje na putu. 0 nema posla ili još traje, 1 slika je cijela
   u svom slotu (*slot), -1 neuspjeh: GPU se više ne koristi dok se postavka
   ne promijeni. Passu je dano 20 ms - i ni mikrosekundu više. */
int aga_v3d_poll(int *slot)
{
    if (!busy) return 0;
    uint64_t frq = v3d_freq(), now = v3d_now();
    if (((v3d_rd(V3D_CORE0 + V3D_CSD_STATUS) >> 4) & 0xff) == n0) {
        if (now - t_pass < frq / 50) return 0;
        late++;
        busy = 0;
        v3d_state &= ~V3D_MEMORY;       /* GPU možda još radi: isključen zauvijek do kraja ovog podizanja */
        v3d_state |= V3D_TRIED;
        aga_diag_log("[AGA] GPU: a pass took more than 20 ms - the CRT look is the display's own now");
        return -1;
    }
    us_pass[cur] = (uint32_t)((now - t_pass) * 1000000 / frq);
    if (!clean(frq / 1000)) {
        busy = 0;
        return -1;
    }
    if (++cur < npass) {
        send(&passes[cur]);
        return 0;
    }
    busy = 0;
    us_draw = (uint32_t)((v3d_now() - t0) * 1000000 / frq);
    /* crtanje dulje od 30 ms viđeno je samo kasno: kadrovi su načas stali
       (igra između svojih ekrana) i s njima pollovi - 80 ms pri Agonyjinu
       učitavanju na CM4. Nije GPU-ovo vrijeme, nego igrino - pa izostavljeno. */
    if (us_draw < 30000) {
        if (us_draw > us_max) us_max = us_draw;
        need_us = us_draw >= need_us ? us_draw : need_us - (need_us - us_draw) / 64;   /* odmah gore, šezdesetčetvrtina dolje */
    }
    if (++drawn % 1000 == 1) {
        static char line[120];
        char *p = line;
        const char *t = "[AGA] GPU: drawn ";
        while (*t) *p++ = *t++;
        uint32_t v[4] = { drawn, us_draw, us_max, skipped };
        const char *sep[4] = { ", last ", " us, worst ", " us, frames not drawn ", "" };
        for (int i = 0; i < 4; i++) {
            char d[12];
            int nd = 0;
            uint32_t x = v[i];
            do { d[nd++] = (char)('0' + x % 10); x /= 10; } while (x);
            while (nd) *p++ = d[--nd];
            for (t = sep[i]; *t; t++) *p++ = *t;
        }
        *p = 0;
        aga_diag_log(line);
    }
    *slot = draw_slot;
    return 1;
}

int aga_v3d_busy(void) { return busy; }
uint32_t aga_v3d_need_us(void) { return need_us; }
