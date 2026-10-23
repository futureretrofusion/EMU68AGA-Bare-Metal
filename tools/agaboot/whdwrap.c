/*
 * whdwrap - AGA-PISTORM's stand-in for WHDLoad, for the installer's AmiKit
 * choice (agaboot WHDWRAP puts it in WHDLoad's place, the real one renamed
 * WHDLoad.real next to it).
 *
 * The emulated AGA chipset is switched on for a game by `agaboot SANDBOX
 * AUTO`, normally from S:WHDLoad-Startup (WHDLoad's ExecuteStartup). WHDLoad
 * does not wait for that command - it runs it in the background - and checks
 * the slave's chipset at once. On AmiKit (WHDLoad 19.1, Kickstart 3.2, a
 * Pi 3) the command started seconds late, every time: WHDLoad had said "This
 * game requires AGA chipset" and run its cleanup before the sandbox came up,
 * and the sandbox then stayed on over the desktop (2026-10-16). Entered by
 * hand before WHDLoad started, the same game played.
 *
 * So this enters the sandbox first and waits for it, then runs the real
 * WHDLoad with exactly the same arguments - a Shell's argument line through
 * RunCommand, an icon's Workbench message as Workbench itself would hand it
 * over - and switches the sandbox off again when WHDLoad has finished, in
 * case its cleanup did not. agaboot SANDBOX AUTO judges the game from the
 * slaves in the current directory, so it runs in the game's drawer.
 *
 * Copyright (C) 2026 Astair86. GPL-2.0-only, as the rest of AGA-PISTORM.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <string.h>

/* agaboot WHDWRAP tells its own stand-in from WHDLoad by this */
static const char version[] __attribute__((used)) = "$VER: WHDLoad-AGA 1.116 (16.10.2026) AGA-PISTORM stand-in for WHDLoad";

extern struct WBStartup *_WBenchMsg;
struct Library *IconBase;

#define REAL     "PROGDIR:WHDLoad.real"
#define STACK    16384                        /* at least: WHDLoad's own advice is 10 KB and more */

/* WHDLoad.real must be the real WHDLoad: a stand-in there would start itself
   for ever (agaboot WHDWRAP never puts one there; this is the last guard) */
static int real_is_us(void)
{
    static const char mark[] = "AGA-PISTORM stand-in for WHDLoad";
    static char buf[4096];
    BPTR f = Open((STRPTR)REAL, MODE_OLDFILE);
    long got, keep = 0;
    int found = 0;
    if (!f) return 0;
    while (!found && (got = Read(f, buf + keep, sizeof buf - keep)) > 0) {
        long n = keep + got;
        for (long i = 0; i + (long)sizeof mark - 1 <= n && !found; i++)
            if (!memcmp(buf + i, mark, sizeof mark - 1)) found = 1;
        keep = n < (long)sizeof mark ? n : (long)sizeof mark;
        memmove(buf, buf + n - keep, keep);
    }
    Close(f);
    return found;
}

/* an agaboot command, with nothing to see and nothing to answer */
static void agaboot(const char *cmd)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (in && out) {
        SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, NP_StackSize, STACK, TAG_END);
        in = out = 0;                         /* SystemTags closed them */
    }
    if (in) Close(in);
    if (out) Close(out);
}

/* the sandbox is still on (agaboot keeps the old chip revision in this
   variable while it is): put it back */
static void sandbox_off(void)
{
    char v[16];
    if (GetVar((STRPTR)"AGA_CHIPREV", (STRPTR)v, sizeof v, GVF_GLOBAL_ONLY) > 0)
        agaboot("C:agaboot SANDBOX OFF");
}

/* A Shell's command line: the first word names the slave; when it carries
   a path, the game's drawer is the one that holds it */
static BPTR slave_dir(const char *a)
{
    static char w[256];
    int n = 0;
    while (*a == ' ' || *a == '\t') a++;
    if (!strnicmp(a, "SLAVE=", 6)) a += 6;
    if (*a == '"') { a++; while (*a && *a != '"' && n < 255) w[n++] = *a++; }
    else while (*a && *a != ' ' && *a != '\t' && *a != '\n' && n < 255) w[n++] = *a++;
    w[n] = 0;
    if (!strchr(w, ':') && !strchr(w, '/')) return 0;
    BPTR l = Lock((STRPTR)w, ACCESS_READ);
    if (!l) return 0;
    BPTR p = ParentDir(l);
    UnLock(l);
    return p;
}

