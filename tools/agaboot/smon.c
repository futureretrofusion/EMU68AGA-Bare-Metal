/*
 * AGA-SoundMonitor - AGA-PISTORM's Sound monitor as a program of its own: an
 * icon in SYS:Utilities, and a name a Shell knows, for what is otherwise the
 * Chipset menu's Sound - Monitor... (users asked where the monitor was on
 * the disk, 2026-10-18). Start it from its icon, a Dock, ToolsDaemon or
 * WBStartup.
 *
 * It starts `C:agaboot SOUND MONITOR` as a process of its own and is done:
 * the monitor lives in agaboot (monitor.c), one copy of it, and a second
 * start brings the window already open to the front (front.c).
 *
 * Copyright (C) 2026 Astair86. GPL-2.0-only, as the rest of AGA-PISTORM.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

static const char version[] __attribute__((used)) = "$VER: AGA-SoundMonitor 1.118 (18.10.2026) AGA-PISTORM";

struct IntuitionBase *IntuitionBase;

int main(void)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (in && out && SystemTags((STRPTR)"C:agaboot SOUND MONITOR", SYS_Input, in, SYS_Output, out,
                                SYS_Asynch, TRUE, NP_StackSize, 32768, TAG_DONE) != -1)
        return 0;                                      /* started: the handles are its now */
    if (in) Close(in);
    if (out) Close(out);
    /* from an icon there is no Shell to say it in */
    if ((IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37))) {
        struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-SoundMonitor",
            (UBYTE *)"C:agaboot could not be started.\n\nIs AGA-PISTORM installed?", (UBYTE *)"OK" };
        EasyRequestArgs(NULL, &es, NULL, NULL);
        CloseLibrary((struct Library *)IntuitionBase);
    }
    return 20;
}
