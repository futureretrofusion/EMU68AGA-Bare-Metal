/* pointer.c - the mouse-pointer probe.
 *
 * After an AGA WHDLoad game Picasso96's software pointer stays frozen at the
 * centre of the RTG screen while the real one moves invisibly; after an ECS
 * game it is fine. Nothing on the Pi side told the two apart, so this is the
 * 68k side: a few lines per call with everything the pointer depends on that
 * is reachable from public structures, appended to the diagnostic log and
 * printed. This is what found the SetChipRev fault: after a ReqAGA slave
 * GfxBase->MemType read 3 and sprite 0's control words came out in the AA
 * layout, after the OCS slave of the same game neither did. Worth every line.
 *
 *   agaboot PROBE [tag]      the lines, now
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/sprite.h>
#include <graphics/view.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <hardware/intbits.h>
#include <hardware/custom.h>
#include <hardware/cia.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <string.h>
#include <stdio.h>

extern struct ExecBase *SysBase;
extern struct GfxBase *GfxBase;
extern struct IntuitionBase *IntuitionBase;

#define HW    ((volatile struct Custom *)0xDFF000)
#define CIAA_ ((volatile struct CIA *)0xBFE001)
#define CIAB_ ((volatile struct CIA *)0xBFD000)

/* the JMP target behind a library vector, or 0 if the slot is not a JMP */
static ULONG lvo_target(void *base, LONG lvo)
{
    UBYTE *p = (UBYTE *)base + lvo;
    if (*(UWORD *)p != 0x4EF9) return 0;
    return *(ULONG *)(p + 2);
}

/* Into the diagnostics' one fixed place, C:AGA-diag.log (agaboot.c DIAG_LOG),
   like the rest of the log. Up to 1.116's second tester build it tried
   Stuff: first: on a system without that volume - a plain AmigaOS 3.2 -
   Lock() put up "Please insert volume Stuff:" at every game's start and end
   while AGA_DIAGLOG was on, and with the game on screen that looked like a
   hang. */
