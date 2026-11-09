/* AGA-STORM - prilagođeni crop (agaboot RTG Image: Size Custom, Crop...):
 * četiri ruba koje korisnik odreže s kadra, PAL-ovi i NTSC-ovi, i
 * upravljačke riječi koje ih nose. Čisti C za sebe, pa ga host testovi
 * provjeravaju (host/tests/test_crop.c); aga_hvs.c je jedini kernel
 * korisnik, i neka tako ostane.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_CROP_H
#define AGA_CROP_H

#include <stdint.h>

#define CROP_FRAME_W    800     /* AGA_OUT_W: hires pikseli od lores stupca $50 */
#define CROP_ROWS_PAL   286     /* AGA_OUT_H */
#define CROP_ROWS_NTSC  237     /* NTSC-ov kadar završava na liniji 262, red 236 (aga_hvs.c) */
#define CROP_MIN_W      320     /* što barem ostaje, hires piksela... */
#define CROP_MIN_H      100     /* ...i linija */
enum { CUT_L, CUT_T, CUT_R, CUT_B };

/* Dok korisnik ne odreže svoje: rubovi 720 x 272, veličina koju TV pokazuje
   (aga_hvs.c zoom_rect: x 74, y 10 u oba standarda). Poslije se sam snađe. */
#define CROP_DEFAULT_PAL  { 74, 10, 6, 4 }
#define CROP_DEFAULT_NTSC { 74, 10, 6, 0 }

/* Pravokutnik koji rubovi ostavljaju: x, y, w, h u kadru. Lijevi i desni u
   koracima od dva hires piksela (jedan lores piksel), što drži pokazivač
   ravnine na osam bajtova; rubovi koji bi ostavili manje od minimuma
   popuštaju - desni lijevome, donji gornjemu. Red mora biti. */
static inline void crop_cut_rect(const uint16_t *cut, int ntsc, int *x, int *y, int *w, int *h)
{
    int rows = ntsc ? CROP_ROWS_NTSC : CROP_ROWS_PAL;
    int l = cut[CUT_L] & ~1, t = cut[CUT_T], r = cut[CUT_R] & ~1, b = cut[CUT_B];
    if (l > CROP_FRAME_W - CROP_MIN_W) l = CROP_FRAME_W - CROP_MIN_W;
    if (r > CROP_FRAME_W - CROP_MIN_W - l) r = CROP_FRAME_W - CROP_MIN_W - l;
    if (t > rows - CROP_MIN_H) t = rows - CROP_MIN_H;
    if (b > rows - CROP_MIN_H - t) b = rows - CROP_MIN_H - t;
    *x = l;
    *y = t;
    *w = CROP_FRAME_W - l - r;
    *h = rows - t - b;
}

/* Size Auto (1.114, RGB2RTG's Crop: Auto): igrin vlastiti prozor prikaza,
   kako ga je jezgra izmjerila na linijama sa slikom (aga_last_frame_window:
   hires pikseli [x0, x1), linije y0..y1, u kadru), praćen dok igra radi.
   Veći se prozor uzima odmah, manji tek kad cijeli period od
   CROP_AUTO_QUIET kadrova nije iskoristio ništa veće - 2 do 4 s nakon
   zadnjeg većeg kadra, ne računajući period u kojem je taj kadar bio - da
   izbornik ili fade ne pumpaju sliku. Lijevo i desno cijeli lores pikseli;
   prozor manji od 160 x 64 ne uzima se za sliku. */
#define CROP_AUTO_QUIET 100     /* 2 s pri 50 Hz */
struct crop_auto {
    int x0, x1, y0, y1;         /* prikazano; x0 < 0: još nijedan */
    int cx0, cx1, cy0, cy1;     /* što su kadrovi od zadnje promjene iskoristili */
    int quiet;
};
static inline void crop_auto_reset(struct crop_auto *s) { s->x0 = -1; s->quiet = 0; }

