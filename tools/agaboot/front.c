/* agaboot - one window of a kind. The Sound monitor, the Equalizer and the
 * voice mixer each open once: a public port named after the window holds its
 * Window, and a second open - from the Sound window, the monitor or a Shell -
 * brings that window to the front instead of opening another damn one. The
 * Sound window starts each of them as a process of its own (front_spawn), so
 * they all stay open together and the Sound window stays usable beside them
 * (2026-10-18: opened from the Sound window, one blocked the others, the
 * little bastards).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>
#include "front.h"

struct front *front_claim(const char *name, int *other)
{
    *other = 0;
    Forbid();
    struct front *o = (struct front *)FindPort((STRPTR)name);
    if (o) {
        if (o->win) { WindowToFront(o->win); ActivateWindow(o->win); }   /* queued: safe under Forbid */
        Permit();
        *other = 1;
        return NULL;
    }
    struct front *f = AllocVec(sizeof *f, MEMF_PUBLIC | MEMF_CLEAR);
    if (f) {
        strncpy(f->name, name, sizeof f->name - 1);
        f->mp.mp_Node.ln_Type = NT_MSGPORT;
        f->mp.mp_Node.ln_Name = f->name;
        f->mp.mp_Flags = PA_IGNORE;                    /* a name only: nothing is ever sent */
        f->mp.mp_MsgList.lh_Head = (struct Node *)&f->mp.mp_MsgList.lh_Tail;
        f->mp.mp_MsgList.lh_Tail = NULL;
        f->mp.mp_MsgList.lh_TailPred = (struct Node *)&f->mp.mp_MsgList.lh_Head;
        AddPort(&f->mp);
    }
    Permit();
    return f;
}

void front_set(struct front *f, struct Window *win)
{
    if (f) f->win = win;
}

void front_release(struct front *f)
{
    if (!f) return;
    Forbid();                                          /* no one brings a closing window forward */
    RemPort(&f->mp);
    f->win = NULL;
    Permit();
    FreeVec(f);
}

int front_spawn(const char *cmd)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out) {
        if (in) Close(in);
        if (out) Close(out);
        return 0;
    }
    if (SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_StackSize, 32768, TAG_DONE) == -1) {
        Close(in);                                     /* not started: the handles are still mine */
        Close(out);
        return 0;
    }
    return 1;
}