static int from_shell(void)
{
    if (real_is_us()) { PutStr("WHDLoad: " REAL " is this stand-in - run the AGA-PISTORM installer again\n"); return 20; }
    BPTR seg = LoadSeg((STRPTR)REAL);
    if (!seg) {
        PutStr("WHDLoad: " REAL " is missing - run the AGA-PISTORM installer again\n");
        return 20;
    }
    STRPTR args = GetArgStr();
    BPTR dir = slave_dir((const char *)args), old = dir ? CurrentDir(dir) : 0;
    agaboot("C:agaboot SANDBOX AUTO");
    if (dir) UnLock(CurrentDir(old));
    struct Process *me = (struct Process *)FindTask(NULL);
    struct CommandLineInterface *cli = (struct CommandLineInterface *)BADDR(me->pr_CLI);
    LONG stack = cli ? cli->cli_DefaultStack * 4 : 0;
    if (stack < STACK) stack = STACK;
    LONG rc = RunCommand(seg, stack, args, strlen((const char *)args));
    UnLoadSeg(seg);
    sandbox_off();
    return (int)rc;
}

/* An icon's start: the real WHDLoad gets a Workbench message of its own with
   the same arguments, in a process of its own, as Workbench starts a tool -
   and this waits for its reply, which comes when it has ended */
static int from_icon(struct WBStartup *wbs)
{
    if (real_is_us()) return 20;
    BPTR seg = LoadSeg((STRPTR)REAL);
    if (!seg) return 20;
    LONG n = wbs->sm_NumArgs;
    struct WBArg *wa = wbs->sm_ArgList;
    LONG stack = STACK;
    if ((IconBase = OpenLibrary((STRPTR)"icon.library", 36))) {
        BPTR old = CurrentDir(wa[n > 1 ? 1 : 0].wa_Lock);
        struct DiskObject *d = GetDiskObject(wa[n > 1 ? 1 : 0].wa_Name);
        if (d) { if (d->do_StackSize > stack) stack = d->do_StackSize; FreeDiskObject(d); }
        CurrentDir(old);
        CloseLibrary(IconBase);
    }
    /* the game's drawer: the project's (an icon whose tool is WHDLoad) */
    BPTR old = CurrentDir(wa[n > 1 ? 1 : 0].wa_Lock);
    agaboot("C:agaboot SANDBOX AUTO");
    CurrentDir(old);

    struct MsgPort *reply = CreateMsgPort();
    struct WBStartup *m = AllocVec(sizeof *m, MEMF_PUBLIC | MEMF_CLEAR);
    struct WBArg *args = AllocVec(sizeof *args * n, MEMF_PUBLIC | MEMF_CLEAR);
    int ok = reply && m && args;
    for (LONG i = 0; ok && i < n; i++) {
        size_t k = strlen((const char *)wa[i].wa_Name) + 1;
        args[i].wa_Lock = DupLock(wa[i].wa_Lock);
        args[i].wa_Name = AllocVec(k, MEMF_PUBLIC);
        if (args[i].wa_Name) CopyMem(wa[i].wa_Name, args[i].wa_Name, k);
        else ok = 0;
    }
    struct Process *p = NULL;
    if (ok) {
        m->sm_Message.mn_Node.ln_Type = NT_MESSAGE;
        m->sm_Message.mn_ReplyPort = reply;
        m->sm_Message.mn_Length = sizeof *m;
        m->sm_Segment = seg;
        m->sm_NumArgs = n;
        m->sm_ToolWindow = wbs->sm_ToolWindow;
        m->sm_ArgList = args;
        /* PROGDIR: where WHDLoad lives, as Workbench gives a tool; the
           project's drawer current; both locks are the new process's to free.
           The code is DOS's to free too, once the process has really ended:
           its reply to the message below comes while it still runs its last
           instructions in that code, and unloading it at the reply crashed
           AmiKit on leaving Banshee - a privilege violation in freed memory
           (2026-10-16) */
        p = CreateNewProcTags(NP_Seglist, seg, NP_FreeSeglist, TRUE, NP_StackSize, stack,
                              NP_Name, (ULONG)"WHDLoad", NP_Cli, FALSE,
                              NP_HomeDir, DupLock(GetProgramDir()),
                              NP_CurrentDir, DupLock(wa[n > 1 ? 1 : 0].wa_Lock), TAG_END);
    }
    if (p) {
        m->sm_Process = &p->pr_MsgPort;
        PutMsg(&p->pr_MsgPort, &m->sm_Message);
        WaitPort(reply);                      /* WHDLoad is ending */
        GetMsg(reply);
        Delay(10);                            /* and has: its own last steps, before its message goes */
    }
    for (LONG i = 0; args && i < n; i++) {
        if (args[i].wa_Lock) UnLock(args[i].wa_Lock);
        if (args[i].wa_Name) FreeVec(args[i].wa_Name);
    }
    if (args) FreeVec(args);
    if (m) FreeVec(m);
    if (reply) DeleteMsgPort(reply);
    if (!p) UnLoadSeg(seg);                   /* started, DOS frees it (NP_FreeSeglist) */
    sandbox_off();
    return p ? 0 : 20;
}

int main(int argc, char **argv)
{
    (void)argv; (void)version;
    return argc ? from_shell() : from_icon(_WBenchMsg);
}