/* prozor jednog kadra (ok 0: nijedan ovaj kadar); 1 kad se prikazani promijenio */
static inline int crop_auto_track(struct crop_auto *s, int rows, int ok, int x0, int x1, int y0, int y1)
{
    if (!ok) return 0;
    if (x0 < 0) x0 = 0;
    if (x1 > CROP_FRAME_W) x1 = CROP_FRAME_W;
    if (y0 < 0) y0 = 0;
    if (y1 > rows - 1) y1 = rows - 1;
    x0 &= ~1;
    x1 = (x1 + 1) & ~1;
    if (x1 - x0 < 160 || y1 - y0 + 1 < 64) return 0;
    if (s->x0 < 0 || x0 < s->x0 || x1 > s->x1 || y0 < s->y0 || y1 > s->y1) {
        if (s->x0 < 0) {
            s->x0 = x0; s->x1 = x1; s->y0 = y0; s->y1 = y1;
        } else {                                /* oboje zajedno: ništa prikazano se ne gubi */
            if (x0 < s->x0) s->x0 = x0;
            if (x1 > s->x1) s->x1 = x1;
            if (y0 < s->y0) s->y0 = y0;
            if (y1 > s->y1) s->y1 = y1;
        }
        s->cx0 = x0; s->cx1 = x1; s->cy0 = y0; s->cy1 = y1;
        s->quiet = 0;
        return 1;
    }
    if (x0 < s->cx0) s->cx0 = x0;
    if (x1 > s->cx1) s->cx1 = x1;
    if (y0 < s->cy0) s->cy0 = y0;
    if (y1 > s->cy1) s->cy1 = y1;
    if (++s->quiet < CROP_AUTO_QUIET) return 0;
    int moved = s->cx0 != s->x0 || s->cx1 != s->x1 || s->cy0 != s->y0 || s->cy1 != s->y1;
    s->x0 = s->cx0; s->x1 = s->cx1; s->y0 = s->cy0; s->y1 = s->cy1;
    s->cx0 = x0; s->cx1 = x1; s->cy0 = y0; s->cy1 = y1;
    s->quiet = 0;
    return moved;
}

/* Što Auto prikazuje poprečno: prozor - ili, kad je igrino polje igre uže
   od standardnog ekrana, 640 hires piksela oko njega: sam standardni ekran
   kad prozor leži unutar njega, inače centrirano na prozor, unutar kadra.
   Prozor je bitplanov (DIW na linijama s podacima), pa ne vidi spriteove u
   rubu: Bansheejevo polje igre je 256 lores piksela sa 256 linija
   (242..754 u kadru), a njegova ploča rezultata, života i štita su border
   spriteovi (BRDSPRT) lijevo od njega, puneći standardni ekran do 114.
   Uzet doslovno, prozor je izgubio ploču, a Fill je rastegnuo ostalo 1.7
   puta gdje standardna igra dobiva 1.33; centriran, ploča je bila odrezana
   i desno se vidio border. Samo širina: prozor niži od standardnog ekrana
   je ono za što Auto postoji, letterbox koji zumira van. Koraci od dva
   hires piksela. */
#define CROP_AUTO_MIN_W 640     /* standardni ekran (aga_hvs.c zoom_rect[2]) */
#define CROP_STD_X      114     /* ...i gdje počinje u kadru: DIWSTRT $2C81 */
static inline void crop_auto_span(int x0, int x1, int *x, int *w)
{
    int cx = x0, cw = x1 - x0;
    if (cw < CROP_AUTO_MIN_W) {
        if (x0 >= CROP_STD_X && x1 <= CROP_STD_X + CROP_AUTO_MIN_W)
            cx = CROP_STD_X;
        else {
            cx = x0 - (CROP_AUTO_MIN_W - cw) / 2;
            if (cx > CROP_FRAME_W - CROP_AUTO_MIN_W) cx = CROP_FRAME_W - CROP_AUTO_MIN_W;
            if (cx < 0) cx = 0;
            cx &= ~1;
        }
        cw = CROP_AUTO_MIN_W;
    }
    *x = cx;
    *w = cw;
}

/* Riječi na $DFF1F2. Bitovi 1 i 15 ostaju čisti u svakoj: kernel bez njih
   sprema riječ kao jezgrin aga_ctrl, gdje ta dva znače "windowed" i "use the
   register" (vectors.c) - ne pitaj zašto baš ta dva, tako je oduvijek.
     $58C0 + E5(d)   još pet bitova vrijednosti: latch = latch << 5 | d
     $5870 + E3(s)   vrijednost u slot s = rub + 4 x NTSC, latch obrisan
     $5880 + E3(s)   crop ekran (samo desktop): standard s >> 2, rub s & 3
                     označen; $5930 ga završava, kao što završava Position
   Vrijednost su dvije data riječi, deset bitova: rub je najviše 480. */
#define CROP_E3(s)  ((uint16_t)(((s) & 1) | (((s) & 6) << 1)))
#define CROP_E5(d)  ((uint16_t)(((d) & 1) | (((d) & 0x1E) << 1)))
#define CROP_D3(v)  ((int)(((v) & 1) | (((v) >> 1) & 6)))
#define CROP_D5(v)  ((int)(((v) & 1) | (((v) >> 1) & 0x1E)))
#define CROP_W_DATA   0x58C0
#define CROP_W_STORE  0x5870
#define CROP_W_SCREEN 0x5880

#endif
