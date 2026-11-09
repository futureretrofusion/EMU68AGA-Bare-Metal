/* AGA-STORM - CRT izgled na 3D GPU Pi 4 (aga_v3d.c). Tko voli ravne
 * piksele, ima PC; ovo je za ostale.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGA_V3D_H
#define AGA_V3D_H

#include <stdint.h>

/* start.c, pri podizanju: GPU prostor - dvije slike u koje crtaju passovi,
   identity-mapped i bez cachea - ili nijedna (Pi 3 nema V3D, jebote) */
void aga_v3d_room(uint8_t *va, uintptr_t pa, uint32_t bytes);

/* jezgra 0, upravljačke riječi: 0 Curved screen (0 ravno, 1-9), 1 Glow
   (0 isključeno, 1-100 %), 2 Afterglow (0 isključeno, 1-3 slabo .. jako).
   GPU se budi prvi put kad je nešto od toga uključeno; pri podizanju ne,
   jer nema smisla buditi zvijer za ravnu sliku. Vraća aga_v3d_wanted(). */
int  aga_v3d_set(int what, int v);
int  aga_v3d_wanted(void);              /* jedno je uključeno i GPU može crtati */
int  aga_v3d_ready(void);               /* GPU probuđen, njegovi programi izrađeni */

/* jezgra 3: tablice passa B za sliku ow x oh iz sh redaka pri vstep
   (16.16), look 0 Sharp 1 Smooth - tamnoća scanlinea svakog HDMI retka
   (0..255, NULL nijedna; flip: reci se čitaju odozdo prema gore) i tamnoća
   CRT maske svakog stupca (NULL nijedna). 1 izrađeno; 0 preveliko, i onda
   nema crtanja. */
int  aga_v3d_tables(const uint8_t *row_dark, int flip, const uint8_t *col_dark, uint32_t ow, uint32_t oh,
                    uint32_t sh, uint32_t vstep, int look);

/* kadar za crtanje: njegov crop na src (fizički; 32-bitne riječi jezgre),
   prethodni kadar na prev (isto mjesto u drugoj stranici, 0 nijedan:
   Frame blend), nacrtan ow x oh - veličina tablica */
struct aga_v3d_frame {
    uint32_t src, pitch, prev;
    uint32_t sw, sh;                    /* crop: pikseli, reci */
    uint32_t ow, oh;                    /* slika na HDMI */
    uint32_t stepx, stepy;              /* izvornih piksela po izlaznom, 16.16 */
    int look;                           /* 0 Sharp, 1 Smooth */
    int scan_pct;                       /* jačina scanlinea, zraka zakrivljenog zaslona */
};
int  aga_v3d_start(const struct aga_v3d_frame *f, int slot);   /* jezgra 3: 1 poslano */
int  aga_v3d_poll(int *slot);           /* jezgra 3: 1 slika je cijela u *slot, -1 neuspjeh, inače 0 */
int  aga_v3d_busy(void);
uint32_t aga_v3d_slot(int s, uint32_t *pitch, uint32_t ow);   /* fizička adresa slike, njezin red */
uint32_t aga_v3d_need_us(void);         /* vrijeme crtanja u posljednje vrijeme: odmah gore, polako dolje */

#endif