static void log_line(const char *line)
{
    BPTR f = Open((STRPTR)"C:AGA-diag.log", MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    Write(f, (APTR)line, strlen(line));
    Close(f);
}

static void emit(const char *line)
{
    log_line(line);
    PutStr((STRPTR)line);
}

static void safe_name(char *dst, const char *s, int max)
{
    int k = 0;
    if (!s) s = "-";
    while (s[k] && k < max) { dst[k] = (s[k] >= 32 && s[k] < 127) ? s[k] : '?'; k++; }
    dst[k] = 0;
}

int ptr_probe(const char *tag)
{
    char line[400], n1[40], n2[40], n3[40];
    int own_gfx = 0, own_int = 0;
    if (!GfxBase) { GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39); own_gfx = 1; }
    if (!GfxBase) return 20;
    if (!IntuitionBase) { IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39); own_int = 1; }

    /* 1. graphics.library chipset and sprite fields */
    sprintf(line, "AGAPTR %s crb=%02lx dflags=%04lx sprw=%lu soft=%02lx defw=%lu smd=%02lx"
                  " want=%02lx bugs=%02lx bpl0=%04lx sres=%02lx bmem=%02lx mem=%02lx\n",
            tag,
            (ULONG)GfxBase->ChipRevBits0, (ULONG)GfxBase->DisplayFlags, (ULONG)GfxBase->SpriteWidth,
            (ULONG)(UBYTE)GfxBase->SoftSprites, (ULONG)GfxBase->DefaultSpriteWidth,
            (ULONG)(UBYTE)GfxBase->SprMoveDisable, (ULONG)GfxBase->WantChips, (ULONG)GfxBase->Bugs,
            (ULONG)(UWORD)GfxBase->system_bplcon0, (ULONG)GfxBase->SpriteReserved,
            (ULONG)GfxBase->BoardMemType, (ULONG)GfxBase->MemType);
    emit(line);

    /* 2. where the patched vectors point: Picasso96 SetFunction()s these */
    sprintf(line, "AGAPTR  lvo MoveSprite=%08lx LoadView=%08lx ChgExtSpr=%08lx GetExtSpr=%08lx"
                  " SetPointer=%08lx SetWinPtr=%08lx WaitTOF=%08lx\n",
            lvo_target(GfxBase, -426), lvo_target(GfxBase, -222), lvo_target(GfxBase, -1026),
            lvo_target(GfxBase, -930),
            IntuitionBase ? lvo_target(IntuitionBase, -270) : 0,
            IntuitionBase ? lvo_target(IntuitionBase, -816) : 0,
            lvo_target(GfxBase, -270));
    emit(line);

    /* 3. the pointer sprite as graphics.library knows it */
    {
        struct SimpleSprite *ss = GfxBase->SimpleSprites ? GfxBase->SimpleSprites[0] : NULL;
        if (ss && ss->posctldata) {
            struct ExtSprite *es = (struct ExtSprite *)ss;
            ULONG sum = 0;
            int h = ss->height, n = (h + 2) * 2;
            if (n > 512) n = 512;
            for (int i = 0; i < n; i++) sum += ss->posctldata[i];
            sprintf(line, "AGAPTR  spr0 ss=%08lx data=%08lx h=%d x=%d y=%d num=%d ww=%u ef=%04x"
                          " sum=%08lx w0-3=%04x %04x %04x %04x\n",
                    (ULONG)ss, (ULONG)ss->posctldata, h, ss->x, ss->y, ss->num,
                    es->es_wordwidth, es->es_flags, sum,
                    ss->posctldata[0], ss->posctldata[1], ss->posctldata[2], ss->posctldata[3]);
        } else {
            sprintf(line, "AGAPTR  spr0 ss=%08lx (no sprite 0 data)\n", (ULONG)ss);
        }
        emit(line);
    }

    /* 4. views, and Intuition's idea of the desktop */
    {
        struct View *v = GfxBase->ActiView;
        sprintf(line, "AGAPTR  view act=%08lx lof=%08lx modes=%04x copinit=%08lx",
                (ULONG)v, v ? (ULONG)v->LOFCprList : 0, v ? v->Modes : 0, (ULONG)GfxBase->copinit);
        if (IntuitionBase) {
            struct Window *w = IntuitionBase->ActiveWindow;
            struct Screen *s = IntuitionBase->ActiveScreen, *f = IntuitionBase->FirstScreen;
            safe_name(n1, w ? (char *)w->Title : NULL, 24);
            safe_name(n2, s ? (char *)s->Title : NULL, 24);
            safe_name(n3, f ? (char *)f->Title : NULL, 24);
            sprintf(line + strlen(line), " mouse=%d,%d win=\"%s\" scr=\"%s\" first=\"%s\"",
                    IntuitionBase->MouseX, IntuitionBase->MouseY, n1, n2, n3);
            if (w) sprintf(line + strlen(line), " wflags=%08lx", (ULONG)w->Flags);
        }
        strcat(line, "\n");
        emit(line);
    }

    /* 5. the real chips and the CIAs */
    sprintf(line, "AGAPTR  hw dmaconr=%04x intenar=%04x intreqr=%04x vposr=%04x deniseid=%04x"
                  " ciaa cra=%02x crb=%02x ta=%02x%02x tb=%02x%02x | ciab cra=%02x crb=%02x ta=%02x%02x tb=%02x%02x\n",
            HW->dmaconr, HW->intenar, HW->intreqr, HW->vposr, HW->deniseid,
            CIAA_->ciacra, CIAA_->ciacrb, CIAA_->ciatahi, CIAA_->ciatalo, CIAA_->ciatbhi, CIAA_->ciatblo,
            CIAB_->ciacra, CIAB_->ciacrb, CIAB_->ciatahi, CIAB_->ciatalo, CIAB_->ciatbhi, CIAB_->ciatblo);
    emit(line);

    /* 6. the VERTB server chain: Picasso96's soft sprite lives in there somewhere */
    {
        struct List *l = (struct List *)SysBase->IntVects[INTB_VERTB].iv_Data;
        char *names[24]; BYTE pris[24]; ULONG codes[24]; int n = 0;
        Disable();
        if (l) {
            for (struct Node *nd = l->lh_Head; nd->ln_Succ && n < 24; nd = nd->ln_Succ) {
                names[n] = nd->ln_Name; pris[n] = nd->ln_Pri;
                codes[n] = (ULONG)((struct Interrupt *)nd)->is_Code; n++;
            }
        }
        Enable();
        sprintf(line, "AGAPTR  vertb %d servers:", n);
        for (int i = 0; i < n; i++) {
            safe_name(n1, names[i], 30);
            sprintf(line + strlen(line), " [%d %s %08lx]", pris[i], n1, codes[i]);
            if (strlen(line) > 330) break;
        }
        strcat(line, "\n");
        emit(line);
    }

    if (own_int && IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    if (own_gfx) { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    return 0;
}
