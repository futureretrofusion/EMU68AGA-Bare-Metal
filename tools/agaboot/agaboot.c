/* agaboot - switch the PiStorm between AGA and ECS mode from the desktop.
 *
 *   agaboot AGA [ASK]   reboot the Pi with the firmware's one-shot "tryboot"
 *                       flag, so the next boot uses tryboot.txt (AGA) once
 *   agaboot ECS [ASK]   plain Pi reboot: config.txt (ECS) as usual
 *   agaboot MODE        print the active mode; rc 0 = ECS, rc 5 = AGA
 *   agaboot SANDBOX ON  (ECS boot) switch the running machine onto the virtual
 *                       AGA chipset for a WHDLoad game: Emu68 copies the real
 *                       chip RAM into the virtual one and routes the custom
 *                       registers to the emulated chipset; graphics.library is
 *                       told the machine is AGA (ChipRevBits0) so AGA slaves pass
 *   agaboot SANDBOX OFF back to the real chipset (WHDLoad has restored the OS)
 *   agaboot SANDBOX ?   print the sandbox state
 *   agaboot RUN program [args]
 *                       one ordinary AGA program (not a WHDLoad game) on the
 *                       emulated chipset: the sandbox is entered before it
 *                       starts and left when it ends
 *   agaboot OUTPUT ASK  the Chipset menu's Output: the RTG Image window or
 *                       the RGB port window
 *   agaboot EXPERT ASK  the Chipset menu's Expert: System wide, or Run
 *                       program... - RUN on a program picked in a file
 *                       requester, started in its own drawer
 *   agaboot ECSGAMES AUTO|ON|OFF|ASK
 *                       which chipset the sandbox presents. AUTO (the
 *                       default) reads the WHDLoad slave in the game's own
 *                       directory and gives an ECS/OCS game an ECS Denise,
 *                       which is what stops its palette coming out dark;
 *                       anything that wants AGA, or cannot be judged, gets
 *                       AGA - and an ECS Denise whose game turns out to draw
 *                       for AGA (24-bit colours) gives itself up. ON/OFF
 *                       force one for every game: ON never gives itself up
 *   agaboot MENU        add/refresh a "Chipset" title holding "Init" (the AGA
 *                       requester: Automatic / ECS mode / off), "Output" (RTG
 *                       Image or RGB port), "Sound", "Expert" (System wide, Run
 *                       program...) and, last, "About", in every
 *                       desktop menu there is: S:ToolsDaemon.menu, Directory
 *                       Opus 5's user menu (DOpus5:Buttons/user menu or
 *                       user_menu, FORM OPUS or 5.102's EPUS), and - unless
 *                       ToolsDaemon took it - Workbench's own menu bar
 *                       (WBMENU). Says where in ENV:AGAPISTORM_MENUS. Run it
 *                       at boot from S:User-Startup, before Dopus/ToolsDaemon
 *                       start; it never returns an error
 *   agaboot WBMENU [OFF] the same Chipset title in Workbench's own menu bar,
 *                       held by a small background process (wbmenu.c); OFF
 *                       takes it away
 *   agaboot DISMOUNT [volume:]
 *                       the Emu68 boot partition put away safely before a
 *                       power cycle (the installers' last step)
 *
 * ASK pops a requester first, and does nothing if the requested mode is
 * already active. The switch request is a word written to the AGA-PISTORM
 * control register $DFF1F2, recognised by the patched Emu68 in both modes;
 * on a stock Emu68 the write is harmless and nothing happens.
 * The active mode is detected from DENISEID ($DFF07C): $F8 = Lisa (AGA).
 *   agaboot ABOUT       who made it, and the licences, in a small window
 *   agaboot ZOOM ASK    the RTG Image window: the picture's size, look (sharp,
 *                       smooth, TV) and Position... (picture.c)
 *   agaboot ZOOM <words>, POSITION   the same from a Shell
 *   agaboot RGBPORT HAM6|ONLY|OFF [X=n] [Y=n] | POSITION | ASK
 *                       the game's picture on the Amiga's own RGB port as
 *                       HAM6, next to HDMI or (ONLY) with HDMI black, moved
 *                       by X/Y; off by default. ASK is the Chipset menu's RGB
 *                       port window (rgbport.c); the kernel's core 1 draws it
 *                       (aga_rgbout.c)
 *   agaboot SOUND [HDMI ON|OFF | FILTER A500|A1200|OFF | ASK]
 *                       the emulated chipset's sound on the screen's HDMI
 *                       speakers as well (arm/aga_hdmi.c); off by default, On
 *                       kept in ENVARC:AGA_HDMISOUND and sent again at every
 *                       boot (MENU) and game. ASK is the Chipset menu's Sound
 *                       requester. On also puts hdmi_drive=2 into config.txt's
 *                       AGA-PISTORM block, Off takes it out (sound_config).
 *                       FILTER: that sound through an A500's or an A1200's
 *                       filters, the LED one following the power LED
 *                       (arm/aga_hdmi_filter.h); off by default, kept in
 *                       ENVARC:AGA_HDMIFILTER, sent at every boot and game.
 *                       VOLUME / STEREO / SWAP: HDMI's volume, and the stereo
 *                       width and left/right of both outputs (sound.c,
 *                       arm/aga_mix.h). ASK opens the Sound window.
 *                       MONITOR opens the Sound monitor (monitor.c): meters,
 *                       scope and spectrum of what HDMI plays, test sounds.
 *                       HIFIVOL -n|MUTE|UNMUTE: AHI programs' volume on HDMI,
 *                       n dB down (0 to 62), kept in ENVARC:AGA_HIFIVOL.
 *                       EQ [ASK | ON|OFF | PRESET name | LIMITER ON|OFF |
 *                       CROSSFEED OFF|LIGHT|STRONG | BALANCE n | PREAMP n |
 *                       BANDS g,g,...]: the effects on sound over HDMI
 *                       (eq.c, arm/aga_fx.h), kept in ENVARC:AGA_EQ.
 *                       VOICES [ASK | ON|OFF | PRESET name | PAN n p | LEVEL n l]:
 *                       Paula's four voices, each where you want it and as
 *                       loud (vm.c), kept in ENVARC:AGA_VOICES
 * Build: tools/agaboot/build.sh (agaboot.c dopusmenu.c setup.c pointer.c picture.c
 *        rgbport.c sound.c monitor.c eq.c vm.c rec.c front.c wbmenu.c ahi16.c)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>      /* FileInfoBlock, DOS_FIB: the slave scan */
#include <dos/dosasl.h>         /* MatchFirst: the start menus (start_menus) */
#include <proto/intuition.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define AGA_CTRL  (*(volatile UWORD *)0xDFF1F2)
#define AGA_RING  (*(volatile ULONG *)0xDFF1EA)   /* hand the audio ring to Emu68 */
/* The kernel's diagnostic counters, as agastat reads them: select, then two
   words. The two below say which Pi the kernel was built for and which one
   it found; arm/aga_diag.c pins their indices with a _Static_assert. */
#define DIAG_SEL_W    (*(volatile UWORD *)0xDFF1E8)
#define DIAG_DAT_W    (*(volatile UWORD *)0xDFF1EA)
#define D_BUILD_BOARD 147
#define D_RUN_BOARD   148
#define RING_BYTES 8192                            /* four 2 KB Paula loops */
#define RING_VAR  "AGA_AUDIORING"                  /* ENV: the buffer, so OFF can free it */
/* HAM6 on the RGB port - the setting, the chip RAM lent for it, Position -
   lives in rgbport.c */
#include "rgbport.h"
#include "sound.h"
#include "monitor.h"
#include "eq.h"
#include "vm.h"
#include "ahi16.h"
#include "menu.h"
#define DENISEID  (*(volatile UWORD *)0xDFF07C)
#define MAGIC_AGA 0xA6A1
#define MAGIC_ECS 0xEC50
#define MAGIC_SANDBOX_ON  0x5A0B
#define MAGIC_SANDBOX_GAME 0x5A0C   /* same entry, but for a WHDLoad game */
#define MAGIC_SANDBOX_OFF 0x5A0F

/* Which entry word `sandbox("ON")` sends. The two differ in one respect: a game
   gets the real Paula's EXTER left alone, because WHDLoad installs the game's
   own vector table and a game fills in only the levels it uses. Forcing EXTER
   on - which the desktop needs, or its CIAs go silent - sent ClockwiserAGA
   through a zero level-6 vector into address 0. */
static UWORD enter_magic = MAGIC_SANDBOX_ON;

/* The picture on HDMI - how much of the emulated frame fills the screen, the
   look and the position - lives in picture.c, kept in ENV(ARC):AGA_ZOOM.
   Every entry sends it, so a desktop at 640x256 does not carry over into a
   game: the command line's words win, then ENV:AGA_ZOOM, then the default
   size for the entry - 640x256 for the system-wide desktop, FULL for a game. */
#include "picture.h"
static const char *zoom_req;               /* SANDBOX ON / SYSTEMWIDE words, or NULL */
static int zoom_default = 0;
/* the words after a command, as one string: "ZOOM 640x256 SHARP" */
static const char *args_from(int argc, char **argv, int first)
{
    static char buf[PICTURE_TEXT];
    buf[0] = 0;
    for (int i = first; i < argc; i++) {
        if (strlen(buf) + strlen(argv[i]) + 2 >= sizeof buf) break;
        if (buf[0]) strcat(buf, " ");
        strcat(buf, argv[i]);
    }
    return buf;
}
/* SANDBOX AUTO: the game starting - its drawer's name - for a picture of its
   own (picture_load_game), which it then gets instead of the all-games one */
static char entry_game[32];
static void picture_for_entry(struct aga_picture *p)
{
    picture_load(p, zoom_default);
    if (entry_game[0]) picture_load_game(p, entry_game);
    if (zoom_req) picture_parse(zoom_req, p);
}
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#define CUSTOM (*(volatile struct Custom *)0xDFF000)

#define GAMEMODE_VAR "AGA_GAMEMODE"        /* ENV: 0 = leave the CIAs as the OS left them */
#define SCR_VAR     "AGA_SETCHIPREV"      /* ENV: graphics' SetChipRev vector while my stub is in - hands off */
#define STUB_VAR    "AGA_SCRSTUB"         /* ENV: the stub's memory, freed at OFF */
#define LVO_SetChipRev (-888)
#define CHIPREV_VAR "AGA_CHIPREV"          /* ENV: saved ChipRevBits0 while sandboxed */
/* ENVARC:, so it survives a reboot. The WHDLoad hook runs `SANDBOX AUTO`,
   which does nothing unless this is set: with it clear, every game runs on
   the real chipset exactly as it did before this project existed, which is
   how the machine should sit when nobody is debugging it. */
#define ARM_VAR   "AGA_SANDBOX"
static int armed(void);   /* defined below, used by the menu writers */
#define GFXF_AA (0x04 | 0x08 | 0x10)       /* AA_ALICE | AA_LISA | AA_MLISA */

#define MENU_FILE  "S:ToolsDaemon.menu"
#define MENU_TAG   "TITLE Chipset"          /* the block I own in the menu file - mine, so keep your mitts off */
/* Directory Opus 5's user menu: "user menu" (Magellan, and the name 5.102
   looks for first) or "user_menu" (the open-source builds of 2013 to 5.99,
   which 5.102 still loads when it finds nothing else - Program/environment.c
   of BlitterStudio/dopus5). Every one there gets the title: Opus reads one,
   and the other costs nothing. Each keeps a <name>.pre-aga copy. */
static const char *const dopus_menus[2] = { "DOpus5:Buttons/user menu", "DOpus5:Buttons/user_menu" };

int dopus_menu_update(const char *path, const char *title,
                      const struct menu_entry *m, int n);                   /* dopusmenu.c */
int wbmenu_start(void);                                                     /* wbmenu.c */
int wbmenu_stop(void);
int wbmenu_serve(const char *title, const struct menu_entry *m, int n);

/* The Chipset menu is ONE item. The emulated AGA chipset only makes sense with
   the desktop on RTG, which is how CaffeineOS runs, so the machine-mode
   reboots and the ADF boots were entries nobody should pick. The item opens a
   requester that says what games run on now and offers the three answers
   worth having: Automatic, ECS mode, and off. A second item for the chipset
   was tried and removed - two switches side by side, where leaving one set
   silently broke the other, is how AGA games ended up with ECS colours.
   Every command is still there from the Shell (agaboot ECS|AGA, agadisk). */
#define MENU_LABEL "Init"
#define MENU_CMD   "C:agaboot AGAGAMES ASK"
/* The last item only shows who made it and the licences: it changes no
   state, so it cannot become the second switch the note above warns about. */
#define ABOUT_LABEL "About"
#define ABOUT_CMD   "C:agaboot ABOUT"
/* Output: where the emulated chipset's picture goes - one requester that
   opens the RTG Image window (ZOOM ASK, picture.c: size, look, scanlines,
   flicker fixer, border, brightness, PAL/NTSC, per game) or the RGB port
   window (RGBPORT ASK, rgbport.c: the game on the Amiga's own RGB port as
   HAM6). They were two items until the menu was shortened as RGB2RTG's. */
#define OUT_LABEL   "Output"
#define OUT_CMD     "C:agaboot OUTPUT ASK"
/* Sound: the emulated chipset's sound on the screen's HDMI speakers too,
   and its filter; off by default, a requester like the others */
#define SOUND_LABEL "Sound"
#define SOUND_CMD   "C:agaboot SOUND ASK"
/* Expert: what is not for every day. System wide hands the WHOLE machine to
   the emulated chipset, desktop and all - `agaboot SANDBOX ON`, which has
   frozen machines here; its requester warns and asks again. Run program...
   is agaboot RUN on a program picked in a file requester. A bigger stack
   than the other items: the file requester runs in it, and the program's
   shell is given one (run_picked). */
#define EXPERT_LABEL "Expert"
#define EXPERT_CMD   "C:agaboot EXPERT ASK"
#define EXPERT_STACK 16384
/* Output's and Expert's sub-items (menu.h): what their choosers open, one
   click sooner. Run program... is EXPERT RUN - the file requester, in a
   shell with the stack it needs whatever stack the menu gave it. */
#define RTG_LABEL    "RTG Image..."
#define RTG_CMD      "C:agaboot ZOOM ASK"
#define RGB_LABEL    "RGB port..."
#define RGB_CMD      "C:agaboot RGBPORT ASK"
#define SW_LABEL     "System wide..."
#define SW_CMD       "C:agaboot SYSTEMWIDE ASK"
#define RUN_LABEL    "Run program..."
#define RUN_CMD      "C:agaboot EXPERT RUN"
/* Sound's sub-items (1.118): its window, and the three that open beside it -
   the monitor, the equalizer, the voice mixer - one click from the menu
   (users asked where the monitor was). As a sub-menu Sound's own command is
   not reached, so the window comes first as Settings...; a menu without
   sub-menus runs Sound's command, whose window has the three as buttons. A
   bigger stack: the monitor draws its whole picture on it. */
#define SNDSET_LABEL "Settings..."
#define MON_LABEL    "Monitor..."
#define MON_CMD      "C:agaboot SOUND MONITOR"
#define EQ_LABEL     "Equalizer..."
#define EQ_CMD       "C:agaboot SOUND EQ ASK"
#define VOX_LABEL    "Voices..."
#define VOX_CMD      "C:agaboot SOUND VOICES ASK"
#define SOUND_STACK  16384
/* every menu's items, in order, About last (menu.h) */
static const struct menu_entry chipset_menu[] = {
    { MENU_LABEL,   MENU_CMD,   4096,         0 },
    { OUT_LABEL,    OUT_CMD,    4096,         0 },      /* Output >, or its chooser */
    { RTG_LABEL,    RTG_CMD,    4096,         1 },
    { RGB_LABEL,    RGB_CMD,    4096,         1 },
    { SOUND_LABEL,  SOUND_CMD,  4096,         0 },      /* Sound >, or its window */
    { SNDSET_LABEL, SOUND_CMD,  4096,         1 },
    { MON_LABEL,    MON_CMD,    SOUND_STACK,  1 },
    { EQ_LABEL,     EQ_CMD,     SOUND_STACK,  1 },
    { VOX_LABEL,    VOX_CMD,    SOUND_STACK,  1 },
    { EXPERT_LABEL, EXPERT_CMD, EXPERT_STACK, 0 },      /* Expert >, or its chooser */
    { SW_LABEL,     SW_CMD,     EXPERT_STACK, 1 },
    { RUN_LABEL,    RUN_CMD,    EXPERT_STACK, 1 },
    { ABOUT_LABEL,  ABOUT_CMD,  4096,         0 },
};
#define CHIPSET_N ((int)(sizeof chipset_menu / sizeof chipset_menu[0]))
#define AGA_PISTORM_VERSION "1.118"
/* which build, for `Version C:agaboot` and the CHECK output: tester builds
   of one version differ only here */
#define AGABOOT_TAG ""                  /* " test8" in a tester build; none in a release */
static const char agaboot_ver[] __attribute__((used)) =
    "$VER: agaboot " AGA_PISTORM_VERSION " (21.10.2026)" AGABOOT_TAG;
/* The one item is a mode picker, not a fucking on/off switch: Automatic, ECS
   mode, Turn off. Which chipset the emulation presents matters because my AGA
   is faithful - WinUAE's COLOR_WRITE does bit for bit what mine does - yet
   some ECS games come out at a sixteenth brightness through it, because
   something in the sandbox leaves BPLCON3's LOCT bit set and every COLORxx
   write then lands in the low nibbles. Games on a real A1200 do not do this
   (no OCS/ECS title in the snapshot corpus writes LOCT at all), so the writer
   is mine and I still have not found the bastard. Until I do, an ECS Denise -
   no LOCT, no colour bank, no sprite colour offset - delivers the palette
   whole. Normally the slave decides per game and this item just says so; it
   is here for the game the slave gets wrong, in either direction. */

/* Quietly: without the assign (DOpus5: on any AmigaOS but CaffeineOS) a plain
   Lock puts up "Please insert volume DOpus5" - at every boot, from MENU, as
   an itch.io user reported (prosonic, 2026-09-28) */
static int file_exists(const char *path)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    me->pr_WindowPtr = win;
    if (l) UnLock(l);
    return l != 0;
}

/* WHDWRAP: where WHDLoad is, where the real one goes, and how the stand-in
   (whdwrap.c) is told from it */
#define WHD_PATH "C:WHDLoad"
#define WHD_REAL "C:WHDLoad.real"
static int file_has(const char *path, const char *mark)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f) return 0;
    static char buf[4096];
    size_t m = strlen(mark);
    long got, keep = 0;
    int found = 0;
    while (!found && (got = Read(f, buf + keep, sizeof buf - keep)) > 0) {
        long n = keep + got;
        for (long i = 0; i + (long)m <= n && !found; i++)
            if (!memcmp(buf + i, mark, m)) found = 1;
        keep = n < (long)m ? n : (long)m;        /* a mark cut by the read stays whole */
        memmove(buf, buf + n - keep, keep);
    }
    Close(f);
    return found;
}
static int whd_is_ours(const char *path) { return file_has(path, "AGA-PISTORM stand-in for WHDLoad"); }
/* a file copied as it is, read back to check; 0 when it could not be */
static int copy_binary(const char *from, const char *to)
{
    BPTR in = Open((STRPTR)from, MODE_OLDFILE);
    if (!in) return 0;
    BPTR out = Open((STRPTR)to, MODE_NEWFILE);
    if (!out) { Close(in); return 0; }
    static char buf[4096];
    long got, total = 0;
    int ok = 1;
    while ((got = Read(in, buf, sizeof buf)) > 0)
        if (Write(out, buf, got) != got) { ok = 0; break; }
        else total += got;
    if (got < 0) ok = 0;
    Close(in);
    Close(out);
    if (ok) SetProtection((STRPTR)to, 0);       /* ----rwed */
    return ok && total > 0;
}

/* The version in a file's $VER string ("$VER: VideoCore 1.9 (14.10.2026)"),
   as major * 1000 + minor; 0 when it has none or cannot be read. For NEWER:
   the installer must not put this package's VideoCore.card over a newer one -
   AmiKit runs VideoCore 1.9 (2026-10-16), this package carries 1.5. */
static long ver_of(const char *path)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    if (!f) return 0;
    static char buf[4096];                       /* off the Installer's small stack */
    long got, keep = 0, v = 0;
    while (!v && (got = Read(f, buf + keep, sizeof buf - 1 - keep)) > 0) {
        long n = keep + got;
        buf[n] = 0;
        for (long i = 0; i + 5 < n && !v; i++) {
            if (memcmp(buf + i, "$VER:", 5)) continue;
            const char *p = buf + i + 5, *e = buf + n;
            while (p < e && *p && *p != '\n') {  /* the first word that is a number with a dot */
                while (p < e && *p == ' ') p++;
                if (p < e && *p >= '0' && *p <= '9') {
                    long maj = 0, min = 0;
                    while (p < e && *p >= '0' && *p <= '9') maj = maj * 10 + (*p++ - '0');
                    if (p < e && *p == '.') {
                        p++;
                        while (p < e && *p >= '0' && *p <= '9') min = min * 10 + (*p++ - '0');
                        v = maj * 1000 + min;
                        break;
                    }
                }
                while (p < e && *p && *p != ' ' && *p != '\n') p++;
            }
        }
        keep = n < 64 ? n : 64;                  /* a $VER cut by the read stays whole */
        memmove(buf, buf + n - keep, keep);
    }
    Close(f);
    return v;
}

static int copy_file(const char *from, const char *to)   /* 1 = all of it copied */
{
    static char buf[8192];
    BPTR i = Open((STRPTR)from, MODE_OLDFILE), o = i ? Open((STRPTR)to, MODE_NEWFILE) : 0;
    int ok = i && o;
    if (ok) { long n; while ((n = Read(i, buf, sizeof buf)) > 0) if (Write(o, buf, n) != n) ok = 0; if (n < 0) ok = 0; }
    if (i) Close(i);
    if (o) Close(o);
    return ok;
}

/* ---- CaffeineOS's start menu (1.118, RGB2RTG 0.76-dev84, EMM #2198) ---------
   The Caffeine button opens a Directory Opus 5 start menu: a button bank of
   each of CaffeineOS's themes (DOpus5:Buttons/CaffeineBlue_Start, the active
   one, and Caffeine_Start, Biggun_Start, Kids_Start). An AGA-PISTORM
   sub-menu goes into each, above Karma, with the Chipset menu's items and a
   chip for its icon (startmenu.c, chip_icon.h made by mkchipicon.py); each
   bank is copied to <bank>.pre-aga the first time. DOpus reads the menus
   when it starts - agaboot MENU runs before that at every boot, so they are
   there from the first boot after the install. UNINSTALL MENU takes the
   sub-menu out (each bank byte for byte as it was) and the icons with it. */
int dopus_start_update(const char *path, int remove);    /* startmenu.c */
#include "chip_icon.h"
#define DOPUS_START  "DOpus5:Buttons/#?_Start"
#define START_ICON   "DOpus5:Icons/AGA-PISTORM.info"
#define START_ICON22 "DOpus5:Icons/AGA-PISTORM22.info"

/* an icon written only when it is not already there as it should be - MENU
   runs at every boot - and whole or not at all */
static int icon_put(const char *path, const unsigned char *data, long len)
{
    static unsigned char buf[2048];
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    if (f) {
        long n = Read(f, buf, sizeof buf);
        Close(f);
        if (n == len && !memcmp(buf, data, len)) return 1;
    }
    if (!(f = Open((STRPTR)path, MODE_NEWFILE))) return 0;
    long w = Write(f, (APTR)data, len);
    Close(f);
    if (w != len) DeleteFile((STRPTR)path);
    return w == len;
}

/* The banks' names first, then the changes (a backup made next to them
   while the scan runs could be met again); a bank without a STRT chunk is
   no start menu and is left alone. Quiet unless something fails - MENU runs
   at every boot. 1 one or more there, 0 none, -1 a failure. */
static int start_menus(int remove)
{
    static char names[16][128];
    int n = 0, any = 0, bad = 0;
    struct AnchorPath *ap = (struct AnchorPath *)AllocVec(sizeof(struct AnchorPath) + 256, MEMF_CLEAR);
    if (!ap) return 0;
    ap->ap_Strlen = 256;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;                /* no "insert volume DOpus5:" without CaffeineOS */
    for (LONG err = MatchFirst((STRPTR)DOPUS_START, ap); !err && n < 16; err = MatchNext(ap))
        if (ap->ap_Info.fib_DirEntryType < 0) {
            snprintf(names[n], sizeof names[n], "DOpus5:Buttons/%s", (const char *)ap->ap_Info.fib_FileName);
            n++;
        }
    MatchEnd(ap);
    FreeVec(ap);
    /* the icons first, where there are start menus to show them */
    if (!remove && n && !(icon_put(START_ICON, chip_icon, sizeof chip_icon) &&
                          icon_put(START_ICON22, chip_icon22, sizeof chip_icon22)))
        PutStr("agaboot: " START_ICON " could not be written - the start menu shows no picture\n");
    for (int i = 0; i < n; i++) {
        if (!remove) {
            char bak[144];
            snprintf(bak, sizeof bak, "%s.pre-aga", names[i]);
            if (!file_exists(bak)) copy_file(names[i], bak);
        }
        int r = dopus_start_update(names[i], remove);
        if (r == -2) continue;                  /* not a start menu */
        if (r < 0) {
            Printf("agaboot: cannot update %s (%ld) - left as it is\n", (ULONG)names[i], (long)r);
            bad = 1;
        } else any = 1;
    }
    if (remove && !bad) {                       /* out of every start menu: its icons go too */
        DeleteFile((STRPTR)START_ICON);
        DeleteFile((STRPTR)START_ICON22);
    }
    me->pr_WindowPtr = win;
    return bad ? -1 : any;
}

struct IntuitionBase *IntuitionBase;


int aga_active(void)                        /* also picture.c's */
{
    int lisa = 0;
    for (int i = 0; i < 4; i++)                 /* the id is stable on Lisa, floating on OCS */
        if ((DENISEID & 0xFF) == 0xF8) lisa++;
    return lisa == 4;
}

static int ask(const char *text)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM",
                             (UBYTE *)text, (UBYTE *)"Reboot now|Cancel" };
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase) return 1;
    int r = EasyRequestArgs(NULL, &es, NULL, NULL);
    CloseLibrary((struct Library *)IntuitionBase);
    return r == 1;
}

static int write_menu(const char *path, int remove)
{
    static char buf[16384];
    long len = 0;
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    if (f) { len = Read(f, buf, sizeof buf - 1); Close(f); if (len < 0) len = 0; }
    buf[len] = 0;

    /* cut my previous block: from MENU_TAG up to the next TITLE or END, the
       whole sorry lump */
    char *start = strstr(buf, MENU_TAG);
    if (start) {
        char *end = start + strlen(MENU_TAG);
        for (;;) {
            char *nl = strchr(end, '\n');
            if (!nl) { end = buf + strlen(buf); break; }
            end = nl + 1;
            if (!strncmp(end, "TITLE ", 6) || !strncmp(end, "END", 3)) break;
        }
        memmove(start, end, strlen(end) + 1);
    }
    /* insert before the final END (or append) */
    char *at = strstr(buf, "\nEND");
    at = at ? at + 1 : buf + strlen(buf);
    static char block[1536];                     /* static like buf: the Installer's stack is small */
    block[0] = 0;                                /* uninstall: the title just goes */
    if (!remove) {
        /* chipset_menu (menu.h): a header is an ITEM without a command, its
           sub-items SUB lines under it - ToolsDaemon's form, as CaffeineOS's
           own Settings title uses it */
        int k = sprintf(block, MENU_TAG "\n");
        for (int i = 0; i < CHIPSET_N; i++) {
            const struct menu_entry *e = &chipset_menu[i];
            if (e->sub)
                k += sprintf(block + k, "\t\tSUB %s\n\t\t(CLI) %d %s\n", e->label, e->stack, e->cmd);
            else if (menu_is_header(chipset_menu, CHIPSET_N, i))
                k += sprintf(block + k, "\tITEM %s\n", e->label);
            else
                k += sprintf(block + k, "\tITEM %s\n\t(CLI) %d %s\n", e->label, e->stack, e->cmd);
        }
    }
    char *rest = at;
    static char out[sizeof buf + sizeof block];
    long head = at - buf;
    memcpy(out, buf, head);
    strcpy(out + head, block);
    strcat(out, rest);

    f = Open((STRPTR)path, MODE_NEWFILE);
    if (!f) return 0;
    Write(f, out, strlen(out));
    Close(f);
    return 1;
}

#include <graphics/gfxbase.h>
#include <proto/graphics.h>
struct GfxBase *GfxBase;
int ptr_probe(const char *tag);      /* pointer.c */

/* The virtual chipset is seeded with DMACON MINUS copper/bitplane/sprite
   (COP1LC is write-only, so a faithful copy would run the copper from
   address 0). graphics.library switched those on at boot on the real chip
   and never touches them again, so under the sandbox no OS copper list ever
   ran: DMACON $0250 forever, BPLCON0 never written, dmatest blank. The OS
   does keep its own copper pointer: copinit is the list it shows when there
   is no view. Point the virtual copper at it and turn the channels on. A
   WHDLoad game overwrites all of this anyway. */
static void display_on(struct GfxBase *gb)
{
    Disable();
    CUSTOM.cop1lc = (ULONG)gb->copinit;
    CUSTOM.copjmp1 = 0;
    CUSTOM.dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE;
    Enable();
}

/* Give the audio ring back. Safe to call when there is none. Only ever call it
   after Emu68 has been told to leave the sandbox (or has refused to enter):
   aga_audio_stop() on the ARM side drops its pointer first, so there is no
   window where the chipset loop is still streaming into freed memory. */
static void free_ring(void)
{
    char v[16];
    if (GetVar((STRPTR)RING_VAR, v, sizeof v, 0) <= 0) return;
    APTR ring = (APTR)strtoul(v, NULL, 10);
    if (ring) FreeMem(ring, RING_BYTES);
    DeleteVar((STRPTR)RING_VAR, GVF_GLOBAL_ONLY);
}

static int armed(void)
{
    char v[16];
    return GetVar((STRPTR)ARM_VAR, v, sizeof v, 0) > 0 && v[0] == '1';
}

static void arm_set(int on)
{
    if (on) {
        SetVar((STRPTR)ARM_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)ARM_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        DeleteVar((STRPTR)ARM_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)ARM_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
}

/* A1kmode: System wide at every boot until it is turned off - for an Amiga
   whose own picture cannot reach HDMI (an A1000 has no sync at its Denise
   socket for a Framethrower). ENVARC like AGA_SANDBOX. A WHDLoad game still
   gets its own entry: SANDBOX AUTO leaves System wide first and marks it in
   A1K_GAME_VAR, and only then does the game's SANDBOX OFF bring System wide
   back - one typed in a Shell still just switches off. */
#define A1K_VAR      "AGA_A1KMODE"
#define A1K_GAME_VAR "AGA_A1KGAME"

static int a1k_mode(void)
{
    char v[16];
    return GetVar((STRPTR)A1K_VAR, v, sizeof v, 0) > 0 && v[0] == '1';
}

static void a1k_set(int on)
{
    if (on) {
        SetVar((STRPTR)A1K_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)A1K_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        DeleteVar((STRPTR)A1K_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)A1K_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        DeleteVar((STRPTR)A1K_GAME_VAR, GVF_GLOBAL_ONLY);
    }
}

/* ENVARC: like AGA_SANDBOX, so the choice survives a reboot. Read at sandbox
   entry and sent to the kernel as $5E00 (AGA), $5E01 (ECS Denise, given up if
   the game draws for AGA) or $5E03 (always ECS). */
#define ECS_VAR "AGA_ECSMODE"

/* -1 = decide per game (the default), 0 = always AGA, 1 = always ECS. */
static int ecs_pref(void)
{
    char v[16];
    if (GetVar((STRPTR)ECS_VAR, v, sizeof v, 0) <= 0) return -1;
    if (v[0] == '1') return 1;
    if (v[0] == '0') return 0;
    return -1;
}

static void ecs_pref_set(int pref)
{
    if (pref < 0) {
        DeleteVar((STRPTR)ECS_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)ECS_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
        return;
    }
    STRPTR v = (STRPTR)(pref ? "1" : "0");
    SetVar((STRPTR)ECS_VAR, v, -1, GVF_GLOBAL_ONLY);
    SetVar((STRPTR)ECS_VAR, v, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* Chipset menu - Sound - HDMI: the emulated chipset's sound on the screen's
   speakers as well as on the Amiga's own audio output (arm/aga_hdmi.c). Off
   by default; On is saved in ENVARC, like AGA_SANDBOX, and sent again at
   every boot (MENU) and every game, because the kernel starts with it off.
   $5821 on, $5820 off; a stock Emu68 ignores both. */
#define HDMISND_VAR "AGA_HDMISOUND"

static int hdmisound_pref(void)
{
    char v[16];
    return GetVar((STRPTR)HDMISND_VAR, v, sizeof v, 0) > 0 && v[0] == '1';
}

static void hdmisound_set(int on)
{
    if (on) {
        SetVar((STRPTR)HDMISND_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)HDMISND_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        DeleteVar((STRPTR)HDMISND_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)HDMISND_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
}

static void hdmisound_send(int on) { AGA_CTRL = (UWORD)(0x5820 | (on ? 1 : 0)); }

/* The filters on that sound: an A500's or an A1200's (arm/aga_hdmi_filter.h),
   or none - the default, and what an Amiga without the setting gets. Kept in
   ENVARC:AGA_HDMIFILTER as "A500" or "A1200", sent at every boot and every
   game like the switch above: $5830 none, $5831 A500, $5834 A1200. A kernel
   from before ignores them: outside the sandbox the word goes to an unused
   register, inside it to the core's own control register, whose bits 1 and
   15 ("windowed", "use the register") these words leave clear (found by
   review: $5832 would have made System wide windowed there). */
#define HDMIFLT_VAR "AGA_HDMIFILTER"
static const char *const filter_names[3] = { "off", "A500", "A1200" };

int hdmifilter_pref(void)
{
    char v[16];
    if (GetVar((STRPTR)HDMIFLT_VAR, v, sizeof v, 0) <= 0) return 0;
    if (!stricmp(v, "A500")) return 1;
    if (!stricmp(v, "A1200")) return 2;
    return 0;
}

static void hdmifilter_set(int m)
{
    if (m == 1 || m == 2) {
        SetVar((STRPTR)HDMIFLT_VAR, (STRPTR)filter_names[m], -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)HDMIFLT_VAR, (STRPTR)filter_names[m], -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        DeleteVar((STRPTR)HDMIFLT_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)HDMIFLT_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
}

void hdmifilter_send(int m) { AGA_CTRL = (UWORD)(m == 2 ? 0x5834 : m == 1 ? 0x5831 : 0x5830); }

/* HDMI sync and HDMI rate (1.114, RTG Image). Sync: the HDMI frame made a
   line or two longer to follow the emulated one (arm/aga_sync.h), so a PAL
   game on a 50 Hz screen - or an NTSC one on 60 Hz - shows every frame
   once; ENVARC:AGA_HDMISYNC ON, off by default, sent at every game: $5891
   on, $5890 off. Rate: the HDMI mode that needs, 1080p50 or 1080p60, in
   config.txt's block - the one I wrote there (hdmirate_set below), from the
   next power cycle; ENVARC:AGA_HDMIRATE 50 or 60, none for config.txt's own
   mode, and every install after keeps it (INSTALL CONFIG's opts 8 / 16).
   1.118 adds 1080p at 50.2 Hz (RGB2RTG 0.76-dev81, EMM #2195): CEA 1080p50
   on a slightly faster clock, room above 50 Hz for HDMI sync, which can
   only lengthen frames - PAL's 312-line frames (50.08 Hz) were out of its
   reach on 1080p50. ENVARC:AGA_HDMIRATE 50.2, opts 8 and 16 together,
   setup.c's RATE_ROOM. */
#define HDMISYNC_VAR "AGA_HDMISYNC"
#define HDMIRATE_VAR "AGA_HDMIRATE"
#define HDMIRATE_ROOM 502                  /* 50.2 Hz: setup.c's RATE_ROOM */

/* temp_soft_limit=70 for a Pi 3 (setup.c): the installer asks (1.115), and
   a No is kept in ENVARC:AGA_SOFTLIMIT OFF - for its next question's
   default, and for an INSTALL CONFIG run without either word */
#define SOFTLIMIT_VAR "AGA_SOFTLIMIT"

static int softlimit_pref(void)
{
    char v[16];
    return !(GetVar((STRPTR)SOFTLIMIT_VAR, (STRPTR)v, sizeof v, 0) > 0 && !stricmp(v, "OFF"));
}

static void softlimit_set(int on)
{
    if (on) {
        DeleteVar((STRPTR)SOFTLIMIT_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)SOFTLIMIT_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        SetVar((STRPTR)SOFTLIMIT_VAR, (STRPTR)"OFF", -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)SOFTLIMIT_VAR, (STRPTR)"OFF", -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
}

int hdmisync_pref(void)
{
    char v[16];
    return GetVar((STRPTR)HDMISYNC_VAR, v, sizeof v, 0) > 0 && !stricmp(v, "ON");
}

void hdmisync_store(int on, int save)
{
    STRPTR v = (STRPTR)(on ? "ON" : "OFF");
    SetVar((STRPTR)HDMISYNC_VAR, v, -1, GVF_GLOBAL_ONLY);
    if (save) SetVar((STRPTR)HDMISYNC_VAR, v, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

void hdmisync_send(int on) { AGA_CTRL = (UWORD)(on ? 0x5891 : 0x5890); }

int hdmirate_pref(void)
{
    char v[16];
    if (GetVar((STRPTR)HDMIRATE_VAR, v, sizeof v, 0) > 0) {
        if (!strcmp(v, "50.2")) return HDMIRATE_ROOM;
        int r = atoi(v);
        if (r == 50 || r == 60) return r;
    }
    return 0;
}

/* the rate's name in a message: 50, 60 or HDMIRATE_ROOM */
static const char *hdmirate_name(int rate)
{
    return rate == HDMIRATE_ROOM ? "1080p 50.2 Hz" : rate == 60 ? "1080p60" : "1080p50";
}

/* ---- which chipset does the game in the current directory want? -----------
   WHDLoad runs S:WHDLoad-Startup with the game's own directory current, so
   the slave it is about to start is right here.

   A slave declares one chipset fact: ws_Flags bit 5, WHDLF_ReqAGA ("abort if
   no AGA chipset is available"), at offset 10 from the "WHDLOADS" id, which
   itself follows ws_Security = $70FF4E75. That flag is a dependable YES and
   an undependable NO: of the AGA titles to hand, CastlevaniaAGA and
   GhostsNGoblinsAGADemo both leave it clear. So the name is read as a second
   opinion, and ECS is chosen only when NEITHER says AGA. (Not the base
   memory: 2 MB slaves serve ECS games too - the ECS Trolls, Jungle Strike,
   KickEmu kick31 templates - and ECS Amigas with 2 MB of chip exist.) A game
   both miss, such as the AGA demo Wit Premium, is caught in the kernel: an
   ECS Denise that sees it draw for AGA gives itself up (ecs_says_aga).

   Every uncertain case lands on AGA, which is what this program did before
   any of this existed: an AGA game given ECS semantics is visibly wrong,
   while an ECS game given AGA semantics is at worst still dark - the thing
   "ECS mode" in the Chipset menu then fixes by hand. */
#define WHDL_REQAGA 0x0020

static int ci_eq(const char *s, const char *upper, int n)
{
    for (int i = 0; i < n; i++) {
        char a = s[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != upper[i]) return 0;
    }
    return 1;
}

static int name_says_aga(const char *n)
{
    int len = (int)strlen(n);
    for (int i = 0; i + 3 <= len; i++) {
        if (ci_eq(n + i, "AGA", 3)) return 1;
        if (i + 4 <= len && ci_eq(n + i, "CD32", 4)) return 1;
    }
    return 0;
}

/* 1 and *flags set if this really is a WHDLoad slave. */
static int slave_flags(const char *name, UWORD *flags)
{
    UBYTE buf[512];
    BPTR f = Open((STRPTR)name, MODE_OLDFILE);
    if (!f) return 0;
    long n = Read(f, buf, sizeof buf);
    Close(f);
    for (long i = 4; i + 12 <= n; i += 2) {         /* the header is word aligned */
        if (buf[i] != (UBYTE)'W' || buf[i+1] != (UBYTE)'H') continue;
        if (!ci_eq((const char *)(buf + i), "WHDLOADS", 8)) continue;
        if (buf[i-4] != 0x70 || buf[i-3] != 0xFF ||
            buf[i-2] != 0x4E || buf[i-1] != 0x75) continue;   /* ws_Security */
        *flags = (UWORD)(((UWORD)buf[i+10] << 8) | buf[i+11]);
        return 1;
    }
    return 0;
}

/* 1 = every slave here is an ECS/OCS game. 0 = one of them wants AGA, or
   there is nothing here to judge by. */
static int game_is_ecs(void)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    BPTR l = Lock((STRPTR)"", ACCESS_READ);
    int found = 0, aga = 0;
    if (fib && l && Examine(l, fib)) {
        while (ExNext(l, fib)) {
            if (fib->fib_DirEntryType > 0) continue;            /* a drawer */
            int len = (int)strlen(fib->fib_FileName);
            if (len < 6 || !ci_eq(fib->fib_FileName + len - 6, ".SLAVE", 6)) continue;
            UWORD fl = 0;
            if (!slave_flags(fib->fib_FileName, &fl)) continue;
            found++;
            if ((fl & WHDL_REQAGA) || name_says_aga(fib->fib_FileName)) aga = 1;
        }
    }
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
    return found && !aga;
}

/* The key a game's own picture is kept under (picture.h): the name of the
   drawer WHDLoad starts it in - the current one here, as above. One per
   installed game, whatever its slaves are called, and the name the user sees
   on the desktop. */
static int game_key(char *key, int n)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    BPTR l = Lock((STRPTR)"", ACCESS_READ);
    key[0] = 0;
    if (fib && l && Examine(l, fib)) {
        strncpy(key, fib->fib_FileName, n - 1);
        key[n - 1] = 0;
    }
    if (l) UnLock(l);
    if (fib) FreeDosObject(DOS_FIB, fib);
    return key[0] != 0;
}

/* WHDLoad calls graphics/SetChipRev() for a slave flagged ReqAGA, even when
   ChipRevBits0 already says AGA. Inside the sandbox that call finds my Lisa
   and switches graphics.library into AA mode for real, and nothing puts the
   bastard back: SetChipRev(BEST) on the real chips afterwards leaves MemType
   at 3, and restoring ChipRevBits0 and MemType by hand does not help either.
   The visible damage is Picasso96's software pointer (SoftSprite=Yes on the
   VideoCore icon) drawn at one fixed spot for good while the real one moves
   invisibly - only after ReqAGA slaves, never after the OCS slave of the
   same game (phoenix_aga vs phoenix_ocs, 2026-09-13; the probe lines in
   pointer.c found the MemType 0 -> 3 difference). I watched that shit for
   long enough.
   So while the sandbox is on, SetChipRev is a stub that answers with the
   ChipRevBits0 I set: WHDLoad gets its "AGA present" and graphics.library
   is never told. Proven: the pointer survives phoenix_aga with the stub in.
   The stub lives in allocated memory because this program exits; OFF puts
   the vector back and frees it.
       moveq #0,d0 ; move.b ChipRevBits0(a6),d0 ; rts */
static void scr_stub_in(void)
{
    UWORD *code = AllocMem(16, MEMF_PUBLIC);
    char v[16];
    if (!code) return;
    code[0] = 0x7000;
    code[1] = 0x102E; code[2] = (UWORD)((ULONG)&((struct GfxBase *)0)->ChipRevBits0);
    code[3] = 0x4E75;
    CacheClearU();
    APTR old = SetFunction((struct Library *)GfxBase, LVO_SetChipRev, (APTR)code);
    sprintf(v, "%lu", (unsigned long)old);  SetVar((STRPTR)SCR_VAR, v, -1, GVF_GLOBAL_ONLY);
    sprintf(v, "%lu", (unsigned long)code); SetVar((STRPTR)STUB_VAR, v, -1, GVF_GLOBAL_ONLY);
}
static void scr_stub_out(void)
{
    char v[16];
    if (GetVar((STRPTR)SCR_VAR, v, sizeof v, 0) > 0) {
        APTR old = (APTR)strtoul(v, NULL, 10);
        if (old) SetFunction((struct Library *)GfxBase, LVO_SetChipRev, old);
        DeleteVar((STRPTR)SCR_VAR, GVF_GLOBAL_ONLY);
    }
    if (GetVar((STRPTR)STUB_VAR, v, sizeof v, 0) > 0) {
        APTR code = (APTR)strtoul(v, NULL, 10);
        if (code) FreeMem(code, 16);
        DeleteVar((STRPTR)STUB_VAR, GVF_GLOBAL_ONLY);
    }
}

/* ---- the second megabyte on a 1 MB machine -------------------------------
   The game never runs in real chip RAM: the sandbox maps 2 MB of the Pi's
   memory at address 0 whatever the machine has, and the kernel copies only
   as much real chip RAM in and out as there is. What stops a 2 MB game on a
   1 MB machine is Exec's memory list: WHDLoad reads it and refuses. So while
   the sandbox is on, lend Exec the range from the real top up to 2 MB as a
   second chip-memory header, at a lower priority than the real one so the OS
   itself only touches it when the real megabyte is full, and take it back
   before the mapping goes. MaxLocMem follows, because some code reads the
   chip size from there rather than from the list. */
#define LEND_NAME "AGA-PISTORM chip"
#define MAXLOC_VAR "AGA_MAXLOCMEM"
extern struct ExecBase *SysBase;
/* Found by what it is, not by its name: AddMemList keeps the NAME POINTER,
   and a string inside this program is gone the moment SANDBOX ON exits -
   the OFF run would be comparing against freed memory, and I am not fucking
   playing that game. So the name lives in allocated public memory, and the
   search goes by range: the one chip header that starts at or above 512 KB
   and ends at 2 MB, at my priority. */
static struct MemHeader *lend_find(void)
{
    for (struct Node *n = SysBase->MemList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct MemHeader *mh = (struct MemHeader *)n;
        if ((mh->mh_Attributes & MEMF_CHIP) && (ULONG)mh->mh_Lower >= 0x80000 &&
            (ULONG)mh->mh_Upper == 0x200000 && n->ln_Pri == -10) return mh;
    }
    return NULL;
}
/* The after-game diagnostics - the counter dump, the pointer probe, the chip
   RAM lending notes - go to AGA-diag.log only when ENV:AGA_DIAGLOG is 1.
   Someone who just plays needs none of it and waits for it on every F10;
   a test machine sets it with `SetEnv SAVE AGA_DIAGLOG 1`. */
int aga_diaglog(void)
{
    char v[4];
    return GetVar((STRPTR)"AGA_DIAGLOG", (STRPTR)v, sizeof v, 0) > 0 && v[0] == '1';
}

/* One fixed place for the diagnostics, whatever volumes a machine has */
#define DIAG_LOG  "C:AGA-diag.log"
#define DIAG_PROF "C:AGA-prof.bin"
static void lend_log(const char *line);
/* A game that hangs never reaches the exit that writes AGA-diag.log, and
   the soft reset that gets out of it takes the counters away - except that
   the kernel keeps that session across the reset. agaboot MENU runs at
   every boot, so it appends the kept session here: the hang logs itself.
   Only with my kernel (the diagnostics magic), only when a session was
   kept, and only if it ran frames - a plain desktop reset adds nothing. */
static void log_kept_session(void)
{
    UWORD a, b, kept, hi, lo;
    if (!aga_diaglog()) return;
    Disable();
    DIAG_SEL_W = 0x200;  a = DIAG_DAT_W; b = DIAG_DAT_W;     /* "AGAD" magic */
    DIAG_SEL_W = 0x1600; kept = DIAG_DAT_W;                  /* a session was kept */
    DIAG_SEL_W = 0x1601;                                     /* read the kept one */
    DIAG_SEL_W = 0;      hi = DIAG_DAT_W; lo = DIAG_DAT_W;   /* its frame count */
    DIAG_SEL_W = 0x1602;
    Enable();
    if (a != 0xA6A1 || b != 0xD1A6 || kept != 1 || (hi == 0 && lo == 0)) return;
    lend_log("=== the session before the last soft reset (it did not exit) ===\n");
    Execute((STRPTR)"C:agastat PREV >>" DIAG_LOG, 0, 0);
    Execute((STRPTR)"C:agastat PREV TRACE >>" DIAG_LOG, 0, 0);
    Execute((STRPTR)"C:agastat PREVPROF " DIAG_PROF, 0, 0);
}

static void lend_log(const char *line)
{
    if (!aga_diaglog()) return;
    BPTR f = Open((STRPTR)DIAG_LOG, MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    Write(f, (APTR)line, strlen(line));
    Close(f);
}
static void chip_lend(int on)
{
    char v[16];
    if (on) {
        if (lend_find()) return;
        ULONG top = 0;
        Forbid();
        for (struct Node *n = SysBase->MemList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
            struct MemHeader *mh = (struct MemHeader *)n;
            if ((mh->mh_Attributes & MEMF_CHIP) && (ULONG)mh->mh_Upper > top) top = (ULONG)mh->mh_Upper;
        }
        Permit();
        if (top == 0 || top >= 0x200000) return;          /* a 2 MB machine: nothing to lend */
        sprintf(v, "%lu", (unsigned long)SysBase->MaxLocMem);
        SetVar((STRPTR)MAXLOC_VAR, v, -1, GVF_GLOBAL_ONLY);
        char *name = AllocMem(32, MEMF_PUBLIC | MEMF_CLEAR);    /* outlives this program; freed at OFF */
        if (name) strcpy(name, LEND_NAME);
        AddMemList(0x200000 - top, MEMF_CHIP | MEMF_PUBLIC | MEMF_LOCAL | MEMF_24BITDMA, -10,
                   (APTR)top, (STRPTR)name);
        SysBase->MaxLocMem = 0x200000;
        { char l[120];
          sprintf(l, "AGALEND on: %lu KB above $%06lx, chip avail now %lu KB\n",
                  (unsigned long)((0x200000 - top) >> 10), (unsigned long)top,
                  (unsigned long)(AvailMem(MEMF_CHIP) >> 10));
          lend_log(l); PutStr(l); }
    } else {
        struct MemHeader *mh = lend_find();
        if (!mh) { lend_log("AGALEND off: nothing lent\n"); return; }
        ULONG size = (ULONG)mh->mh_Upper - (ULONG)mh->mh_Lower;
        char *name = (char *)mh->mh_Node.ln_Name;
        int waited = 0;
        /* WHDLoad gives everything back before its cleanup script runs, but
           give a late FreeMem a moment before pulling the floor away. */
        while (mh->mh_Free + sizeof(struct MemHeader) < size && waited < 25) { Delay(10); waited++; }
        if (mh->mh_Free + sizeof(struct MemHeader) < size)
            Printf("agaboot: WARNING %ld bytes of the lent chip RAM still in use, unlinking anyway\n",
                   (long)(size - mh->mh_Free));
        Forbid();
        Remove((struct Node *)mh);
        Permit();
        if (name) FreeMem(name, 32);
        { char l[140];
          sprintf(l, "AGALEND off: %lu KB unlinked, %lu bytes were still in use, chip avail now %lu KB\n",
                  (unsigned long)(size >> 10), (unsigned long)(size - mh->mh_Free),
                  (unsigned long)(AvailMem(MEMF_CHIP) >> 10));
          lend_log(l); PutStr(l); }
        if (GetVar((STRPTR)MAXLOC_VAR, v, sizeof v, 0) > 0) {
            SysBase->MaxLocMem = strtoul(v, NULL, 10);
            DeleteVar((STRPTR)MAXLOC_VAR, GVF_GLOBAL_ONLY);
        }
    }
}

/* What to send at sandbox entry: the preference, or the game if it is AUTO. */
static int ecs_for_this_game(void)
{
    int pref = ecs_pref();
    return (pref < 0) ? game_is_ecs() : pref;
}

static int sandbox(const char *what)
{
    if (!stricmp(what, "?")) {
        char v[16];
        int on = GetVar((STRPTR)CHIPREV_VAR, v, sizeof v, 0) > 0;
        Printf("sandbox %s (chipset reports %s), AGA for games: %s, emulated: %s\n",
               (ULONG)(on ? "ON" : "OFF"), (ULONG)(aga_active() ? "AGA" : "ECS"),
               (ULONG)(armed() ? "ARMED" : "off"),
               (ULONG)(ecs_pref() < 0 ? "per game" : ecs_pref() ? "ECS Denise" : "AGA"));
        return on ? 5 : 0;
    }
    if (!stricmp(what, "AUTO")) {
        /* what the WHDLoad hook calls. Never fails: WHDLoad aborts the whole
           game on a non-zero return from ExecuteStartup, and "not armed" is a
           normal state, not an error. */
        if (!armed()) return 0;
        /* System wide is up - the desktop on the emulated chipset, in its own
           mode. A game still gets its own entry (game mode, a reset chipset,
           its own picture), so leave it first; until 1.111 the game ran under
           System wide's mode instead, and its cleanup switched everything off
           anyway. In A1kmode that cleanup brings System wide back. */
        if (aga_active()) {
            sandbox("OFF");
            if (a1k_mode()) SetVar((STRPTR)A1K_GAME_VAR, (STRPTR)"1", -1, GVF_GLOBAL_ONLY);
        }
        /* Game mode silences the CIA-B interrupt sources AmigaOS leaves
           running, at the moment WHDLoad takes the machine. Left alone they
           hold the CIA's /INT line asserted and the 68k drowns in level 6.
           Kept switchable so the old behaviour is one SetEnv away:
             SetEnv AGA_GAMEMODE 0   change nothing at takeover
             SetEnv AGA_GAMEMODE 1   silence them (the default) */
        {
            char v[16];
            int gm = 1;
            if (GetVar((STRPTR)GAMEMODE_VAR, v, sizeof v, 0) > 0 && v[0] == '0') gm = 0;
            enter_magic = gm ? MAGIC_SANDBOX_GAME : MAGIC_SANDBOX_ON;
        }
        /* which game: its own picture, if it has one, and RTG Image's
           "Settings for" offers it afterwards */
        if (game_key(entry_game, sizeof entry_game))
            SetVar((STRPTR)LASTGAME_VAR, (STRPTR)entry_game, -1, GVF_GLOBAL_ONLY);
        return sandbox("ON");
    }
    if (stricmp(what, "ON") && stricmp(what, "OFF")) return 5;
    int on = !stricmp(what, "ON");
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
    if (!GfxBase) return 20;
    if (on) {
        if (aga_active()) { PutStr("agaboot: AGA chipset already active, nothing to do\n"); CloseLibrary((struct Library *)GfxBase); return 0; }
        /* graphics.library says AGA FIRST, and SetChipRev cannot switch it for
           real, before the slow part below (the chip RAM ring, the prefs, the
           slaves, the entry itself). WHDLoad does not wait for this command -
           ExecuteStartup runs in the background - and checks the slave's
           chipset at once: on AmiKit (WHDLoad 19.1, Kickstart 3.2, a Pi 3) an
           AGA game was refused, "This game requires AGA chipset", while the
           entry was still under way; the sandbox came up seconds later, after
           WHDLoad's own cleanup had run (2026-10-16). Entered first by hand,
           the same game played. The original bits are kept as before, and an
           entry that fails puts both back. */
        if (aga_diaglog()) ptr_probe("entry-before-poke");
        char crb[16];
        sprintf(crb, "%lu", (unsigned long)GfxBase->ChipRevBits0);
        SetVar((STRPTR)CHIPREV_VAR, crb, -1, GVF_GLOBAL_ONLY);
        /* the full set, HR_AGNUS and HR_DENISE included: WHDLoad tests for
           SETCHIPREV_AA, and with only GFXF_AA answered it refuses the slave
           ("needs AGA chipset", 2026-09-13, once SetChipRev was stubbed) */
        GfxBase->ChipRevBits0 |= SETCHIPREV_AA;
        scr_stub_in();
        /* Paula can only DMA from chip RAM, so the emulated mix has to be
           staged there - and in sandbox mode AmigaOS owns all of it. Allocate
           the ring properly and tell Emu68 where it is, rather than letting the
           ARM side write to an address it assumed was free. It was not: the
           first guess landed on the exception vectors, the second on memory
           WHDLoad PRELOAD had just handed to the game. If this fails Emu68
           simply plays no audio - it will not fall back to a fixed address. */
        {
            APTR ring = AllocMem(RING_BYTES, MEMF_CHIP | MEMF_PUBLIC | MEMF_CLEAR);
            if (ring) {
                char v[16];
                sprintf(v, "%lu", (unsigned long)ring);
                SetVar((STRPTR)RING_VAR, v, -1, GVF_GLOBAL_ONLY);
                AGA_RING = (ULONG)ring;
            } else {
                PutStr("agaboot: no chip RAM for the audio ring, continuing without sound\n");
            }
        }
        /* HAM6 on the RGB port (Chipset menu - RGB port): the same arrangement
           as the ring, 162 KB of it (rgbport.c) */
        rgbport_entry();
        /* HDMI sound as set, in case the kernel lost it (a no-op when it has it) */
        hdmisound_send(hdmisound_pref());
        hdmifilter_send(hdmifilter_pref());
        hdmisync_send(hdmisync_pref());       /* HDMI sync, as RTG Image set it */
        {   /* the Sound window's volume, stereo and swap (sound.c) */
            struct sound_prefs sp;
            sound_load(&sp);
            sound_send_mix(&sp);
        }
        struct aga_picture pic;
        picture_for_entry(&pic);
        /* Which chipset this game gets - judged here, not inside Disable():
           it reads a variable and the slaves in the game's drawer */
        int forced_ecs = ecs_pref() > 0, ecs = forced_ecs || ecs_for_this_game();
        CacheClearU();   /* flush the 68k's caches HERE, in my own context - not from inside Emu68's abort handler */
        Disable();
        /* ... sent BEFORE the enter word: entering resets the virtual
           chipset, and the core deliberately carries the mode across that
           reset. Ignored by a stock Emu68, like the rest. $5E03 is the
           user's "always ECS": the kernel then never switches the game to
           AGA, whatever it draws (ecs_says_aga in the core). $5E01 goes
           first, so a kernel from before $5E03 still gives ECS. */
        AGA_CTRL = (UWORD)(0x5E00 | (ecs ? 1 : 0));
        if (forced_ecs) AGA_CTRL = 0x5E03;
        picture_words(&pic);
        AGA_CTRL = enter_magic;
        Enable();
        if (!aga_active()) {
            /* Deliberately NOT an error. This runs from S:WHDLoad-Startup,
               and WHDLoad aborts the entire game on any non-zero return from
               ExecuteStartup. A missing sandbox is a no-op - the game just
               runs in ECS as it always did - so failing here breaks WHDLoad
               for no benefit. Only genuine faults return non-zero. */
            free_ring();
            rgbport_leave();
            scr_stub_out();                      /* the early AGA answer goes again */
            GfxBase->ChipRevBits0 = (UBYTE)atoi(crb);
            DeleteVar((STRPTR)CHIPREV_VAR, GVF_GLOBAL_ONLY);
            PutStr("agaboot: sandbox not available (stock Emu68, or aga.nosandbox);"
                   " continuing in ECS\n");
            CloseLibrary((struct Library *)GfxBase);
            return 0;   /* see note above: must not fail */
        }
        chip_lend(1);
        display_on(GfxBase);
        PutStr("agaboot: sandbox ON, virtual AGA chipset active\n");
    } else {
        char v[16];
        chip_lend(0);
        scr_stub_out();
        if (GetVar((STRPTR)CHIPREV_VAR, v, sizeof v, 0) > 0) {
            GfxBase->ChipRevBits0 = (UBYTE)atoi(v);
            DeleteVar((STRPTR)CHIPREV_VAR, GVF_GLOBAL_ONLY);
        }
        CacheClearU();   /* dirty lines reach chip_va through the redirect before the leave copies it back */
        Disable();
        AGA_CTRL = MAGIC_SANDBOX_OFF;
        Enable();
        free_ring();     /* after MAGIC_SANDBOX_OFF: the ARM has dropped its pointer */
        rgbport_leave(); /* likewise: core 1 stopped and the real copper was off before chip RAM went back */
        /* Put the OS back on the REAL chipset, the mirror of the display_on()
           above. Leaving the sandbox replays the virtual chipset's last
           DMACON to the real chips, and that is the GAME's - a game that
           quits with sprite DMA off hands the desktop a chipset with no
           sprites, and the mouse pointer IS sprite 0. Reported from the
           machine as "no mouse cursor when i return to workbench". Cheap,
           idempotent, and the same call the entry path already trusts. */
        if (!aga_active()) display_on(GfxBase);
        if (aga_diaglog()) ptr_probe("exit-after-restore");
        /* A snapshot of the Pi's counters after every game, appended to
           C:AGA-diag.log. The fault being chased - no mouse pointer after
           an AGA game - is exactly the one that makes reading them by hand
           hard, because opening a Shell without a pointer is the problem.
           Appending lets an ECS run and an AGA run be compared side by side,
           which is the comparison that matters: only AGA games do it.
           A few KB per game; delete the file to reset it. Never RAM:, since
           the point is to survive a power cycle. */
        if (aga_diaglog())
            Execute((STRPTR)"C:agastat >>" DIAG_LOG, 0, 0);
        PutStr(aga_active() ? "agaboot: sandbox still active?\n" : "agaboot: sandbox OFF, real chipset active\n");
    }
    CloseLibrary((struct Library *)GfxBase);
    return 0;
}

/* System wide on: the menu's, A1kmode's at boot, and back after a game */
static int systemwide_on(void)
{
    enter_magic = MAGIC_SANDBOX_ON;
    zoom_default = 2;                    /* the desktop is a standard screen */
    return sandbox("ON");
}

/* ---- installer support (setup.c does the text edits) ------------------------
 *
 *   agaboot CHECK [partition:]      pre-flight for the Installer script: sets
 *                                   ENV:AGAPISTORM_CLASSIC / _PS16 / _PI4 /
 *                                   _PI3 / _WRITABLE / _CONFIG / _CONFIG16 /
 *                                   _CFGOPEN / _CHIP2M / _NTSC / _UNICAM / _PALOVL /
 *                                   _VCMON to 1, 0 or ?, _BOOT / _CFGFILE /
 *                                   _OVLDIR / _KDIR to the boot partition and
 *                                   the names on it, and prints what it found;
 *                                   every file is left as it was. A partition
 *                                   (the one the user picked when none was
 *                                   found) is taken if config.txt is on it
 *   agaboot INSTALL HOOKS           S:WHDLoad-Startup, S:WHDLoad-Cleanup and
 *                                   S:User-Startup get their blocks
 *   agaboot INSTALL CONFIG [PS16] [file]   config.txt boots the AGA kernels:
 *                                   both boards' sections are set, PS16 says
 *                                   this one is a PiStorm16 - its section
 *                                   must exist (default EMU68:config.txt)
 *   agaboot INSTALL OVERLAYS from to   the overlays the boot partition is
 *                                   missing, whatever case its names are in
 *   agaboot UNINSTALL HOOKS|CONFIG [file]|MENU   undo each of those (CONFIG
 *                                   puts back both boards' kernel lines)
 *
 * Every edit backs the file up to <name>.pre-aga the first time and is safe to
 * repeat: running the installer twice changes nothing the second time. */
extern const char *const setup_whd_on, *const setup_whd_off, *const setup_user_startup;
char *setup_script_set(const char *text, const char *block, int where);
char *setup_script_remove(const char *text);
char *setup_config(const char *text, int install, unsigned opts, int *err);
int setup_has_section(const char *text, int ps16);
int setup_config_open(const char *text);
int setup_config_has(const char *text, const char *s);
int setup_config_hdmi_state(const char *text);
char *setup_config_hdmi(const char *text, int on, int *err);
int setup_config_rate_state(const char *text);
char *setup_config_rate(const char *text, int rate, int *err);
int setup_config_after_block(const char *text, const char *s);
char *setup_read(const char *path);
int setup_write(const char *path, const char *text);
int setup_backup(const char *path, const char *text);
void setup_board(int ps16);
const char *setup_section(void);
char *setup_whdprefs(const char *text, int install, int *other);
/* the low speed option (setup.c: OPT_SLOW, LOWSPEED_WORD, the note BLK_SLOW) */
#define OPT_SLOW      128
#define LOWSPEED_WORD "emmc.low_speed"
int setup_block_slow(const char *text);
int setup_card_slow(const char *text);
void setup_cmdline_file(const char *text, char *name, int len);
int setup_cmdline_has(const char *text, const char *tok);
char *setup_cmdline_edit(const char *text, const char *tok, int add);

#include <dos/dostags.h>
#include <dos/exall.h>          /* ExAll in find_ci: the names as List and ls see them */
#include <dos/dosextens.h>
#include <dos/filehandler.h>    /* FileSysStartupMsg: which device a partition is on */
#define SETUP_OK    "  %-24s %s\n"

static int exists_quiet(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) UnLock(l);
    return l != 0;
}

/* `name` in the directory `dir` ("Emu68:", "Emu68:overlays"), in whatever
 * case the disk has it: the path goes to `out`, and 1 if it is there - or,
 * when it is not, the path to create it under. FAT handlers differ. fat95
 * finds CONFIG.TXT when asked for config.txt, but a tester's MOUNTFAT
 * matches the case exactly (2026-10-02), and a config.txt written without a
 * long name is stored, and listed, as CONFIG.TXT. Asking only for
 * "config.txt" found no boot partition there, and writing it would have made
 * a second file beside the one the Pi reads. So the exact name first (all
 * that fat95 ever needs), then the directory is read for it in another case. */
/* The directory is read with ExAll first, as List, ls and the file requester
 * read it: ExAll hands over the handler's own names. Through ExNext, dos
 * converts each name from a BCPL string, and a handler that already gives
 * C strings comes out with its names mangled - the same tester's MOUNTFAT
 * still failed with ExNext alone (test2). ExNext stays as the fallback, for
 * a handler that ExAll does not reach. `probe` prints what is seen. */
static int probe_out;                       /* agaboot BOOTPROBE */
static int scan_dir_ci(const char *dir, const char *name, char *out, size_t size)
{
    int found = 0, seen = 0;
    BPTR l = Lock((STRPTR)dir, ACCESS_READ);
    if (!l) {
        if (probe_out) Printf("    Lock(%s) failed, IoErr %ld\n", (ULONG)dir, IoErr());
        return 0;
    }
    static ULONG buf[512];                  /* 2 KB of ExAllData */
    struct ExAllControl *eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    if (eac) {
        eac->eac_LastKey = 0;
        int more;
        do {
            more = ExAll(l, (struct ExAllData *)buf, sizeof buf, ED_NAME, eac);
            if (!more && IoErr() != ERROR_NO_MORE_ENTRIES && !eac->eac_Entries) {
                if (probe_out) Printf("    ExAll failed, IoErr %ld\n", IoErr());
                break;
            }
            for (struct ExAllData *ed = eac->eac_Entries ? (struct ExAllData *)buf : NULL; ed; ed = ed->ed_Next) {
                seen++;
                if (probe_out && seen <= 40) Printf("      [%s]\n", (ULONG)ed->ed_Name);
                if (!found && !stricmp((char *)ed->ed_Name, name)) {
                    strncpy(out, dir, size - 1);
                    out[size - 1] = 0;
                    AddPart((STRPTR)out, (STRPTR)ed->ed_Name, size);
                    found = 1;
                }
            }
        } while (more && !found);
        if (more) ExAllEnd(l, (struct ExAllData *)buf, sizeof buf, ED_NAME, eac);
        FreeDosObject(DOS_EXALLCONTROL, eac);
        if (probe_out) Printf("    ExAll: %ld names, %s\n", (long)seen, (ULONG)(found ? out : "no match"));
    }
    if (!found) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        seen = 0;
        if (fib && Examine(l, fib))
            while (!found && ExNext(l, fib)) {
                seen++;
                if (probe_out && seen <= 40) Printf("      <%s>\n", (ULONG)fib->fib_FileName);
                if (!stricmp((char *)fib->fib_FileName, name)) {
                    strncpy(out, dir, size - 1);
                    out[size - 1] = 0;
                    AddPart((STRPTR)out, (STRPTR)fib->fib_FileName, size);
                    found = 1;
                }
            }
        else if (probe_out) Printf("    Examine failed, IoErr %ld\n", IoErr());
        if (probe_out) Printf("    ExNext: %ld names, %s\n", (long)seen, (ULONG)(found ? out : "no match"));
        if (fib) FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return found;
}

static int find_ci(const char *dir, const char *name, char *out, size_t size)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    int found = 0;
    strncpy(out, dir, size - 1);
    out[size - 1] = 0;
    AddPart((STRPTR)out, (STRPTR)name, size);
    if (exists_quiet(out)) found = 1;
    else {
        if (probe_out) Printf("    Lock(%s) failed, IoErr %ld - reading the directory\n", (ULONG)out, IoErr());
        char keep[256];
        strncpy(keep, out, sizeof keep - 1);
        keep[sizeof keep - 1] = 0;
        found = scan_dir_ci(dir, name, out, size);
        if (!found) { strncpy(out, keep, size - 1); out[size - 1] = 0; }   /* the name to create */
    }
    if (probe_out) Printf("  %-28s %s\n", (ULONG)name, (ULONG)(found ? out : "NOT FOUND"));
    me->pr_WindowPtr = win;
    return found;
}

/* A partition of the SD card mounted to start on first use - fat95 from
 * SYS:Storage/DOSDrivers on a plain 3.2 card - is a device without a volume
 * until something asks for it, so find_boot() and CHECK said "boot partition
 * NOT FOUND" right after mounting it (the A500's 3.2 card, 1.112). Every
 * device on Emu68's SD driver whose handler has not started yet is asked
 * once, which starts it (from RGB2RTG 9bef928). A handler already running
 * is left alone. */
static void wake_sd_partitions(void)
{
    static char devs[16][34];                     /* off the Installer's small stack */
    int n = 0;
    struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES)) && n < 16) {
        if (dl->dol_Task) continue;               /* started already */
        /* dol_Startup is a FileSysStartupMsg only for a disk partition: other
           handlers keep a small number or a string there */
        BPTR st = dl->dol_misc.dol_handler.dol_Startup;
        struct FileSysStartupMsg *fssm = (struct FileSysStartupMsg *)BADDR(st);
        if ((ULONG)st < 64 || !TypeOfMem(fssm)) continue;
        UBYTE *d = (UBYTE *)BADDR(fssm->fssm_Device);
        if (!d || !TypeOfMem(d)) continue;
        char dev[40];
        int len = d[0] < 39 ? d[0] : 39;
        memcpy(dev, d + 1, len);
        dev[len] = 0;
        /* brcm-emmc.device (Pi 4), brcm-sdhc.device (Pi 3) */
        if (!strstr(dev, "emmc") && !strstr(dev, "sdhc")) continue;
        UBYTE *b = (UBYTE *)BADDR(dl->dol_Name);
        len = b[0] < 30 ? b[0] : 30;
        memcpy(devs[n], b + 1, len);
        devs[n][len] = ':';
        devs[n][len + 1] = 0;
        n++;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    /* only now, with the DOS list let go: the Lock starts the handler, and the
       handler adds its volume to that list */
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;                  /* an empty slot: no requester */
    for (int i = 0; i < n; i++) {
        if (probe_out) Printf("waking %s\n", (ULONG)devs[i]);
        BPTR l = Lock((STRPTR)devs[i], ACCESS_READ);
        if (l) UnLock(l);
    }
    me->pr_WindowPtr = win;
}

/* The Emu68 boot partition, as "<volume>:", or "" if none is mounted.
 *
 * On CaffeineOS it is called EMU68:, but that is only the FAT partition's
 * LABEL - a card labelled BOOT or NO NAME mounts under that name instead.
 * So look for what makes it the boot partition: config.txt and the Pi
 * firmware. CaffeineOS also keeps its kernels in a kernel directory, and a
 * volume with one wins. A card set up by hand has its kernel in the root, and
 * asking for the directory too told such a user (AmigaOS 3.2.3, 2026-09-30)
 * that the partition they were looking at did not exist. Every mounted volume
 * is asked, not the first 16. Only MOUNTED volumes are asked, so no "insert
 * volume" requester can appear; requesters are off anyway. The SD card's
 * partitions that have not started yet are woken first. */
static char boot_vol[40];
static const char *find_boot(void)
{
    if (boot_vol[0]) return boot_vol;
    wake_sd_partitions();
    /* The names are copied out first: nothing may be looked up on a volume
       while the DOS list is locked. Two passes under the one lock, so the
       count cannot change in between. */
    struct DosList *head = LockDosList(LDF_VOLUMES | LDF_READ), *dl;
    int n = 0;
    for (dl = head; (dl = NextDosEntry(dl, LDF_VOLUMES)); ) n++;
    char (*names)[32] = n ? malloc((size_t)n * sizeof *names) : NULL;
    int got = 0;
    for (dl = head; names && got < n && (dl = NextDosEntry(dl, LDF_VOLUMES)); got++) {
        UBYTE *b = (UBYTE *)BADDR(dl->dol_Name);
        int len = b[0] < 30 ? b[0] : 30;
        memcpy(names[got], b + 1, len);
        names[got][len] = ':';
        names[got][len + 1] = 0;
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    int pick = -1;                     /* the first boot partition found */
    for (int i = 0; i < got; i++) {
        char p[64];                                 /* any case: see find_ci */
        if (probe_out) Printf("%s\n", (ULONG)names[i]);
        if (!find_ci(names[i], "config.txt", p, sizeof p)) continue;
        if (!find_ci(names[i], "start4.elf", p, sizeof p) && !find_ci(names[i], "start.elf", p, sizeof p)) continue;
        if (find_ci(names[i], "kernel", p, sizeof p)) { pick = i; break; }   /* CaffeineOS's: take it */
        if (pick < 0) pick = i;
    }
    if (pick >= 0) strcpy(boot_vol, names[pick]);
    me->pr_WindowPtr = win;
    free(names);
    return boot_vol;
}

/* The partition the user picked in the Installer's requester because
 * find_boot() found none. A pick only has to prove one thing: config.txt is
 * on it. It must be a partition itself ("Emu68:"), not a drawer on one - the
 * Pi reads config.txt from the root, so a copy in a drawer does nothing.
 * Kept in boot_vol, so config_txt() and the rest use it too. */
static const char *pick_boot(const char *path)
{
    size_t n = strlen(path);
    boot_vol[0] = 0;
    if (n < 2 || n > sizeof boot_vol - 1 || path[n - 1] != ':') return boot_vol;
    char p[64];
    if (find_ci(path, "config.txt", p, sizeof p)) strcpy(boot_vol, path);
    return boot_vol;
}

/* config.txt under the name the boot partition has it (CONFIG.TXT on some);
   "" when no boot partition is mounted. Not "EMU68:config.txt" then: that is
   only CaffeineOS's label, and asking for a volume that is not there would
   bring up an "insert volume" requester on every other system. */
static const char *config_txt(void)
{
    static char p[64];
    p[0] = 0;
    if (find_boot()[0]) find_ci(find_boot(), "config.txt", p, sizeof p);
    return p;
}

/* DISMOUNT [volume:]: the boot partition put away safely, like a safe remove,
 * as the installers' last step before the power cycle - a tester's idea
 * (2026-10-02). Its file system writes out whatever it still holds
 * (ACTION_FLUSH), lets go of the disk (Inhibit: where a FAT file system keeps
 * FAT's clean-shutdown flag, it sets it here), and its device leaves the DOS
 * list, as Assign DISMOUNT does - which on its own writes nothing out. It is
 * back at the next boot, or when it is mounted again; nothing but installing
 * needs it in between. Only written out, and left mounted, when this process
 * stands on it (an installer started from the partition itself, where
 * SD-Setup puts it) or the file system will not let go.
 * ENV:AGAPISTORM_PUTAWAY says which: 1 put away, 0 not. */
static int dismount(const char *vol)
{
    SetVar((STRPTR)"AGAPISTORM_PUTAWAY", (STRPTR)"0", -1, GVF_GLOBAL_ONLY);
    if (!vol || !vol[0]) vol = find_boot();
    if (!vol[0]) { PutStr("No Emu68 boot partition is mounted.\n"); return 5; }
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;                  /* no "insert volume" requester */
    struct DevProc *dp = GetDeviceProc((STRPTR)vol, NULL);
    struct MsgPort *port = dp ? dp->dvp_Port : NULL;
    if (dp) FreeDeviceProc(dp);
    me->pr_WindowPtr = win;
    if (!port) { Printf("%s is not mounted.\n", (ULONG)vol); return 5; }

    char dev[40] = "";                            /* the device it is mounted as */
    struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES)))
        if (dl->dol_Task == port) {
            UBYTE *b = (UBYTE *)BADDR(dl->dol_Name);
            int len = b[0] < 38 ? b[0] : 38;
            memcpy(dev, b + 1, len);
            dev[len] = ':';
            dev[len + 1] = 0;
            break;
        }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    DoPkt(port, ACTION_FLUSH, 0, 0, 0, 0, 0);
    BPTR cd = me->pr_CurrentDir, home = me->pr_HomeDir;
    if ((cd && ((struct FileLock *)BADDR(cd))->fl_Task == port) ||
        (home && ((struct FileLock *)BADDR(home))->fl_Task == port)) {
        Printf("%s written out; left mounted, as this runs from it.\n", (ULONG)vol);
        return 0;
    }
    if (!dev[0] || !Inhibit((STRPTR)dev, DOSTRUE)) {
        Printf("%s written out; left mounted, as its file system will not let go.\n", (ULONG)vol);
        return 0;
    }
    int same = !stricmp(dev, vol);                /* PISTORM: mounted as PISTORM: */
    dev[strlen(dev) - 1] = 0;                     /* FindDosEntry: no colon */
    dl = LockDosList(LDF_DEVICES | LDF_WRITE);
    struct DosList *d = FindDosEntry(dl, (STRPTR)dev, LDF_DEVICES);
    if (d && d->dol_Task == port) RemDosEntry(d);
    UnLockDosList(LDF_DEVICES | LDF_WRITE);
    SetVar((STRPTR)"AGAPISTORM_PUTAWAY", (STRPTR)"1", -1, GVF_GLOBAL_ONLY);
    if (same) Printf("%s put away safely. It is back at the next boot.\n", (ULONG)vol);
    else Printf("%s (%s:) put away safely. It is back at the next boot.\n", (ULONG)vol, (ULONG)dev);
    return 0;
}

/* INSTALL OVERLAYS <from> <to>: each file of <from> that <to> does not have,
   in any case - so an OVERLAYS drawer with EMMC.DTBO in it gets no second
   copy beside it. <to> is made when it is missing. */
static int install_overlays(const char *from, const char *to)
{
    static char src[256], dst[256];              /* off the Installer's small stack */
    int rc = 0, added = 0;
    BPTR d = Lock((STRPTR)to, ACCESS_READ);
    if (!d) d = CreateDir((STRPTR)to);
    if (!d) { Printf(SETUP_OK, (ULONG)to, (ULONG)"CANNOT MAKE"); return 10; }
    UnLock(d);
    BPTR l = Lock((STRPTR)from, ACCESS_READ);
    struct FileInfoBlock *fib = l ? AllocDosObject(DOS_FIB, NULL) : NULL;
    if (!fib || !Examine(l, fib)) rc = 10;
    else while (ExNext(l, fib)) {
        if (fib->fib_DirEntryType > 0) continue;
        if (find_ci(to, (char *)fib->fib_FileName, dst, sizeof dst)) continue;   /* the card has it */
        strncpy(src, from, sizeof src - 1);
        src[sizeof src - 1] = 0;
        AddPart((STRPTR)src, (STRPTR)fib->fib_FileName, sizeof src);
        if (copy_file(src, dst)) added++;
        else { Printf(SETUP_OK, (ULONG)dst, (ULONG)"CANNOT COPY"); rc = 10; }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    if (l) UnLock(l);
    Printf("  %-24s %ld added\n", (ULONG)to, (long)added);
    return rc;
}

static int edit_script(const char *path, const char *block, int where, int install)
{
    char *t = setup_read(path);
    if (!t) { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT READ"); return 10; }
    char *n = install ? setup_script_set(t, block, where) : setup_script_remove(t);
    int rc = 0;
    if (!n) Printf(SETUP_OK, (ULONG)path, (ULONG)(install ? "already set up" : "nothing to remove"));
    else {
        if (install && !setup_backup(path, t)) { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT BACK UP - left alone"); rc = 10; }
        else if (!setup_write(path, n))        { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT WRITE"); rc = 10; }
        else Printf(SETUP_OK, (ULONG)path, (ULONG)(install ? "set up" : "restored"));
        free(n);
    }
    free(t);
    return rc;
}

/* A plain AmigaOS 3.x may lack the scripts CaffeineOS ships: INSTALL HOOKS
   CREATE makes the missing ones empty first - the two WHDLoad ones with the
   script bit, as WHDLoad runs them by name through ExecuteStartup/-Cleanup */
static void create_script(const char *path, int script)
{
    if (file_exists(path)) return;
    if (!setup_write(path, "")) { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT CREATE"); return; }
    if (script) SetProtection((STRPTR)path, FIBF_SCRIPT);
    Printf(SETUP_OK, (ULONG)path, (ULONG)"created");
}

/* S:WHDLoad.prefs: ExecuteStartup and ExecuteCleanup switched on (setup.c),
   the file made when there is none. Another command already there is the
   user's, left alone: rc 5 and a line saying what to do. */
static int edit_whdprefs(int install)
{
    const char *path = "S:WHDLoad.prefs";
    char *t = setup_read(path);
    if (!t) {
        if (!install) { Printf(SETUP_OK, (ULONG)path, (ULONG)"nothing to restore"); return 0; }
        t = malloc(1);
        if (!t) return 20;
        t[0] = 0;
    }
    int other = 0, rc = 0;
    char *n = setup_whdprefs(t, install, &other);
    if (!n) Printf(SETUP_OK, (ULONG)path, (ULONG)(install ? (other ? "left alone" : "already runs the scripts") : "nothing to restore"));
    else {
        if (install && t[0] && !setup_backup(path, t)) { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT BACK UP - left alone"); rc = 10; }
        else if (!setup_write(path, n))               { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT WRITE"); rc = 10; }
        else Printf(SETUP_OK, (ULONG)path, (ULONG)(install ? "runs the WHDLoad scripts now" : "restored"));
        free(n);
    }
    if (other) {
        Printf("  ExecuteStartup/Cleanup already runs another command (%s%s): add the\n"
               "  AGA-PISTORM block from S:WHDLoad-Startup/-Cleanup to that script yourself\n",
               (ULONG)(other & 1 ? "startup " : ""), (ULONG)(other & 2 ? "cleanup" : ""));
        if (!rc) rc = 5;
    }
    free(t);
    return rc;
}

/* MultibootOS's boot menu copies config_<os>.txt over config.txt at every
 * pick of a system, so a change made to config.txt alone is gone at the next
 * one: a MultibootOS card in the A500 came back on the stock kernel after an
 * install of 1.116 (2026-10-16). The system's own copy is the config_*.txt
 * that is config.txt byte for byte, as config.txt was before the change, and
 * it is changed with it. None the same (no MultibootOS, or config.txt changed
 * by hand since) or two (which system this is cannot be told): left alone,
 * and said so. RGB2RTG 0.76 does the same (install.c twin_find, EMM #2153). */
static struct {
    char path[96];
    int same, files;
} twin;

static void flush_vol(const char *vol);

static void twin_find(const char *cfgtext)
{
    static char names[16][40], p[96];   /* off the Installer's small stack */
    int n = 0;
    twin.path[0] = 0;
    twin.same = twin.files = 0;
    const char *boot = find_boot();
    BPTR l = boot[0] ? Lock((STRPTR)boot, ACCESS_READ) : 0;
    if (!l) return;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(l, fib))
        while (n < 16 && ExNext(l, fib)) {
            const char *s = (const char *)fib->fib_FileName;
            size_t k = strlen(s);
            if (fib->fib_DirEntryType < 0 && k > 11 && k < sizeof names[0] &&
                !strnicmp(s, "config_", 7) && !stricmp(s + k - 4, ".txt"))
                strcpy(names[n++], s);
        }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    twin.files = n;
    size_t len = strlen(cfgtext);
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s", boot);
        AddPart((STRPTR)p, (STRPTR)names[i], sizeof p);
        char *x = setup_read(p);
        if (x && strlen(x) == len && !memcmp(x, cfgtext, len) && !twin.same++)
            snprintf(twin.path, sizeof twin.path, "%s", p);
        free(x);
    }
    if (twin.same != 1) twin.path[0] = 0;
}

/* After config.txt was written: its twin as config.txt is now (old: as it
   was, for the twin's first .pre-aga copy). 1 written, 0 there is none,
   -1 it could not be written. */
static int twin_put(const char *old, const char *now)
{
    if (!twin.path[0]) return 0;
    if (!setup_backup(twin.path, old) || !setup_write(twin.path, now)) return -1;
    return 1;
}

/* The same, for the Installer's log: a line either way, 10 when the twin
   could not be written */
static int twin_write(const char *old, const char *now)
{
    int r = twin_put(old, now);
    if (r < 0) {
        Printf(SETUP_OK, (ULONG)twin.path, (ULONG)"CANNOT WRITE - MultibootOS's menu puts the old\n"
               "  config.txt back at the next pick of this system");
        return 10;
    }
    if (r) Printf(SETUP_OK, (ULONG)twin.path, (ULONG)"the same (MultibootOS: this system's own config.txt)");
    else if (twin.same > 1)
        Printf("  config.txt is the same as %ld config_*.txt files - which system this is\n"
               "  cannot be told, so they were left alone: MultibootOS's menu may undo this\n", (LONG)twin.same);
    else if (twin.files)
        PutStr("  config.txt is none of the config_*.txt files here - they were left alone:\n"
               "  on MultibootOS its menu puts config.txt back at the next pick\n");
    return 0;
}

/* ...and for a requester's text: what to add to msg, if anything */
static void twin_note(int r, char *msg, int len)
{
    size_t k = strlen(msg);
    if (r < 0)
        snprintf(msg + k, len - k, "%sBut %s could not\nbe written: MultibootOS's menu puts the\n"
                                   "old config.txt back at the next pick.", k ? "\n\n" : "", twin.path);
    else if (!r && twin.files)
        snprintf(msg + k, len - k, "%sOn MultibootOS: config.txt is %s of\nthe config_*.txt files, so the menu\n"
                                   "may undo this at the next pick.", k ? "\n\n" : "",
                 twin.same > 1 ? "more than one" : "none");
}

static int edit_config(const char *path, int install, unsigned opts, int ps16)
{
    char *t = setup_read(path);
    if (!t) { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT READ"); return 10; }
    twin_find(t);
    int err, rc = 0;
    setup_board(ps16);
    char *n = setup_config(t, install, opts, &err);
    if (err == 1) {
        Printf("  %-24s has no %s section - not a config this understands, left alone\n", (ULONG)path,
               (ULONG)(install ? setup_section() : "[gpio17=0] or [gpio24=1]"));
        rc = 10;
    } else if (err == 3) {
        /* rc 20, not 10: the Installer scripts tell this apart from a
           partition they cannot write, and say which it is */
        Printf("  %s has the AGA-PISTORM block start without its end line -\n"
               "  left alone, fix it by hand\n", (ULONG)path);
        rc = 20;
    } else if (err) {
        Printf(SETUP_OK, (ULONG)path, (ULONG)"OUT OF MEMORY - left alone");
        rc = 10;
    } else if (!n) Printf(SETUP_OK, (ULONG)path, (ULONG)(install ? "already boots the AGA kernel" : "nothing to restore"));
    else {
        if (install && !setup_backup(path, t)) { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT BACK UP - left alone"); rc = 10; }
        else if (!setup_write(path, n))        { Printf(SETUP_OK, (ULONG)path, (ULONG)"CANNOT WRITE (read-only?)"); rc = 10; }
        else {
            Printf(SETUP_OK, (ULONG)path, (ULONG)(install ? "boots the AGA kernel now" : "boots the previous kernel again"));
            rc = twin_write(t, n);
        }
        free(n);
    }
    free(t);
    return rc;
}

/* ---- the low speed option (1.116, RGB2RTG 924ba03) ----------------------------
   The Pi 4's SD card at 25 MHz instead of 50: some cards lost single writes at
   50 MHz, under Emu68 1.0.4 and 1.1 alike (RGB2RTG's user, two cheap cards; a
   SanDisk Ultra was clean). LOWSPEED_WORD goes into the command line file
   config.txt names - my kernel takes it from there (start.c), as Emu68 1.0.x
   did - and my block's note says it is mine (setup.c BLK_SLOW): Uninstall,
   and an install without the option, take it out again. Fucking SD cards. */

/* the command line file of config.txt at cfg, whose text is t */
static void cmdline_path(const char *cfg, const char *t, char *out, int len)
{
    char name[128];
    setup_cmdline_file(t, name, sizeof name);
    int d = (int)((const char *)FilePart((STRPTR)cfg) - cfg);
    snprintf(out, len, "%.*s%s", d, cfg, name);
}

/* 1 the word is mine, 2 the card runs at 25 MHz of its own (its emmc overlay
   line with low_speed, or the word in the command line that is not mine),
   0 neither */
static int lowspeed_state(const char *cfg, const char *t)
{
    if (setup_block_slow(t)) return 1;
    if (setup_card_slow(t)) return 2;
    static char cp[300];                             /* off the Installer's small stack */
    cmdline_path(cfg, t, cp, sizeof cp);
    char *c = setup_read(cp);
    int has = c && setup_cmdline_has(c, LOWSPEED_WORD);
    free(c);
    return has ? 2 : 0;
}

/* After config.txt was written (cfg; was = its state before): the command
   line as the block now says - the word in when my note is there, out
   when the note was mine and is gone. 10 when the file could not be written. */
static int lowspeed_sync(const char *cfg, int was)
{
    char *t = setup_read(cfg);
    if (!t) return 0;
    int ours = setup_block_slow(t);
    static char cp[300];
    cmdline_path(cfg, t, cp, sizeof cp);
    free(t);
    if (!ours && was != 1) return 0;                 /* never mine: nothing to do */
    char *c = setup_read(cp);
    const char *old = c ? c : "";
    char *n = setup_cmdline_edit(old, LOWSPEED_WORD, ours);
    int rc = 0;
    if (n) {
        if (ours && !setup_backup(cp, old))
            { Printf(SETUP_OK, (ULONG)cp, (ULONG)"CANNOT BACK UP - the SD card stays at 50 MHz"); rc = 10; }
        else if (!setup_write(cp, n))
            { Printf(SETUP_OK, (ULONG)cp, (ULONG)"CANNOT WRITE - the SD card speed is unchanged"); rc = 10; }
        else
            Printf(SETUP_OK, (ULONG)cp, (ULONG)(ours ? LOWSPEED_WORD ": the SD card at 25 MHz from the next power cycle"
                                                     : LOWSPEED_WORD " taken out: the SD card at 50 MHz again"));
        free(n);
    }
    free(c);
    return rc;
}

/* Run a command and keep the first line it prints (empty on failure). */
static void run_capture(const char *cmd, char *line, int size)
{
    line[0] = 0;
    BPTR out = Open((STRPTR)"T:agaboot.tmp", MODE_NEWFILE);
    if (!out) return;
    LONG r = SystemTags((STRPTR)cmd, SYS_Output, out, SYS_Input, 0, TAG_END);
    Close(out);
    if (r == 0) {
        BPTR f = Open((STRPTR)"T:agaboot.tmp", MODE_OLDFILE);
        if (f) {
            LONG n = Read(f, line, size - 1);
            Close(f);
            line[n > 0 ? n : 0] = 0;
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
        }
    }
    DeleteFile((STRPTR)"T:agaboot.tmp");
}

static void set_flag(const char *name, int v)     /* 1, 0, or -1 = "?" (could not tell) */
{
    SetVar((STRPTR)name, (STRPTR)(v < 0 ? "?" : v ? "1" : "0"), -1, GVF_GLOBAL_ONLY);
}

#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <proto/gadtools.h>
extern struct Library *GadToolsBase;               /* picture.c */

/* Chipset menu - About. A window, not a requester, because a requester has
   one font: the credit line goes in the screen's own font, bold, and the
   licence lines below it in topaz 8. Closes on its close gadget, a click
   inside or any key. The lines are GadTools text gadgets, drawn whenever
   Intuition draws the window's gadgets: text put into a window with Text()
   once it is open never shows on AmiKit, whose look patches paint the
   window's inside after that, while gadget labels come through (RGB2RTG
   0ec01a0, EMM #2134). */
static int about(void)
{
    static const char *const head[] = {
        "AGA-PISTORM " AGA_PISTORM_VERSION,
        "made by Astair86 for the Amiga community",
    };
    static const char *const body[] = {
        "An AGA chipset (Alice, Lisa and Paula) emulated on the Raspberry",
        "Pi, for WHDLoad games on the classic PiStorm and the PiStorm16.",
        "",
        "AGA-PISTORM and its Amiga tools are free software under the GNU",
        "General Public License, version 2 (Licenses/GPL-2.0.txt). The",
        "chipset contains code from UAE and WinUAE by Bernd Schmidt,",
        "Alessandro Bissacco, Toni Wilen and others (GPL).",
        "",
        "The kernel is Emu68 by Michal Schulz, Mozilla Public License 2.0",
        "(Licenses/MPL-2.0.txt), with the AGA-PISTORM chipset built in;",
        "as a whole it is distributed under the GPL version 2, as the",
        "MPL 2.0 allows. VideoCore.card is from Emu68-tools by Michal",
        "Schulz, MPL 2.0, with changes by Astair86. AGA-PISTORM's source",
        "is in Source/ in the package, with Emu68's and VideoCore.card's.",
        "Sound over HDMI follows Linux's VC4 HDMI audio driver (GPL-2.0;",
        "Broadcom, The Linux Foundation, Red Hat). HiFi sound's driver",
        "is written for AHI by Martin Blom.",
        "",
        "Thanks to the Emu68, PiStorm and CaffeineOS projects, and to",
        "Future Retro Fusion for the RGB port size and RUN ideas.",
        "",
        "Special thank you to katzazakis, FraBro, AngelClaww and Future",
        "Retro Fusion!",
        "",
        "This program comes with ABSOLUTELY NO WARRANTY.",
        "Use it at your own risk - back up your SD card first.",
    };
    enum { NH = sizeof head / sizeof head[0], NB = sizeof body / sizeof body[0], MARGIN = 16 };
    int rc = 20;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    struct Screen *scr = NULL;
    struct DrawInfo *dri = NULL;
    struct TextFont *big = NULL, *small = NULL;
    struct Window *win = NULL;
    struct Gadget *glist = NULL;
    APTR vi = NULL;
    int opened_gt = 0;
    if (!IntuitionBase || !GfxBase) goto out;
    /* GTTX_FrontPen and GTTX_Justification are V39 */
    if (!GadToolsBase && (GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39))) opened_gt = 1;
    if (!GadToolsBase) goto out;
    scr = LockPubScreen(NULL);
    if (!scr) goto out;
    dri = GetScreenDrawInfo(scr);
    vi = GetVisualInfo(scr, TAG_DONE);
    big = OpenFont(scr->Font);
    static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
    static struct TextAttr bold;
    bold = *scr->Font;
    bold.ta_Style |= FSF_BOLD;
    small = OpenFont(&topaz8);
    if (!dri || !vi || !big || !small) goto out;

    /* measure with a scratch RastPort, then size the window to the text */
    struct RastPort mrp;
    InitRastPort(&mrp);
    int w = 0;
    SetFont(&mrp, big);
    SetSoftStyle(&mrp, FSF_BOLD, AskSoftStyle(&mrp));
    for (int i = 0; i < NH; i++) { int t = TextLength(&mrp, (STRPTR)head[i], strlen(head[i])); if (t > w) w = t; }
    SetFont(&mrp, small);
    SetSoftStyle(&mrp, 0, AskSoftStyle(&mrp));
    for (int i = 0; i < NB; i++) { int t = TextLength(&mrp, (STRPTR)body[i], strlen(body[i])); if (t > w) w = t; }
    int bh = big->tf_YSize + 2, sh = small->tf_YSize + 2;
    int iw = w + 2 * MARGIN;
    int ih = MARGIN + NH * bh + small->tf_YSize + NB * sh + MARGIN;

    /* the lines, as text gadgets where the window's inside will be (below a
       title bar in the screen's font) */
    struct NewGadget ng;
    memset(&ng, 0, sizeof ng);
    ng.ng_VisualInfo = vi;
    int x0 = scr->WBorLeft, y = scr->WBorTop + scr->Font->ta_YSize + 1 + MARGIN;
    struct Gadget *g = CreateContext(&glist);
    for (int i = 0; i < NH + NB; i++) {
        int h = i < NH;
        const char *s = h ? head[i] : body[i - NH];
        if (i == NH) y += small->tf_YSize;
        if (*s) {
            ng.ng_LeftEdge = x0 + MARGIN;
            ng.ng_TopEdge = y;
            ng.ng_Width = iw - 2 * MARGIN;
            ng.ng_Height = h ? bh : sh;
            ng.ng_TextAttr = h ? &bold : &topaz8;
            g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)s,
                             GTTX_FrontPen, dri->dri_Pens[i == 0 ? HIGHLIGHTTEXTPEN : TEXTPEN],
                             GTTX_Justification, h ? GTJ_CENTER : GTJ_LEFT, TAG_DONE);
        }
        y += h ? bh : sh;
    }
    if (!g) goto out;

    win = OpenWindowTags(NULL,
        WA_PubScreen, (ULONG)scr,
        WA_Title, (ULONG)"About AGA-PISTORM",
        WA_Gadgets, (ULONG)glist,
        WA_InnerWidth, iw, WA_InnerHeight, ih,
        WA_Left, scr->Width > iw ? (scr->Width - iw) / 2 : 0,
        WA_Top, scr->Height > ih ? (scr->Height - ih) / 2 : 0,
        WA_CloseGadget, TRUE, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
        WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_AutoAdjust, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_REFRESHWINDOW,
        TAG_DONE);
    if (!win) goto out;
    GT_RefreshWindow(win, NULL);

    /* a click closes it only when pressed and let go inside the window's
       contents: the button let go after dragging the title bar comes as a
       SELECTUP too, and moving the window closed it (RGB2RTG, AmiKit) */
    for (int done = 0, down = 0; !done; ) {
        WaitPort(win->UserPort);
        struct IntuiMessage *m;
        while ((m = GT_GetIMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code;
            int in = m->MouseX >= win->BorderLeft && m->MouseX < win->Width - win->BorderRight &&
                     m->MouseY >= win->BorderTop && m->MouseY < win->Height - win->BorderBottom;
            GT_ReplyIMsg(m);
            if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
            } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                down = in;
            } else if (cls == IDCMP_CLOSEWINDOW || cls == IDCMP_VANILLAKEY ||
                       (cls == IDCMP_MOUSEBUTTONS && code == SELECTUP && down && in))
                done = 1;
        }
    }
    rc = 0;
out:
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (small) CloseFont(small);
    if (big) CloseFont(big);
    if (dri) FreeScreenDrawInfo(scr, dri);
    if (scr) UnlockPubScreen(NULL, scr);
    if (opened_gt) { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    GfxBase = NULL; IntuitionBase = NULL;
    return rc;
}

/* Which Pi the running AGA-PISTORM kernel was built for and which it runs on:
   3 or 4 each, or both 0 when the kernel does not say (an older one, or not
   mine - then the reads land on the real chipset and are ignored). */
void kernel_boards(int *built, int *runs)   /* also picture.c's */
{
    static const UWORD sel[2] = { D_BUILD_BOARD, D_RUN_BOARD };
    ULONG v[2];
    for (int i = 0; i < 2; i++) {
        Disable();
        DIAG_SEL_W = sel[i];
        ULONG hi = DIAG_DAT_W;
        ULONG lo = DIAG_DAT_W;
        Enable();
        v[i] = (hi << 16) | lo;
    }
    *built = (v[0] == 3 || v[0] == 4) ? (int)v[0] : 0;
    *runs  = (v[1] == 3 || v[1] == 4) ? (int)v[1] : 0;
    if (!*built || !*runs) *built = *runs = 0;
}

/* The running kernel's AGA-PISTORM version as major * 1000 + minor (aga_diag.c
   SEL_KVER, 1.117), 0 from a kernel before it - I only ask it of a kernel
   kernel_boards() knows as mine. A new kernel loads only when the Amiga is
   switched off and on: Ctrl-Amiga-Amiga after an install keeps the old one
   running beside the new agaboot (a user's HAM6 stayed dark after every
   update until a sound setting made him power cycle, 2026-10-17). */
#define D_KVER 0x1803
static ULONG version_num(const char *s)
{
    ULONG v = 0, part = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') part = part * 10 + (ULONG)(*s - '0');
        else { v = v * 1000 + part; part = 0; if (!*s) break; }
    }
    return v;
}
static ULONG kernel_version(void)
{
    Disable();
    DIAG_SEL_W = D_KVER;
    ULONG hi = DIAG_DAT_W;
    ULONG lo = DIAG_DAT_W;
    Enable();
    return (hi << 16) | lo;
}
/* a kernel that does not say is 1.116 or older: 1.116, the last without it */
#define KVER_UNSAID 1116
static ULONG kernel_version_or_old(void)
{
    ULONG v = kernel_version();
    return v ? v : KVER_UNSAID;
}
/* 1 when the kernel running is mine and older than this agaboot */
static int kernel_stale(void)
{
    int kb, kr;
    kernel_boards(&kb, &kr);
    return kb && kernel_version_or_old() < version_num(AGA_PISTORM_VERSION);
}

/* A string property from the device tree Emu68 hands on, into out ("" and 0
   when there is none): "/emu68" "variant" is the PiStorm the kernel found,
   "/" "model" the Pi - what C:Emu68Info prints, which a plain AmigaOS 3.x
   does not have. devicetree.resource is called by hand, sfd bias 6 (from
   RGB2RTG 9bef928). */
#include <inline/macros.h>
static APTR DeviceTreeBase;
#define DT_OpenKey(n)         LP1(0x06, APTR, DT_OpenKey, CONST_STRPTR, n, a0, , DeviceTreeBase)
#define DT_CloseKey(k)        LP1NR(0x0c, DT_CloseKey, APTR, k, a0, , DeviceTreeBase)
#define DT_FindProperty(k, p) LP2(0x18, APTR, DT_FindProperty, APTR, k, a0, CONST_STRPTR, p, a1, , DeviceTreeBase)
#define DT_GetPropValue(p)    LP1(0x30, CONST_APTR, DT_GetPropValue, APTR, p, a0, , DeviceTreeBase)
static int dt_string(const char *node, const char *name, char *out, int len)
{
    out[0] = 0;
    if (!DeviceTreeBase) DeviceTreeBase = OpenResource((STRPTR)"devicetree.resource");
    if (!DeviceTreeBase) return 0;
    APTR key = DT_OpenKey((CONST_STRPTR)node);
    if (!key) return 0;
    APTR prop = DT_FindProperty(key, (CONST_STRPTR)name);
    const char *v = prop ? (const char *)DT_GetPropValue(prop) : NULL;
    if (v) snprintf(out, len, "%s", v);
    DT_CloseKey(key);
    return out[0] != 0;
}

/* a property there at all (a boolean such as the SD driver's "low-speed"):
   1 there, 0 not, -1 no such node or no devicetree.resource */
static int dt_has(const char *node, const char *name)
{
    if (!DeviceTreeBase) DeviceTreeBase = OpenResource((STRPTR)"devicetree.resource");
    if (!DeviceTreeBase) return -1;
    APTR key = DT_OpenKey((CONST_STRPTR)node);
    if (!key) return -1;
    int has = DT_FindProperty(key, (CONST_STRPTR)name) != NULL;
    DT_CloseKey(key);
    return has;
}

/* Is there a DEVS:Monitors icon whose BOARDTYPE tooltype is VideoCore? That
   icon is the only way Picasso96 starts VideoCore.card: CaffeineOS has one,
   a plain AmigaOS 3.x with Picasso96 from Aminet has none, and then the
   VideoCore.card the installer puts in never runs - no RTG desktop on HDMI,
   and RTG Image's desktop TV border does nothing - while the installer said
   "installed" (from RGB2RTG 01b1e00). 1 there is one, 0 none (or no
   DEVS:Monitors), -1 could not look: no icon.library, or no memory. */
#include <dos/dosasl.h>
#include <libraries/asl.h>
#include <proto/asl.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/icon.h>
struct Library *IconBase;                         /* opened where used */
static int vc_monitor(void)
{
    int found = -1;
    IconBase = OpenLibrary((STRPTR)"icon.library", 36);
    if (!IconBase) return -1;
    struct AnchorPath *ap = AllocVec(sizeof(struct AnchorPath) + 256, MEMF_CLEAR);   /* longword aligned */
    if (ap) {
        ap->ap_Strlen = 256;                      /* the full path into ap_Buf */
        found = 0;
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR win = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;              /* no DEVS:Monitors? no requester */
        for (LONG err = MatchFirst((STRPTR)"DEVS:Monitors/#?.info", ap); !err && !found; err = MatchNext(ap)) {
            static char name[260];                /* off the Installer's small stack */
            snprintf(name, sizeof name, "%s", (char *)ap->ap_Buf);
            size_t n = strlen(name);
            if (n <= 5 || ap->ap_Info.fib_DirEntryType > 0) continue;
            name[n - 5] = 0;                      /* GetDiskObject wants it without .info */
            struct DiskObject *d = GetDiskObject((STRPTR)name);
            if (!d) continue;
            STRPTR bt = FindToolType((CONST_STRPTR *)d->do_ToolTypes, (STRPTR)"BOARDTYPE");
            /* AGS and AmiKit name the same driver's monitor Emu68-VC4 */
            if (bt && (!strnicmp((char *)bt, "VideoCore", 9) || !stricmp((char *)bt, "Emu68-VC4"))) found = 1;
            FreeDiskObject(d);
        }
        MatchEnd(ap);
        me->pr_WindowPtr = win;
        FreeVec(ap);
    }
    CloseLibrary(IconBase);
    IconBase = NULL;
    return found;
}

/* ---- VC4_LEGACY_ID (1.116, RGB2RTG 6c01a17) ---------------------------------
   VideoCore.card 1.5 and later - this package's too - tell Picasso96 the board
   is a BT_PiStorm; 1.2.x and older said uaegfx. Picasso96 keeps the screen
   modes and their IDs per board type: a system whose settings were made with
   an old driver finds no VideoCore modes after the new one, and the Workbench
   comes up on the native screen (RGB2RTG on a MultibootOS card, 2026-10-14:
   AmigaOS 3.2, AGS and CaffeineOS alike). The monitor icon's tooltype
   VC4_LEGACY_ID makes the driver say uaegfx again, and the old settings work.

   INSTALL LEGACY decides it before Install replaces the driver - the driver
   the system has NOW is what tells - and leaves the answer in ENV:AGA_LEGACY;
   INSTALL MONITOR, after the driver, applies it to every VideoCore monitor's
   icon, the icon kept first in DEVS:Monitors.pre-aga - not in DEVS:Monitors,
   where the boot starts every file that is not an .info; UNINSTALL MONITOR
   puts the kept icons back. */
#define MONBAK     "DEVS:Monitors.pre-aga"
#define LEGACY_VAR "AGA_LEGACY"

static int vc_board(const char *bt)
{
    return bt && (!strnicmp(bt, "VideoCore", 9) || !strnicmp(bt, "Emu68-VC4", 9));
}

/* the driver file a BoardType value names: SYS:Libs/Picasso96/<value>.card */
static void vc_card(const char *bt, char *out, int len)
{
    int n = 0;
    while (bt[n] && bt[n] != ' ' && bt[n] != '\t' && n < 40) n++;
    snprintf(out, len, "SYS:Libs/Picasso96/%.*s.card", n, bt);
}

/* Picasso96's emulation.library as version << 16 | revision: in memory when
   Picasso96 runs, else from its file; -1 when neither tells. 41.560 is where
   CaffeineOS 9317's own switcher draws the line between Picasso96 2.x, which
   does not know VideoCore 1.5's board type, and the newer ones. */
static LONG p96_emulation(void)
{
    LONG v = -1;
    Forbid();
    struct Library *l = (struct Library *)FindName(&SysBase->LibList, (STRPTR)"emulation.library");
    if (l) v = ((LONG)l->lib_Version << 16) | l->lib_Revision;
    Permit();
    if (v >= 0) return v;
    BPTR f = Open((STRPTR)"LIBS:Picasso96/emulation.library", MODE_OLDFILE);
    if (!f) return -1;
    static char buf[4096 + 32];
    static const char key[] = "emulation.library ";
    long got, keep = 0;
    while (v < 0 && (got = Read(f, buf + keep, 4096)) > 0) {
        long n = keep + got;
        for (long i = 0; i + (long)sizeof key + 3 < n && v < 0; i++)
            if (buf[i] == 'e' && !memcmp(buf + i, key, sizeof key - 1)) {
                const char *p = buf + i + sizeof key - 1;
                if (*p < '0' || *p > '9') continue;
                LONG maj = 0, min = 0;
                while (*p >= '0' && *p <= '9') maj = maj * 10 + (*p++ - '0');
                if (*p++ == '.')
                    while (*p >= '0' && *p <= '9') min = min * 10 + (*p++ - '0');
                v = (maj << 16) | min;
            }
        keep = n < 32 ? n : 32;                   /* a version cut by the read stays whole */
        memmove(buf, buf + n - keep, keep);
    }
    Close(f);
    return v;
}

/* VC4_LEGACY_ID in a monitor's tooltypes: 2 on, 1 there but off as
   "(VC4_LEGACY_ID)", 0 not there */
static int legacy_state(STRPTR *tt)
{
    int st = 0;
    for (; tt && *tt; tt++) {
        if (!stricmp((char *)*tt, "VC4_LEGACY_ID")) return 2;
        if (!stricmp((char *)*tt, "(VC4_LEGACY_ID)")) st = 1;
    }
    return st;
}

/* What to do about a monitor whose driver file is `card` - the driver it has
   now, before mine replaces it: 0 nothing, 1 add VC4_LEGACY_ID, 2 switch
   "(VC4_LEGACY_ID)" on.
   - on already: nothing
   - Picasso96 2.x (emulation.library before 41.560) does not know the new
     board type at all: needed - added, or switched on
   - switched off by hand or by the system - CaffeineOS 9317 does, with a
     newer Picasso96 and settings made for the new type: left so
   - not there, a newer Picasso96: added only when the driver being replaced
     predates the tooltype (an old driver: settings of the old type). One that
     knows it - 1.5 or newer, AmiKit's 1.9 - leaves the settings as they are. */
static int legacy_action(STRPTR *tt, const char *card)
{
    int st = legacy_state(tt);
    if (st == 2) return 0;
    LONG emul = p96_emulation();
    int old_p96 = emul >= 0 && emul < ((41L << 16) | 560);
    if (st == 1) return old_p96 ? 2 : 0;
    if (old_p96) return 1;
    return exists_quiet(card) && !file_has(card, "VC4_LEGACY_ID") ? 1 : 0;
}

/* the VideoCore monitors' icons in DEVS:Monitors, without .info (up to max);
   IconBase must be open */
static int vc_monitors(char names[][108], int max)
{
    int n = 0;
    struct AnchorPath *ap = AllocVec(sizeof(struct AnchorPath) + 256, MEMF_CLEAR);
    if (!ap) return 0;
    ap->ap_Strlen = 256;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    for (LONG err = MatchFirst((STRPTR)"DEVS:Monitors/#?.info", ap); !err && n < max; err = MatchNext(ap)) {
        static char path[140];                    /* off the Installer's small stack */
        const char *b = (const char *)FilePart(ap->ap_Buf);
        size_t k = strlen(b);
        if (k <= 5 || k >= 100 + 5 || ap->ap_Info.fib_DirEntryType > 0) continue;
        snprintf(path, sizeof path, "DEVS:Monitors/%.*s", (int)(k - 5), b);
        struct DiskObject *d = GetDiskObject((STRPTR)path);
        if (!d) continue;
        if (vc_board((char *)FindToolType((CONST_STRPTR *)d->do_ToolTypes, (STRPTR)"BOARDTYPE")))
            snprintf(names[n++], 108, "%.*s", (int)(k - 5), b);
        FreeDiskObject(d);
    }
    MatchEnd(ap);
    me->pr_WindowPtr = win;
    FreeVec(ap);
    return n;
}

/* over every VideoCore monitor, the strongest of legacy_action's answers;
   -1 when icon.library cannot be had */
static int legacy_plan(void)
{
    static char names[8][108];
    int plan = 0;
    if (!(IconBase = OpenLibrary((STRPTR)"icon.library", 36))) return -1;
    int n = vc_monitors(names, 8);
    for (int i = 0; i < n; i++) {
        static char path[140], card[80];
        snprintf(path, sizeof path, "DEVS:Monitors/%s", names[i]);
        struct DiskObject *d = GetDiskObject((STRPTR)path);
        if (!d) continue;
        vc_card((char *)FindToolType((CONST_STRPTR *)d->do_ToolTypes, (STRPTR)"BOARDTYPE"), card, sizeof card);
        int a = legacy_action(d->do_ToolTypes, card);
        if (a > plan) plan = a;
        FreeDiskObject(d);
    }
    CloseLibrary(IconBase);
    IconBase = NULL;
    return plan;
}

/* INSTALL MONITOR: VC4_LEGACY_ID on every VideoCore monitor that needs it -
   as INSTALL LEGACY decided before the driver was replaced (ENV:AGA_LEGACY:
   "1" add, "2" switch on, "0" nothing), or from what is installed now when
   it did not run. The icon as it was goes to DEVS:Monitors.pre-aga first,
   the first time only. */
static int monitor_install(void)
{
    char lv[8] = "";
    int given = GetVar((STRPTR)LEGACY_VAR, (STRPTR)lv, sizeof lv, GVF_GLOBAL_ONLY) > 0;
    static char names[8][108];
    int rc = 0;
    if (!(IconBase = OpenLibrary((STRPTR)"icon.library", 36))) return 20;
    int n = vc_monitors(names, 8);
    for (int i = 0; i < n; i++) {
        static char path[140], info[150], bak[150], card[80];
        snprintf(path, sizeof path, "DEVS:Monitors/%s", names[i]);
        struct DiskObject *d = GetDiskObject((STRPTR)path);
        if (!d) continue;
        STRPTR *old = d->do_ToolTypes;
        vc_card((char *)FindToolType((CONST_STRPTR *)old, (STRPTR)"BOARDTYPE"), card, sizeof card);
        int st = legacy_state(old);
        int want = given ? (lv[0] == '1' || lv[0] == '2') : legacy_action(old, card) != 0;
        int legacy = !want || st == 2 ? 0 : st == 1 ? 2 : 1;     /* as the icon is now */
        if (!legacy) {
            Printf(SETUP_OK, (ULONG)names[i], (ULONG)(st == 2 ? "VC4_LEGACY_ID on already" : "VC4_LEGACY_ID not needed"));
            FreeDiskObject(d);
            continue;
        }
        snprintf(info, sizeof info, "%s.info", path);
        snprintf(bak, sizeof bak, MONBAK "/%s.info", names[i]);
        if (!exists_quiet(MONBAK)) { BPTR l = CreateDir((STRPTR)MONBAK); if (l) UnLock(l); }
        if (!exists_quiet(bak) && !copy_binary(info, bak)) {
            Printf(SETUP_OK, (ULONG)info, (ULONG)"CANNOT KEEP A COPY - left alone");
            FreeDiskObject(d);
            rc = 20;
            continue;
        }
        int cnt = 0;
        for (STRPTR *tt = old; tt && *tt; tt++) cnt++;
        STRPTR *nt = AllocVec((cnt + 2) * sizeof(STRPTR), MEMF_CLEAR);
        if (!nt) { FreeDiskObject(d); rc = 20; continue; }
        int j = 0;
        if (legacy == 1) nt[j++] = (STRPTR)"VC4_LEGACY_ID";
        for (int t = 0; t < cnt; t++)
            nt[j++] = (legacy == 2 && !stricmp((char *)old[t], "(VC4_LEGACY_ID)")) ? (STRPTR)"VC4_LEGACY_ID" : old[t];
        d->do_ToolTypes = nt;
        if (PutDiskObject((STRPTR)path, d))
            Printf(SETUP_OK, (ULONG)names[i], (ULONG)(legacy == 1 ? "VC4_LEGACY_ID added (the icon as it was: " MONBAK ")"
                                                                  : "VC4_LEGACY_ID switched on (the icon as it was: " MONBAK ")"));
        else {
            Printf(SETUP_OK, (ULONG)info, (ULONG)"CANNOT WRITE");
            rc = 20;
        }
        d->do_ToolTypes = old;
        FreeVec(nt);
        FreeDiskObject(d);
    }
    if (!n) Printf(SETUP_OK, (ULONG)"DEVS:Monitors", (ULONG)"no VideoCore monitor - nothing done");
    CloseLibrary(IconBase);
    IconBase = NULL;
    DeleteVar((STRPTR)LEGACY_VAR, GVF_GLOBAL_ONLY);
    return rc;
}

/* UNINSTALL MONITOR: every icon INSTALL MONITOR kept goes back */
static int monitor_uninstall(void)
{
    static char found[10][140];
    int n = 0, rc = 0;
    struct AnchorPath *ap = AllocVec(sizeof(struct AnchorPath) + 256, MEMF_CLEAR);
    if (!ap) return 20;
    ap->ap_Strlen = 256;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    for (LONG err = MatchFirst((STRPTR)MONBAK "/#?.info", ap); !err && n < 10; err = MatchNext(ap))
        snprintf(found[n++], sizeof found[0], "%s", (const char *)FilePart(ap->ap_Buf));
    MatchEnd(ap);
    me->pr_WindowPtr = win;
    FreeVec(ap);
    for (int i = 0; i < n; i++) {
        static char from[160], to[160];
        snprintf(from, sizeof from, MONBAK "/%s", found[i]);
        snprintf(to, sizeof to, "DEVS:Monitors/%s", found[i]);
        if (copy_binary(from, to) && DeleteFile((STRPTR)from)) Printf(SETUP_OK, (ULONG)to, (ULONG)"put back");
        else { Printf(SETUP_OK, (ULONG)to, (ULONG)"CANNOT PUT BACK"); rc = 20; }
    }
    if (exists_quiet(MONBAK)) DeleteFile((STRPTR)MONBAK);   /* the drawer, when empty */
    return rc;
}

static int check(const char *pick)
{
    char variant[64], board[96];
    run_capture("C:Emu68Info VARIANT", variant, sizeof variant);
    run_capture("C:Emu68Info BOARDNAME", board, sizeof board);
    /* No C:Emu68Info - a plain AmigaOS 3.x has none: the same two strings
       from the device tree, where Emu68Info gets them */
    if (!variant[0]) dt_string("/emu68", "variant", variant, sizeof variant);
    if (!board[0]) dt_string("/", "model", board, sizeof board);
    /* Neither of them means "cannot tell", not "no": the Installer asks instead.
       Emu68 1.1 names the board at runtime - "pistorm" (classic), "pistorm16",
       "pistorm32lite". 1.0.x only had the build name, "pistorm" or
       "pistorm32lite", and a PiStorm16 kernel of that age may say the latter:
       so "pistorm32lite" is "cannot tell" too, and the Installer asks. */
    int classic = !variant[0] ? -1 : !strcmp(variant, "pistorm");
    int ps16 = (!variant[0] || !strcmp(variant, "pistorm32lite")) ? -1 : !strcmp(variant, "pistorm16");
    int pi4 = !board[0] ? -1 : strstr(board, "Pi 4") || strstr(board, "Pi 400") || strstr(board, "Compute Module 4");
    /* the Pi 3 has its own kernel since 1.1; the Installer picks it by this.
       The Zero 2 W is the same SoC family - VideoCore IV, the 19.2 MHz timer
       the kernel tells the two generations apart by - so it takes the Pi 3
       kernel too (untested: nobody here has one). */
    int pi3 = !board[0] ? -1 : strstr(board, "Pi 3") || strstr(board, "Compute Module 3") || strstr(board, "Zero 2");
    /* The real Agnus, read before any sandbox: VPOSR bit 12 is its NTSC bit on
       OCS and ECS alike. Pre-answers the Installer's PAL/NTSC question. */
    int ntsc = aga_active() ? -1 : (*(volatile UWORD *)0xDFF004 & 0x1000) != 0;
    const char *boot = pick && pick[0] ? pick_boot(pick) : find_boot();
    int writable = 0;
    if (boot[0]) {
        char probe[64];
        snprintf(probe, sizeof probe, "%sagaboot.write-test", boot);
        BPTR f = Open((STRPTR)probe, MODE_NEWFILE);
        if (f) { Close(f); DeleteFile((STRPTR)probe); writable = 1; }
    }
    /* the Installer copies the kernel and overlays here */
    SetVar((STRPTR)"AGAPISTORM_BOOT", (STRPTR)boot, -1, GVF_GLOBAL_ONLY);
    {   /* ...under the names the partition already has, in whatever case:
           the Installer joins no names to the partition itself (find_ci) */
        char p[64];
        SetVar((STRPTR)"AGAPISTORM_CFGFILE", (STRPTR)(boot[0] ? config_txt() : ""), -1, GVF_GLOBAL_ONLY);
        if (boot[0]) find_ci(boot, "overlays", p, sizeof p); else p[0] = 0;
        SetVar((STRPTR)"AGAPISTORM_OVLDIR", (STRPTR)p, -1, GVF_GLOBAL_ONLY);
        if (boot[0]) find_ci(boot, "kernel", p, sizeof p); else p[0] = 0;
        SetVar((STRPTR)"AGAPISTORM_KDIR", (STRPTR)p, -1, GVF_GLOBAL_ONLY);
    }
    int config = 0, config16 = 0, unicam = 0, palovl = 0, cfgopen = 0, hdmicfg = 0, ratecfg = 0, slowcfg = 0;
    char *t = boot[0] ? setup_read(config_txt()) : NULL;
    int have_cfg = t != NULL;
    if (t) {
        config = setup_has_section(t, 0);      /* each board's section, whichever this is */
        config16 = setup_has_section(t, 1);
        /* my block without its end line: INSTALL CONFIG would stop on it
           after the kernels were copied, so the Installer stops here */
        cfgopen = setup_config_open(t);
        hdmicfg = setup_config_hdmi_state(t);  /* Sound on HDMI's hdmi_drive=2 */
        ratecfg = setup_config_rate_state(t);  /* HDMI rate's mode lines */
        slowcfg = lowspeed_state(config_txt(), t);   /* the Pi 4's SD card at 25 MHz: 1 mine, 2 the card's own */
        twin_find(t);                          /* MultibootOS: this system's config_<os>.txt */
        /* Overlays already in config.txt settle their question: the installer
           only ever adds lines. dtoverlay=pal also explains a "PAL" Agnus on
           an NTSC machine - Emu68 writes the PAL bit into BEAMCON0 at boot
           and masks the NTSC bit out of VPOSR (aarch64/vectors.c). */
        for (char *p = t; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, "dtoverlay=unicam", 16)) unicam = 1;
            if (!strncmp(p, "dtoverlay=pal", 13) && (p[13] == '\r' || p[13] == '\n' || p[13] == ',' || !p[13])) palovl = 1;
        }
        free(t);
    }
    /* 2 MB of chip RAM, as the OS counted it at boot. The game itself runs in
       the emulated 2 MB, but entry and exit copy 2 MB between the real chip RAM
       and the emulated one - on a 1 MB machine the upper half is a mirror of
       the lower, and the copy back would put stale data over the OS. And
       WHDLoad refuses a 2 MB game when the OS reports 1 MB anyway. */
    ULONG chip = AvailMem(MEMF_CHIP | MEMF_TOTAL);
    int chip2m = chip >= 0x1C0000;
    int vcmon = vc_monitor();
    set_flag("AGAPISTORM_CLASSIC", classic);
    set_flag("AGAPISTORM_PS16", ps16);
    set_flag("AGAPISTORM_PI4", pi4);
    set_flag("AGAPISTORM_PI3", pi3);
    set_flag("AGAPISTORM_WRITABLE", writable);
    set_flag("AGAPISTORM_CONFIG", config);
    set_flag("AGAPISTORM_CONFIG16", config16);
    set_flag("AGAPISTORM_CFGOPEN", cfgopen);
    set_flag("AGAPISTORM_CHIP2M", chip2m);
    set_flag("AGAPISTORM_NTSC", ntsc);
    set_flag("AGAPISTORM_UNICAM", unicam);
    set_flag("AGAPISTORM_PALOVL", palovl);
    set_flag("AGAPISTORM_VCMON", vcmon);
    SetVar((STRPTR)"AGAPISTORM_SLOW", (STRPTR)(slowcfg == 1 ? "1" : slowcfg == 2 ? "2" : "0"), -1, GVF_GLOBAL_ONLY);
    Printf("  agaboot                  %s%s\n", (ULONG)AGA_PISTORM_VERSION, (ULONG)AGABOOT_TAG);
    Printf("  PiStorm variant          %s\n", (ULONG)(variant[0] ? variant : "unknown (no C:Emu68Info, no device tree)"));
    Printf("  Raspberry Pi             %s\n", (ULONG)(board[0] ? board : "unknown"));
    {
        int kb, kr;
        kernel_boards(&kb, &kr);
        if (kb) {
            Printf("  AGA-PISTORM kernel       for a Pi %ld, running on a Pi %ld%s\n", (ULONG)kb, (ULONG)kr,
                   (ULONG)(kb != kr ? " - run the installer again" : ""));
            ULONG kv = kernel_version(), mine = version_num(AGA_PISTORM_VERSION);
            int old = kernel_version_or_old() < mine;
            if (kv) Printf("  kernel version           %ld.%03ld", kv / 1000, kv % 1000);
            else    PutStr("  kernel version           1.116 or older (it does not say)");
            PutStr(old ? " - OLDER than this agaboot:\n"
                         "                           power cycle (switch off and on) to start the new one\n" : "\n");
        } else  /* the reads landed on the real chipset: stock Emu68, or
                   AGA-PISTORM 1.0, which did not say its board yet */
            PutStr("  AGA-PISTORM kernel       NOT running (power cycle after installing;"
                   " or config.txt starts another kernel)\n");
    }
    Printf("  Agnus                    %s\n", (ULONG)(ntsc < 0 ? "unknown (AGA active)" : ntsc ? "NTSC" : "PAL"));
    Printf("  Framethrower in config   %s\n", (ULONG)(unicam ? "yes" : "no"));
    Printf("  dtoverlay=pal in config  %s\n", (ULONG)(palovl ? "yes (the system already runs as PAL)" : "no"));
    Printf("  Chip RAM                 %ld KB%s\n", chip >> 10, (ULONG)(chip2m ? "" : " (games get 2 MB from the Pi)"));
    Printf("  Emu68 boot partition     %s %s\n", (ULONG)(boot[0] ? boot : "NOT FOUND"),
           (ULONG)(!boot[0] ? "" : writable ? "(writable)" : "(READ-ONLY)"));
    if (boot[0]) Printf("  config.txt is            %s\n", (ULONG)config_txt());
    if (have_cfg && twin.files) {
        if (twin.path[0]) Printf("  MultibootOS              %s is this system's config.txt (changed with it)\n",
                                 (ULONG)FilePart((STRPTR)twin.path));
        else Printf("  MultibootOS              config.txt is %s of the config_*.txt files - its\n"
                    "                           menu may undo a change at the next pick\n",
                    (ULONG)(twin.same > 1 ? "more than one" : "none"));
    }
    if (!have_cfg)
        PutStr("  config.txt               missing\n");
    else {
        Printf("  config.txt [gpio17=0]    %s\n", (ULONG)(config ? "found (classic PiStorm)" : "not found"));
        Printf("  config.txt [gpio24=1]    %s\n", (ULONG)(config16 ? "found (PiStorm16)" : "not found"));
        Printf("  SD card speed (Pi 4)     %s\n", (ULONG)(slowcfg == 1 ? "25 MHz - " LOWSPEED_WORD " in the cmdline (the installer's)"
                                                    : slowcfg == 2 ? "25 MHz - the card's own setting"
                                                    : "50 MHz"));
        {   /* what the running kernel gave the SD driver: the overlay's or
               the cmdline's "low-speed" (start.c; a stock 1.1 kernel takes
               only the overlay's) */
            int e = dt_has("/emu68/brcm-emmc", "low-speed");
            if (e >= 0) Printf("  SD card speed now        %s\n", (ULONG)(e ? "25 MHz (the running kernel's low-speed)" : "50 MHz"));
        }
        if (cfgopen)
            PutStr("  config.txt AGA block     NO END LINE (# AGA-PISTORM }) - put it back by hand\n");
    }
    {
        int snd = hdmisound_pref();
        Printf("  Sound on HDMI            %s%s\n", (ULONG)(snd ? "ON" : "off"),
               (ULONG)(!have_cfg ? "" : hdmicfg == 1 ? ", hdmi_drive=2 in config.txt" :
                       hdmicfg == 2 ? ", config.txt's own hdmi_drive=2" :
                       snd ? ", no hdmi_drive=2 in config.txt yet" : ""));
        Printf("  HDMI sound filter        %s\n", (ULONG)filter_names[hdmifilter_pref()]);
        {
            int r = hdmirate_pref();
            Printf("  HDMI rate                %s%s\n", (ULONG)(r ? hdmirate_name(r) : "config.txt's own"),
                   (ULONG)(!have_cfg || !r ? "" : ratecfg == r ? ", in config.txt" : ", NOT in config.txt yet"));
            Printf("  HDMI sync                %s\n", (ULONG)(hdmisync_pref() ? "ON (the HDMI frame follows the emulated one)" : "off"));
        }
        {
            struct sound_prefs sp;
            sound_load(&sp);
            Printf("  Sound mix                HDMI volume %s, stereo %s%s\n", (ULONG)sound_vol_name(sp.vol),
                   (ULONG)sound_width_name(sp.width), (ULONG)(sp.swap ? ", left and right swapped" : ""));
            Printf("  Paula on HDMI            %s\n", (ULONG)(sp.paula ? "band-limited" : "classic"));
            Printf("  HDMI sound rate          %s\n", (ULONG)sound_rate_name(sp.rate));
            int mu, db = hifivol_pref(&mu);
            Printf("  HiFi volume (AHI)        %s%ld dB%s\n", (ULONG)(db ? "-" : ""), (LONG)db, (ULONG)(mu ? ", muted" : ""));
            struct eq_set es;
            char ed[96];
            eq_load(&es);
            eq_describe(&es, ed, sizeof ed);
            Printf("  Effects (HDMI)           %s\n", (ULONG)ed);
            struct vm_set vs;
            char vd[128];
            vm_load(&vs);
            vm_describe(&vs, vd, sizeof vd);
            Printf("  Voice mixer              %s\n", (ULONG)vd);
        }
        ahi16_check();
    }
    Printf("  VideoCore monitor icon   %s\n", (ULONG)(vcmon > 0 ? "found in DEVS:Monitors" :
           vcmon < 0 ? "unknown (no icon.library?)" :
           "NONE in DEVS:Monitors - Picasso96 starts\n"
           "                           VideoCore.card only through a monitor whose\n"
           "                           BoardType is VideoCore (see the README)"));
    if (vcmon > 0) {
        int lp = legacy_plan();
        Printf("  VC4_LEGACY_ID            %s\n", (ULONG)(lp == 1 ? "to be added - the screen modes were made\n"
                                                                    "                           for the old driver's board type"
                                                    : lp == 2 ? "to be switched on - the screen modes were\n"
                                                                "                           made for the old driver's board type"
                                                    : lp < 0 ? "unknown (no icon.library?)" : "nothing to do (on, or not needed)"));
    }
    /* the answer is in the ENV flags; a non-zero return would only give the
       Installer script an error to trip over */
    return 0;
}

/* BUSTEST [HAM6]: how fast the kernel moves data through the real bus - the
   number that decides whether an AGA picture can be drawn into real chip RAM
   every frame for the Amiga's own RGB port (a 320 x 256 HAM6 frame is 61,440
   bytes). 64 KB of chip RAM is lent to the kernel ($DFF1EC) for four timed
   runs ($596N; the kernel's aga_bustest_run). With HAM6 a 320 x 256 HAM6
   screen is in front meanwhile, so the real Agnus fetches six bitplanes while
   the kernel writes, as it would with the picture up. */
#include <graphics/modeid.h>
#define D_BUSTEST 470                              /* mode, bytes, microseconds, worst ns: arm/aga_diag.c pins them */
#define AGA_BUSBUF (*(volatile ULONG *)0xDFF1EC)
ULONG diag_read(UWORD sel)                         /* also picture.c's */
{
    if (sel >= 0x100) sel = 0x1700 + (sel - 0x100); /* counters from 256 up: $100 is the log */
    Disable();
    DIAG_SEL_W = sel;
    ULONG hi = DIAG_DAT_W;
    ULONG lo = DIAG_DAT_W;
    Enable();
    return (hi << 16) | lo;
}
/* What the kernel says about HDMI sound (arm/aga_hdmi.c; aga_diag.c pins the
   index), as a reason it cannot play, or NULL when it can - or is playing */
#define D_HDMI_STATE 490
static const char *hdmisound_why_not(void)
{
    int built, runs;
    kernel_boards(&built, &runs);
    if (!built) return "the AGA-PISTORM kernel is not running";
    switch (diag_read(D_HDMI_STATE)) {
    case 2:  return "no HDMI link that carries sound - a\ndisplay the Pi drives as DVI needs\nhdmi_drive=2 in config.txt (Sound On adds\nit) and a power cycle - or no moving\npicture on HDMI now (it starts when it comes)";
    case 3:  return runs == 3 ? "the HDMI clock is off, or runs at a rate\nit does not know (it starts once the\nscreen's clock is back)"
                              : "there was no HDMI screen when the Pi\nstarted";
    case 4:  return "the Pi's firmware uses the packet slot it\nneeds";
    case 5:  return "no DMA channel is free";
    case 6:  return "an old test kernel without it on this Pi\n(install the current one)";
    case 7:  return "cmdline.txt says aga.nohdmiaudio";
    case 8:  return "another program plays sound over HDMI\n(it starts when that one stops)";
    default: return NULL;                        /* 0 off, 1 playing */
    }
}

/* HiFi sound switched (ahi16.c), with a word when sound on HDMI - what it
   plays through - is off */
static int ahi16_switch(int on, int hdmi, char *msg, int len)
{
    int rc = ahi16_set(on, msg, len);
    if (on && rc < 20 && !hdmi) {
        int n = strlen(msg);
        snprintf(msg + n, len - n, "%sSound on HDMI is Off: switch it On too,\nor none of it is heard.", n ? "\n\n" : "");
        if (rc < 5) rc = 5;
    }
    return rc;
}

/* the Chipset menu's Sound has no Shell to print to: a requester */
static void sound_tell(const char *body)
{
    /* through "%s": a path with a % in it is no format */
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM - Sound",
                             (UBYTE *)"%s", (UBYTE *)"OK" };
    ULONG args[1] = { (ULONG)body };
    int opened = 0;
    if (!IntuitionBase && (IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37))) opened = 1;
    if (IntuitionBase) EasyRequestArgs(NULL, &es, NULL, args);
    if (opened) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

/* What a file system still holds for the volume, written out - a FAT one
   may keep it until it is put away, and the Pi reads config.txt at the next
   power cycle */
static void flush_vol(const char *vol)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR win = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;                  /* no "insert volume" requester */
    struct DevProc *dp = GetDeviceProc((STRPTR)vol, NULL);
    if (dp) {
        DoPkt(dp->dvp_Port, ACTION_FLUSH, 0, 0, 0, 0, 0);
        FreeDeviceProc(dp);
    }
    me->pr_WindowPtr = win;
}

/* Sound on HDMI switched On or Off (SOUND): config.txt's hdmi_drive=2 with it,
 * as RGB2RTG does. A display the Pi's firmware drives as DVI gets no sound at
 * all; hdmi_drive=2 makes it HDMI, from the next power cycle. Most displays
 * get HDMI anyway, but the line goes in with every On, so it is there when the
 * display changes. The sound itself is the kernel's and the setting is
 * ENVARC:'s: neither needs the boot partition - which many systems do not
 * mount by themselves, and which is called EMU68: on CaffeineOS only. It is
 * found by its config.txt on any mounted volume (find_boot, which wakes a
 * fat95 one first); not mounted, it is a message saying what to do, and the
 * sound is switched all the same. Only my block's line changes
 * (setup_config_hdmi). A PiStorm16's config.txt is left alone: only its
 * kernel= line is mine there, as its tester asked.
 * msg gets what came of it, "" when there is nothing to say. Returns 0
 * nothing to say, 1 config.txt changed, 5 a word of warning (a PiStorm16, or
 * the card's own hdmi_drive=1), 10 no boot partition mounted, 20 config.txt
 * could not be changed. */
static int sound_config(int on, char *msg, int len)
{
    char variant[64];
    msg[0] = 0;
    run_capture("C:Emu68Info VARIANT", variant, sizeof variant);
    if (!variant[0]) dt_string("/emu68", "variant", variant, sizeof variant);
    if (!strcmp(variant, "pistorm16") || !strcmp(variant, "pistorm32lite")) {
        if (!on) return 0;
        snprintf(msg, len, "On a PiStorm16 config.txt is left as it\n"
                           "is. A display the Pi drives as DVI needs\n"
                           "hdmi_drive=2 in it for the sound: add that\n"
                           "line yourself, then power cycle.");
        return 5;
    }
    const char *path = config_txt();
    if (!path[0]) {
        if (!on) return 0;                         /* a line left in does no harm */
        snprintf(msg, len, "config.txt was not changed: the SD card's\n"
                           "boot partition (the FAT one with config.txt)\n"
                           "is not mounted. Mount it the way your\n"
                           "system does (the README has a mountlist)\n"
                           "and switch Sound on again - or add\n"
                           "hdmi_drive=2 to config.txt on a PC. Only a\n"
                           "display the Pi drives as DVI needs it.");
        return 10;
    }
    char *t = setup_read(path);
    if (!t) { snprintf(msg, len, "%s could not be read.", path); return 20; }
    twin_find(t);
    int err, rc = 0;
    char *n = setup_config_hdmi(t, on, &err);
    if (err == 1) {
        if (on) snprintf(msg, len, "%s was not changed: AGA-PISTORM\nis not installed in it.", path);
        rc = on ? 20 : 0;
    } else if (err == 3) {
        snprintf(msg, len, "%s was not changed: its AGA-PISTORM\n"
                           "block has lost its end line\n"
                           "(# AGA-PISTORM }). Put it back by hand.", path);
        rc = 20;
    } else if (err) {
        snprintf(msg, len, "%s was not changed: out of memory.", path);
        rc = 20;
    } else if (n) {
        if (!setup_backup(path, t) || !setup_write(path, n)) {
            snprintf(msg, len, "%s could not be written\n(read-only?): hdmi_drive=2 %s.", path,
                     on ? "is not in it" : "is still in it");
            rc = 20;
        } else {
            int tw = twin_put(t, n);
            flush_vol(find_boot());
            snprintf(msg, len, on ? "config.txt: hdmi_drive=2 added - HDMI with\nsound from the next power cycle."
                                  : "config.txt: hdmi_drive=2 taken out (from\nthe next power cycle).");
            twin_note(tw, msg, len);
            rc = tw < 0 ? 20 : 1;
        }
    }
    /* the card's own hdmi_drive=1 (DVI) further down wins over mine */
    if (on && !err && setup_config_has(t, "hdmi_drive=1")) {
        size_t k = strlen(msg);
        snprintf(msg + k, len - k, "%sBut config.txt has its own hdmi_drive=1\n"
                                   "line (DVI: no sound). Remove it.", k ? "\n" : "");
        if (rc < 5) rc = 5;
    }
    free(n);
    free(t);
    return rc;
}

/* HDMI rate switched (RTG Image): ENVARC:AGA_HDMIRATE, and config.txt's
 * mode lines in my block (setup_config_rate) - found as sound_config finds
 * it; a PiStorm16's config.txt is left alone, as there. rate 50, 60, or 0
 * for config.txt's own mode. msg gets what came of it. Returns as
 * sound_config: 0 nothing to say, 1 config.txt changed, 5 a word of warning
 * (a PiStorm16, or a line of the card's own further down that wins over
 * mine), 10 no boot partition mounted, 20 config.txt could not be changed. */
int hdmirate_set(int rate, char *msg, int len)
{
    char variant[64];
    static const char *const lines[3] = { "hdmi_group=1\nhdmi_mode=31\nhdmi_force_mode=1\nhdmi_pixel_encoding=2",
                                          "hdmi_group=2\nhdmi_mode=82\nhdmi_force_mode=1",
                                          "hdmi_group=2\nhdmi_mode=87\nhdmi_timings=1920 1 528 44 148 1080 1 4 5 36\n"
                                          "  0 0 0 50 0 149094000 3   (one line)\nhdmi_force_mode=1" };
    const char *mode = hdmirate_name(rate);
    msg[0] = 0;
    if (rate == 50 || rate == 60 || rate == HDMIRATE_ROOM) {
        STRPTR v = (STRPTR)(rate == 50 ? "50" : rate == 60 ? "60" : "50.2");
        SetVar((STRPTR)HDMIRATE_VAR, v, -1, GVF_GLOBAL_ONLY);
        SetVar((STRPTR)HDMIRATE_VAR, v, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        rate = 0;
        DeleteVar((STRPTR)HDMIRATE_VAR, GVF_GLOBAL_ONLY);
        DeleteVar((STRPTR)HDMIRATE_VAR, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    }
    run_capture("C:Emu68Info VARIANT", variant, sizeof variant);
    if (!variant[0]) dt_string("/emu68", "variant", variant, sizeof variant);
    if (!strcmp(variant, "pistorm16") || !strcmp(variant, "pistorm32lite")) {
        if (!rate) return 0;
        snprintf(msg, len, "On a PiStorm16 config.txt is left as it\n"
                           "is. For HDMI at %s add these lines\n"
                           "to it yourself, then power cycle:\n\n%s", mode,
                 lines[rate == HDMIRATE_ROOM ? 2 : rate == 60]);
        return 5;
    }
    const char *path = config_txt();
    if (!path[0]) {
        snprintf(msg, len, "config.txt was not changed: the SD card's\n"
                           "boot partition (the FAT one with config.txt)\n"
                           "is not mounted. Mount it the way your\n"
                           "system does (the README has a mountlist)\n"
                           "and set the HDMI rate again.");
        return 10;
    }
    char *t = setup_read(path);
    if (!t) { snprintf(msg, len, "%s could not be read.", path); return 20; }
    twin_find(t);
    int err, rc = 0;
    char *n = setup_config_rate(t, rate, &err);
    if (err == 1) {
        snprintf(msg, len, "%s was not changed: AGA-PISTORM\nis not installed in it.", path);
        rc = 20;
    } else if (err == 3) {
        snprintf(msg, len, "%s was not changed: its AGA-PISTORM\n"
                           "block has lost its end line\n"
                           "(# AGA-PISTORM }). Put it back by hand.", path);
        rc = 20;
    } else if (err) {
        snprintf(msg, len, "%s was not changed: out of memory.", path);
        rc = 20;
    } else if (n) {
        if (!setup_backup(path, t) || !setup_write(path, n)) {
            snprintf(msg, len, "%s could not be written\n(read-only?).", path);
            rc = 20;
        } else {
            int tw = twin_put(t, n);
            flush_vol(find_boot());
            if (rate) snprintf(msg, len, "config.txt: HDMI at %s from the next\npower cycle.", mode);
            else snprintf(msg, len, "config.txt: HDMI in config.txt's own mode\nfrom the next power cycle.");
            twin_note(tw, msg, len);
            rc = tw < 0 ? 20 : 1;
        }
    }
    /* config.txt's later line wins: the card's own mode further down */
    if (rate && !err && (setup_config_after_block(n ? n : t, "hdmi_mode=") || setup_config_after_block(n ? n : t, "hdmi_group="))) {
        size_t k = strlen(msg);
        snprintf(msg + k, len - k, "%sBut config.txt has its own hdmi_mode or\n"
                                   "hdmi_group line further down, which wins:\n"
                                   "put a # in front of it.", k ? "\n\n" : "");
        if (rc < 5) rc = 5;
    }
    free(n);
    free(t);
    return rc;
}

static int bustest(int ham6)
{
    static const char *const what[4] = { "16-bit writes, bus lock each", "32-bit writes, bus lock each",
                                         "16-bit writes, lock per 64 words", "16-bit reads, bus lock each" };
    int built, runs, rc = 0;
    kernel_boards(&built, &runs);
    if (!built) { PutStr("agaboot: not the AGA-PISTORM kernel\n"); return 5; }
    if (aga_active()) { PutStr("agaboot: not while the sandbox is up\n"); return 5; }
    APTR buf = AllocMem(65536 + 16, MEMF_CHIP | MEMF_CLEAR);
    if (!buf) { PutStr("agaboot: no 64 KB of chip RAM free\n"); return 10; }
    struct Screen *scr = NULL;
    if (ham6) {
        IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
        if (IntuitionBase)
            scr = OpenScreenTags(NULL, SA_DisplayID, PAL_MONITOR_ID | HAM_KEY, SA_Width, 320, SA_Height, 256,
                                 SA_Depth, 6, SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN, TAG_DONE);
        if (!scr) PutStr("agaboot: no HAM6 screen - measuring without one\n");
        else Delay(10);                            /* let it come up */
    }
    ULONG a = ((ULONG)buf + 3) & ~3UL;
    Disable(); AGA_BUSBUF = a; Enable();
    Printf("agaboot: real bus test, 64 KB of chip RAM at $%08lx%s\n", a, (ULONG)(scr ? ", HAM6 screen up" : ""));
    for (int m = 0; m < 4; m++) {
        Disable(); AGA_CTRL = (UWORD)(0x5960 | m); Enable();
        ULONG mode = diag_read(D_BUSTEST), bytes = diag_read(D_BUSTEST + 1);
        ULONG us = diag_read(D_BUSTEST + 2), worst = diag_read(D_BUSTEST + 3);
        if (mode != (ULONG)m || !us || !bytes) { Printf("  %-34s no answer (a kernel without BUSTEST?)\n", (ULONG)what[m]); rc = 5; continue; }
        ULONG bpms = bytes * 1000 / us;            /* bytes per millisecond */
        ULONG kbs = bpms * 1000 / 1024;
        ULONG fps10 = bpms * 10000 / 61440;        /* HAM6 frames a second, x10 */
        Printf("  %-34s %6ld us  %5ld KB/s  %3ld.%ld HAM6 frames/s  slowest %ld ns\n",
               (ULONG)what[m], us, kbs, fps10 / 10, fps10 % 10, worst);
    }
    Disable(); AGA_BUSBUF = 0; Enable();
    if (scr) CloseScreen(scr);
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
    FreeMem(buf, 65536 + 16);
    return rc;
}

/* RUN's work, for the Shell's RUN and the menu's Run program...: the
   sandbox entered unless it is up already, the command run synchronously,
   the sandbox left again if this entered it. The program's return code;
   *failed set when it could not be started on the emulated chipset (a
   program's own code can be negative, so that is not the sign). */
static int run_program(const char *cmd, int *failed)
{
    *failed = 0;
    int entered = 0;
    if (!aga_active()) {
        enter_magic = MAGIC_SANDBOX_ON;  /* the desktop's entry, not a game's: the OS keeps its CIAs */
        zoom_default = 2;                /* a program's screen is a standard one, like the desktop's */
        /* its drawer - Run program's CurrentDir, or the Shell's - for a
           picture of its own, as a game's (RTG Image's Settings for) */
        if (game_key(entry_game, sizeof entry_game))
            SetVar((STRPTR)LASTGAME_VAR, (STRPTR)entry_game, -1, GVF_GLOBAL_ONLY);
        sandbox("ON");
        if (!aga_active()) {
            PutStr("agaboot: RUN - the emulated AGA chipset is not available, so it was not started\n");
            *failed = 1;
            return 10;
        }
        entered = 1;
    }
    Printf("agaboot: running %s on the emulated AGA chipset\n", (ULONG)cmd);
    BPTR in = Input(), out = Output(), nil_in = 0, nil_out = 0;
    if (!in) in = nil_in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    if (!out) out = nil_out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    struct TagItem tags[] = { { SYS_Input, (ULONG)in }, { SYS_Output, (ULONG)out },
                              { SYS_UserShell, TRUE }, { TAG_DONE, 0 } };
    LONG rc = SystemTagList((STRPTR)cmd, tags);   /* synchronous: my handles stay open */
    if (nil_in) Close(nil_in);
    if (nil_out) Close(nil_out);
    /* back on the real chipset - unless the program left the sandbox
       itself (a WHDLoad game started this way: its cleanup hook did) */
    if (entered && aga_active()) sandbox("OFF");
    if (rc == -1) { PutStr("agaboot: RUN - could not start it\n"); *failed = 1; return 10; }
    return (int)rc;
}

static int ask3(const char *title, const char *body, const char *gadgets)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)title, (UBYTE *)"%s", (UBYTE *)gadgets };
    ULONG args[1] = { (ULONG)body };              /* through "%s": no format in the body */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase) return 0;
    int r = EasyRequestArgs(NULL, &es, NULL, args);
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = NULL;
    return r;
}

/* Chipset menu - Run program...: a file requester, then RUN on the pick,
   with the program's own drawer as its current one (as Workbench starts a
   program: games load their data from there) and a 64 KB stack for the
   shell that runs it. The drawer is remembered in ENVARC:AGA_RUNDIR. */
#define RUNDIR_VAR "AGA_RUNDIR"
struct Library *AslBase;

/* What would keep the pick from running, said before the sandbox is entered
   - otherwise the screen goes to the emulated chipset and back and the
   shell's "not executable" goes to NIL: (found by review). NULL: a program
   (a hunk file, or a script with its s bit) that can be started. */
static const char *run_check(const char *path, const char *file)
{
    int n = (int)strlen(file);
    if (n > 6 && !stricmp(file + n - 6, ".slave"))
        return "is a WHDLoad slave. Start the game from its\n"
               "icon: with Init on, WHDLoad games run on the\n"
               "emulated chipset by themselves.";
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (!l) return "cannot be found.";
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    const char *why = NULL;
    if (!fib || !Examine(l, fib)) why = "cannot be read.";
    else if (fib->fib_DirEntryType > 0) why = "is a drawer, not a program.";
    else if (fib->fib_Protection & FIBF_EXECUTE)     /* set means NOT executable */
        why = "is not marked as a program (its e\nprotection bit is off).";
    else if (!(fib->fib_Protection & FIBF_SCRIPT)) {
        ULONG magic = 0;
        BPTR f = Open((STRPTR)path, MODE_OLDFILE);
        if (!f || Read(f, &magic, 4) != 4 || magic != 0x000003F3UL)
            why = "is not an Amiga program.";
        if (f) Close(f);
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return why;
}

static int run_picked(void)
{
    static char drawer[256], file[128], path[400], cmd[420], msg[480];
    if (GetVar((STRPTR)RUNDIR_VAR, drawer, sizeof drawer, 0) <= 0) drawer[0] = 0;
    file[0] = 0;
    AslBase = OpenLibrary("asl.library", 37);
    if (!AslBase) { PutStr("agaboot: no asl.library for the file requester\n"); return 10; }
    struct FileRequester *fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Run a program on the emulated chipset",
        ASLFR_InitialDrawer, (ULONG)drawer,
        ASLFR_PositiveText, (ULONG)"Run",
        ASLFR_RejectIcons, TRUE,
        TAG_DONE);
    int ok = fr && AslRequest(fr, NULL);
    if (ok) {
        strncpy(drawer, (const char *)fr->fr_Drawer, sizeof drawer - 1);
        drawer[sizeof drawer - 1] = 0;
        strncpy(file, (const char *)fr->fr_File, sizeof file - 1);
        file[sizeof file - 1] = 0;
    }
    if (fr) FreeAslRequest(fr);
    CloseLibrary(AslBase);
    AslBase = NULL;
    if (!ok || !file[0]) return 0;                /* Cancel, or no file picked */
    /* the drawer as a full path: ASL gives back what its Drawer gadget holds,
       which can be relative - and the program's shell starts in that drawer,
       so a relative one would count twice (found by review) */
    BPTR dir = Lock((STRPTR)drawer, ACCESS_READ);
    if (!dir || !NameFromLock(dir, (STRPTR)drawer, sizeof drawer)) {
        if (dir) UnLock(dir);
        snprintf(msg, sizeof msg, "The drawer %s\ncannot be found.", drawer);
        ask3("AGA-PISTORM - Run program", msg, "OK");
        return 10;
    }
    strcpy(path, drawer);
    const char *why = AddPart((STRPTR)path, (STRPTR)file, sizeof path) ? run_check(path, file)
                                                                        : "has too long a name.";
    if (why) {
        UnLock(dir);
        snprintf(msg, sizeof msg, "%s\n%s", file, why);
        ask3("AGA-PISTORM - Run program", msg, "OK");
        return 10;
    }
    SetVar((STRPTR)RUNDIR_VAR, (STRPTR)drawer, -1, GVF_GLOBAL_ONLY);
    SetVar((STRPTR)RUNDIR_VAR, (STRPTR)drawer, -1, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    snprintf(cmd, sizeof cmd, "\"%s\"", path);
    BPTR old = CurrentDir(dir);
    struct CommandLineInterface *cli = Cli();
    LONG stack = cli ? cli->cli_DefaultStack : 0;
    if (cli && cli->cli_DefaultStack < 65536 / 4) cli->cli_DefaultStack = 65536 / 4;
    int failed;
    int rc = run_program(cmd, &failed);
    if (cli) cli->cli_DefaultStack = stack;
    CurrentDir(old);
    UnLock(dir);
    if (failed) {
        snprintf(msg, sizeof msg, "%s could not be started on the\nemulated AGA chipset.", file);
        ask3("AGA-PISTORM - Run program", msg, "OK");
        return 10;
    }
    return rc;
}

/* the stack this process was given: the file requester wants more than the
   4 KB a menu may start a command with */
static ULONG own_stack(void)
{
    struct Task *t = FindTask(NULL);
    return (ULONG)t->tc_SPUpper - (ULONG)t->tc_SPLower;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !stricmp(argv[1], "BUSTEST")) return bustest(argc >= 3 && !stricmp(argv[2], "HAM6"));
    if (argc >= 2 && !stricmp(argv[1], "CHECK")) return check(argc >= 3 ? argv[2] : NULL);
    if (argc >= 2 && !stricmp(argv[1], "BOOTPROBE")) {
        /* BOOTPROBE [volume:]: what the boot partition search sees, disk by
           disk and name by name - for a FAT handler where it finds nothing */
        probe_out = 1;
        Printf("agaboot %s%s\n", (ULONG)AGA_PISTORM_VERSION, (ULONG)AGABOOT_TAG);
        if (argc >= 3) {
            char p[64];
            Printf("%s\n", (ULONG)argv[2]);
            find_ci(argv[2], "config.txt", p, sizeof p);
            find_ci(argv[2], "start4.elf", p, sizeof p);
            find_ci(argv[2], "overlays", p, sizeof p);
            return 0;
        }
        const char *b = find_boot();
        Printf("boot partition: %s\n", (ULONG)(b[0] ? b : "NOT FOUND"));
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "ABOUT")) return about();
    if (argc >= 2 && !stricmp(argv[1], "DISMOUNT")) return dismount(argc >= 3 ? argv[2] : NULL);
    if (argc >= 3 && (!stricmp(argv[1], "INSTALL") || !stricmp(argv[1], "UNINSTALL"))) {
        int install = !stricmp(argv[1], "INSTALL");
        if (!stricmp(argv[2], "HOOKS")) {
            /* INSTALL HOOKS [CREATE] [NOWHD]: CREATE makes the missing scripts
               (a plain AmigaOS 3.x); NOWHD (the AmiKit choice) leaves the two
               WHDLoad scripts without my blocks - taking out an earlier
               install's - because WHDWRAP's stand-in switches the sandbox on
               before WHDLoad and off after it, and a hook that started late
               could switch it on again over the desktop */
            int rc = 0, create = 0, nowhd = 0;
            for (int i = 3; i < argc; i++) {
                if (!stricmp(argv[i], "CREATE")) create = 1;
                else if (!stricmp(argv[i], "NOWHD")) nowhd = 1;
            }
            if (install && create) {
                if (!nowhd) {
                    create_script("S:WHDLoad-Startup", 1);
                    create_script("S:WHDLoad-Cleanup", 1);
                }
                create_script("S:User-Startup", 0);
            }
            rc |= edit_script("S:WHDLoad-Startup", setup_whd_on, 1, install && !nowhd);   /* first: see setup.c */
            rc |= edit_script("S:WHDLoad-Cleanup", setup_whd_off, 1, install && !nowhd);
            rc |= edit_script("S:User-Startup", setup_user_startup, 0, install);
            return rc ? 10 : 0;
        }
        if (!stricmp(argv[2], "CONFIG")) {
            /* INSTALL CONFIG [FT] [PAL] [PS16] [AMIKIT] [SOFTLIMIT|NOSOFTLIMIT] [file]:
               FT = a Framethrower (unicam), PAL = an NTSC Agnus that should
               run the system as PAL, PS16 = a PiStorm16 (its own section and
               kernel), AMIKIT = the installer's AmiKit choice (a config.txt
               without a [gpio17=0] section gets one of mine at its end),
               SOFTLIMIT/NOSOFTLIMIT = temp_soft_limit=70 for a Pi 3
               or not, kept for later (neither: as last time, Yes at first),
               LOWSPEED/NOLOWSPEED = the Pi 4's SD card at 25 MHz or not
               (neither: as it was) */
            const char *path = config_txt();
            unsigned opts = 0;
            int ps16 = 0, soft = -1, slow = -1;
            for (int i = 3; i < argc; i++) {
                if (!stricmp(argv[i], "FT")) opts |= 1;
                else if (!stricmp(argv[i], "PAL")) opts |= 2;
                else if (!stricmp(argv[i], "AMIKIT")) opts |= 64;     /* setup.c OPT_AMIKIT: AmiKit's own card */
                else if (!stricmp(argv[i], "PS16")) ps16 = 1;
                else if (!stricmp(argv[i], "SOFTLIMIT")) soft = 1;
                else if (!stricmp(argv[i], "NOSOFTLIMIT")) soft = 0;
                else if (!stricmp(argv[i], "LOWSPEED")) slow = 1;
                else if (!stricmp(argv[i], "NOLOWSPEED")) slow = 0;
                else path = argv[i];
            }
            if (install && soft >= 0) softlimit_set(soft);
            if (install && !softlimit_pref()) opts |= 32;       /* setup.c OPT_NO_SOFT_LIMIT */
            if (!path[0]) {
                PutStr("agaboot: no Emu68 boot partition is mounted - mount it (the FAT\n"
                       "partition with config.txt on the SD card), or name its config.txt\n");
                return 10;
            }
            /* Sound on HDMI switched on keeps its hdmi_drive=2 through an
               install again (sound_config) */
            if (install && hdmisound_pref()) opts |= 4;
            /* ...and HDMI rate its mode lines (hdmirate_set) */
            if (install && hdmirate_pref()) opts |= hdmirate_pref() == 50 ? 8 : hdmirate_pref() == 60 ? 16 : 24;
            /* the low speed option: my note in the block with it, then the
               command line made to match (lowspeed_sync); a card at 25 MHz
               of its own is left as it is */
            int lstate = 0;
            {
                char *ct = setup_read(path);
                if (ct) { lstate = lowspeed_state(path, ct); free(ct); }
            }
            int want = slow >= 0 ? slow : lstate == 1;
            if (install && want && lstate != 2) opts |= OPT_SLOW;
            int rc = edit_config(path, install, opts, ps16);
            if (rc != 20) {
                int r2 = lowspeed_sync(path, lstate);
                if (r2 > rc) rc = r2;
            }
            if (install && want && lstate == 2)
                Printf(SETUP_OK, (ULONG)"SD card", (ULONG)"at 25 MHz already (the card's own setting) - left as it is");
            return rc;
        }
        if (!install && !stricmp(argv[2], "MENU")) {
            if (file_exists(MENU_FILE)) write_menu(MENU_FILE, 1);
            for (int i = 0; i < 2; i++)
                if (file_exists(dopus_menus[i]))
                    dopus_menu_update(dopus_menus[i], "Chipset", NULL, 0);
            start_menus(1);                      /* and the Caffeine button's start menus */
            wbmenu_stop();                       /* Workbench's: gone at once */
            PutStr("  Chipset menu             removed (gone after the next reboot)\n");
            return 0;
        }
        if (!stricmp(argv[2], "WHDPREFS")) return edit_whdprefs(install);
        if (install && !stricmp(argv[2], "OVERLAYS") && argc >= 5) return install_overlays(argv[3], argv[4]);
        if (install && !stricmp(argv[2], "LEGACY")) {
            /* before Install replaces the RTG driver: VC4_LEGACY_ID or not,
               for INSTALL MONITOR after it (see monitor_install) */
            int lp = legacy_plan();
            SetVar((STRPTR)LEGACY_VAR, (STRPTR)(lp == 1 ? "1" : lp == 2 ? "2" : "0"), -1, GVF_GLOBAL_ONLY);
            Printf(SETUP_OK, (ULONG)"VC4_LEGACY_ID", (ULONG)(lp == 1 ? "to be added" : lp == 2 ? "to be switched on" :
                                                             lp < 0 ? "cannot tell (no icon.library)" : "nothing to do (on, or not needed)"));
            return 0;
        }
        if (!stricmp(argv[2], "MONITOR")) return install ? monitor_install() : monitor_uninstall();
        PutStr("usage: agaboot INSTALL HOOKS [CREATE] [NOWHD]|CONFIG [FT] [PAL] [PS16] [file]|WHDPREFS|LEGACY|MONITOR|\n"
               "                      OVERLAYS from to\n"
               "       agaboot UNINSTALL HOOKS|CONFIG [file]|WHDPREFS|MENU|MONITOR\n");
        return 5;
    }
    /* AUDMASK 0-15: which of the four emulated Paula channels reach the mix.
       Diagnostic - isolate one channel on a running game to hear it alone. */
    if (argc >= 3 && !stricmp(argv[1], "AUDMASK")) {
        LONG m = atol((const char *)argv[2]);
        if (m < 0 || m > 15) { PutStr("agaboot: AUDMASK takes 0-15\n"); return 5; }
        Disable(); AGA_CTRL = (UWORD)(0x5D00 | (m & 15)); Enable();
        Printf("agaboot: audio mask now %ld (bit n = channel n)\n", m);
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "SANDBOX")) {
        /* SANDBOX OFF <0-4>: bisect the leave. Each level runs one more step of
           aga_sandbox_leave and then returns, so a level that kills the machine
           names the step that did it. Settable at runtime on purpose - as a
           cmdline option this needed a boot, and therefore a card out of the
           Amiga and into the PC, for every rung. */
        if (argc >= 4 && !stricmp(argv[2], "OFF")) {
            int lvl = atoi(argv[3]);
            if (lvl >= 0 && lvl <= 4) {
                Disable();
                AGA_CTRL = (UWORD)(0x5B00 | lvl);
                Enable();
                Printf("agaboot: leave bisect level %ld\n", (long)lvl);
            }
        }
        /* SANDBOX ON <picture words>: size, look, X= Y= for this entry */
        if (argc >= 4 && !stricmp(argv[2], "ON")) zoom_req = args_from(argc, argv, 3);
        int rc = sandbox(argv[2]);
        /* A1kmode: this OFF is a game's cleanup - SANDBOX AUTO left System
           wide for the game - so System wide goes back on */
        if (!stricmp(argv[2], "OFF")) {
            char v[4];
            if (GetVar((STRPTR)A1K_GAME_VAR, (STRPTR)v, sizeof v, GVF_GLOBAL_ONLY) > 0) {
                DeleteVar((STRPTR)A1K_GAME_VAR, GVF_GLOBAL_ONLY);
                if (a1k_mode() && !aga_active()) rc = systemwide_on();
            }
        }
        return rc;
    }
    if (argc >= 3 && !stricmp(argv[1], "BEAM")) {
        /* BEAM NORMAL|EXACT|INTERP: how far the raster position a program
           reads may run ahead of the line the chipset core has drawn - one
           pacing step (NORMAL, the default), not at all, or up to a frame
           (1.102). Holds until the next reboot. */
        int m = !stricmp(argv[2], "EXACT") ? 1 : !stricmp(argv[2], "INTERP") ? 2
              : !stricmp(argv[2], "NORMAL") ? 0 : -1;
        static const char *const bn[3] = { "NORMAL", "EXACT", "INTERP (1.102)" };
        if (m < 0) { PutStr("agaboot: BEAM NORMAL, EXACT or INTERP\n"); return 5; }
        Disable(); AGA_CTRL = (UWORD)(0x5810 | m); Enable();
        Printf("agaboot: beam %s\n", (ULONG)bn[m]);
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "WHDWRAP")) {
        /* WHDWRAP INSTALL file | UNINSTALL: the installer's AmiKit choice puts
           the stand-in (whdwrap.c; file = the package's copy) in WHDLoad's
           place, the real one renamed WHDLoad.real next to it; UNINSTALL puts
           it back. The stand-in is told by its version string, so running
           either twice never renames the stand-in over the real WHDLoad - and
           a C:WHDLoad that is not mine beside an old WHDLoad.real (WHDLoad
           updated since) becomes the real one, or on UNINSTALL stays, the
           old one deleted */
        if (!stricmp(argv[2], "INSTALL") && argc >= 4) {
            if (!exists_quiet(WHD_PATH)) { Printf(SETUP_OK, (ULONG)WHD_PATH, (ULONG)"not there - no WHDLoad to stand in for"); return 10; }
            if (!whd_is_ours(WHD_PATH)) {
                if (exists_quiet(WHD_REAL)) DeleteFile((STRPTR)WHD_REAL);
                if (!Rename((STRPTR)WHD_PATH, (STRPTR)WHD_REAL)) { Printf(SETUP_OK, (ULONG)WHD_PATH, (ULONG)"CANNOT RENAME - left alone"); return 10; }
                Printf(SETUP_OK, (ULONG)WHD_REAL, (ULONG)"the real WHDLoad, renamed");
            }
            if (!copy_binary(argv[3], WHD_PATH)) {
                /* no stand-in: the real one back where it was */
                DeleteFile((STRPTR)WHD_PATH);
                Rename((STRPTR)WHD_REAL, (STRPTR)WHD_PATH);
                Printf(SETUP_OK, (ULONG)WHD_PATH, (ULONG)"CANNOT COPY the stand-in - the real one put back");
                return 10;
            }
            Printf(SETUP_OK, (ULONG)WHD_PATH, (ULONG)"AGA-PISTORM's stand-in (enters the sandbox before WHDLoad)");
            return 0;
        }
        if (!stricmp(argv[2], "UNINSTALL")) {
            if (exists_quiet(WHD_REAL) && (!exists_quiet(WHD_PATH) || whd_is_ours(WHD_PATH))) {
                DeleteFile((STRPTR)WHD_PATH);
                if (!Rename((STRPTR)WHD_REAL, (STRPTR)WHD_PATH)) { Printf(SETUP_OK, (ULONG)WHD_REAL, (ULONG)"CANNOT RENAME BACK"); return 10; }
                Printf(SETUP_OK, (ULONG)WHD_PATH, (ULONG)"the real WHDLoad again");
            } else if (exists_quiet(WHD_REAL) && !whd_is_ours(WHD_REAL)) {
                /* WHDLoad updated since the install: C:WHDLoad is the new one,
                   and WHDLoad.real the one it replaced */
                DeleteFile((STRPTR)WHD_REAL);
                Printf(SETUP_OK, (ULONG)WHD_REAL, (ULONG)"left over from before a WHDLoad update - deleted");
            } else Printf(SETUP_OK, (ULONG)WHD_PATH, (ULONG)"nothing to restore");
            return 0;
        }
        return 5;
    }
    if (argc >= 4 && !stricmp(argv[1], "NEWER")) {
        /* NEWER installed mine: rc 5 when the installed file's $VER version
           is newer than mine - the installer then leaves it (AmiKit's
           VideoCore 1.9 against this package's 1.5); 0 otherwise, also when
           either cannot be read, as installing always did */
        long vi = ver_of(argv[2]), vo = ver_of(argv[3]);
        if (vi && vo && vi > vo) {
            Printf("%s is %ld.%ld, newer than this package's %ld.%ld - kept\n",
                   (ULONG)argv[2], vi / 1000, vi % 1000, vo / 1000, vo % 1000);
            return 5;
        }
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "WEDGETEST") && !stricmp(argv[2], "YES")) {
        /* WEDGETEST YES: the kernel's housekeeper takes the bus lock and never
           gives it back, the way a PiStorm16 tester's machine wedged. The
           Amiga stops; Ctrl-Amiga-Amiga must still reboot it, through a core
           that sees the reset line itself, and with AGA_DIAGLOG=1 the next
           boot's C:AGA-diag.log says which core took it. Not in the usage
           text: it hangs the machine on purpose. */
        PutStr("agaboot: wedging the bus now. Press Ctrl-Amiga-Amiga; it should\n"
               "reboot, and C:AGA-diag.log should then name the core that took it.\n");
        Delay(50);
        Disable(); AGA_CTRL = 0x52ED; Enable();
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "CODEDIRECT")) {
        /* CODEDIRECT ON|OFF: from the next game, code in chip RAM is read
           where it lives (ON, the default) or through Emu68's software cache
           as before 1.107 (OFF). An A/B switch; holds until the next reboot. */
        int on = !stricmp(argv[2], "ON") ? 1 : !stricmp(argv[2], "OFF") ? 0 : -1;
        if (on < 0) { PutStr("agaboot: CODEDIRECT ON or OFF\n"); return 5; }
        Disable(); AGA_CTRL = (UWORD)(0x5940 | on); Enable();
        Printf("agaboot: chip RAM code %s, from the next game\n",
               (ULONG)(on ? "read where it lives" : "read through the software cache"));
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "BLITTIMING")) {
        /* BLITTIMING REAL|FAST|NOHOLD|FASTNOHOLD: blits report finished after
           their real duration or a fixed 9 microseconds (FAST, 1.101); NOHOLD
           raises every completion at once even while the last BLIT is
           unanswered (1.102). Holds until the next reboot. */
        static const char *const names[4] = { "REAL", "FAST", "NOHOLD", "FASTNOHOLD" };
        int m = -1;
        for (int i = 0; i < 4; i++) if (!stricmp(argv[2], names[i])) m = i;
        if (m < 0) { PutStr("agaboot: BLITTIMING REAL, FAST, NOHOLD or FASTNOHOLD\n"); return 5; }
        Disable(); AGA_CTRL = (UWORD)(0x5800 | m); Enable();
        Printf("agaboot: blit timing %s\n", (ULONG)names[m]);
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "ZOOM") && !stricmp(argv[2], "ASK"))
        return picture_window();             /* Chipset menu - Output - RTG Image (picture.c) */
    if (argc >= 2 && !stricmp(argv[1], "OUTPUT")) {
        /* OUTPUT [ASK]: the Chipset menu's Output - where the picture goes */
        int r = ask3("AGA-PISTORM - Output",
                     "Where the emulated chipset's picture goes:\n\n"
                     "RTG Image: on HDMI - its size, look,\n"
                     "scanlines, flicker fixer, border,\n"
                     "brightness, PAL/NTSC, per game.\n\n"
                     "RGB port: the game on the Amiga's own\n"
                     "monitor too, as HAM6.",
                     "RTG Image...|RGB port...|Cancel");
        if (r == 1) return picture_window();
        if (r == 2) return rgbport_window();
        return 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "ZOOM")) {
        /* ZOOM [FULL|720x272|640x256] [SHARP|SMOOTH|TV]
           [4:3|INTEGER|FILL|5:4|16:9|16:10|21:9|32:9]
           [SCANLINES=0-4] [LACED=0-4] [NTSCLINES=PAL|0-4] [SCANSHAPE=EVEN|CRISP|WHOLE] [MASK=0-4]
           [FLICKERFIX=ON|OFF] [BRIGHTNESS=0-4] [GAMMA=EXACT|CRT|LIGHTER]
           [BORDER=0-10] [BORDERX=] [BORDERY=] [PAL|NTSC] [X=n] [Y=n]
           [CRTLOOK=1084S|NONE|CUSTOM|SAVE]: change the full-screen picture
           now, and for every game until the next reboot. The words not
           given keep what ENV:AGA_ZOOM says. CRTLOOK= is RTG Image's buttons
           under CRT look (1.118, RGB2RTG 0.76-dev83): a Commodore 1084S's
           look, none, the user's own - or SAVE, this look kept as that.
           GAME=<drawer> first: that game's own picture instead, saved (it
           starts from all games' when it has none); GAME=<drawer> alone
           shows it, GAME=<drawer> REMOVE gives the game all games' again. */
        static struct aga_picture pic;       /* static: off main's frame (picture.c) */
        static char v[PICTURE_TEXT];
        int crt_save = 0;
        for (int i = 2; i < argc; i++) {
            if (!stricmp(argv[i], "CRTLOOK=SAVE")) crt_save = 1;
            if (!stricmp(argv[i], "CRTLOOK=CUSTOM") && !picture_crt_custom_has()) {
                PutStr("agaboot: no Custom look saved yet - RTG Image's Save as Custom look, or ZOOM CRTLOOK=SAVE\n");
                return 10;
            }
        }
        if (!strnicmp(argv[2], "GAME=", 5)) {
            const char *key = argv[2] + 5;
            if (!key[0]) { PutStr("agaboot: ZOOM GAME=<the game's drawer> [words|REMOVE]\n"); return 5; }
            if (argc >= 4 && !stricmp(argv[3], "REMOVE")) {
                picture_forget_game(key);
                Printf("agaboot: %s gets the all-games picture again\n", (ULONG)key);
                return 0;
            }
            picture_load(&pic, 0);
            int own = picture_load_game(&pic, key);
            if (argc == 3) {
                picture_format(&pic, v);
                Printf("agaboot: %s: %s%s\n", (ULONG)key, (ULONG)v, (ULONG)(own ? "" : " (all games')"));
                return 0;
            }
            if (!picture_parse(args_from(argc, argv, 3), &pic)) {
                PutStr("agaboot: ZOOM GAME=<drawer> and picture words - see ZOOM\n");
                return 5;
            }
            if (!picture_store_game(&pic, key, 1)) {
                Printf("agaboot: could not keep %s's picture\n", (ULONG)key);
                return 10;
            }
            picture_format(&pic, v);
            Printf("agaboot: %s: %s, saved\n", (ULONG)key, (ULONG)v);
            if (crt_save) PutStr("agaboot: this CRT look saved as your Custom look (CRTLOOK=CUSTOM puts it back)\n");
            return 0;
        }
        picture_load(&pic, 0);
        if (!picture_parse(args_from(argc, argv, 2), &pic)) {
            PutStr("agaboot: ZOOM FULL|720x272|640x256|CUSTOM|AUTO SHARP|SMOOTH|TV\n"
                   "         4:3|INTEGER|FILL|5:4|16:9|16:10|21:9|32:9 SCANLINES=0-4\n"
                   "         LACED=0-4 SCANSHAPE=EVEN|CRISP|WHOLE MASK=0-4 FLICKERFIX=ON|OFF\n"
                   "         BRIGHTNESS=0-4 GAMMA=EXACT|CRT|LIGHTER BORDER=0-10 PAL|NTSC X=n Y=n\n"
                   "         CUT=l,t,r,b NTSCCUT=l,t,r,b NTSCLINES=PAL|0-4\n"
                   "         CRTLOOK=1084S|NONE|CUSTOM|SAVE\n"
                   "         (any of them); GAME=<drawer> ... for one game\n");
            return 5;
        }
        picture_send(&pic);
        picture_border_desktop(pic.bx, pic.by);     /* the TV border: the desktop's too */
        /* and for the games started after it: every entry sets the picture
           again from AGA_ZOOM, so without this a ZOOM typed on the desktop
           was undone by the very next game. Not saved - SetEnv SAVE, or RTG
           Image's Save, does that. */
        picture_store(&pic, 0);
        picture_format(&pic, v);
        Printf("agaboot: picture %s\n", (ULONG)v);
        if (crt_save) PutStr("agaboot: this CRT look saved as your Custom look (CRTLOOK=CUSTOM puts it back)\n");
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "POSITION")) {
        /* RTG Image's Position... straight from a Shell: saved when kept */
        static struct aga_picture pic;
        static char v[PICTURE_TEXT];
        picture_load(&pic, 0);
        if (!picture_position(&pic)) { PutStr("agaboot: position left alone\n"); return 0; }
        picture_store(&pic, 1);
        picture_format(&pic, v);
        Printf("agaboot: picture %s, saved\n", (ULONG)v);
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "CROP")) {
        /* RTG Image's Crop... straight from a Shell: Size Custom, saved when kept */
        static struct aga_picture pic;
        static char v[PICTURE_TEXT];
        picture_load(&pic, 0);
        if (!picture_crop(&pic)) { PutStr("agaboot: crop left alone\n"); return 0; }
        picture_store(&pic, 1);
        picture_format(&pic, v);
        Printf("agaboot: picture %s, saved\n", (ULONG)v);
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "PROBE")) return ptr_probe(argc >= 3 ? argv[2] : "manual");
    if (argc >= 3 && !stricmp(argv[1], "REDIRECT")) {
        /* REDIRECT 0|1|2: chip RAM through Emu68's abort handler with the real
           chipset left running (1 = top 64 KB, 2 = all of it, 0 = off). The
           mechanism sandbox entry relies on, tested on its own. */
        int n = atoi(argv[2]);
        if (n < 0 || n > 2) return 5;
        CacheClearU();
        Disable();
        AGA_CTRL = (UWORD)(0x5C00 | n);
        Enable();
        Printf("agaboot: chip RAM redirect %s\n",
               (ULONG)(n == 0 ? "OFF (window copied back)" : n == 1 ? "ON for the top 64 KB" : "ON for ALL chip RAM"));
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "AGAGAMES")) {
        /* AGAGAMES ON|OFF for scripts; no argument, or ASK, opens a requester.
           The menu entry uses ASK on purpose. Putting the state in the menu
           LABEL does not work: the label is written into the menu file, and
           ToolsDaemon and Dopus only read that file when they start. So a
           label saying "- Active" kept claiming the switch was on for the rest
           of the session after it had been turned off - the tool was right and
           the menu was lying. A requester reads the state when it opens, so it
           cannot go stale, and it also gives the confirmation that clicking a
           menu item otherwise never showed. */
        if (argc >= 3 && !stricmp(argv[2], "ON"))  arm_set(1);
        else if (argc >= 3 && !stricmp(argv[2], "OFF")) arm_set(0);
        else if (argc < 3 || !stricmp(argv[2], "ASK")) {
            /* One item for the whole Chipset menu. Picking a mode also turns
               the emulation on, because that is plainly what picking one
               means - nobody chooses a chipset for games that are about to
               run on the real one. "Always AGA" is deliberately not here:
               Automatic already gives every AGA game AGA, so a fourth button
               would earn its space about once a year. It is still one
               `agaboot ECSGAMES OFF` away in a Shell. */
            {   /* The kernel on the card was installed for the other Pi: say so
                   before anything else. A Pi 3 kernel on a Pi 4 works as a Pi 4
                   one; a Pi 4 kernel on a Pi 3 runs games without the picture.
                   No AGA-PISTORM kernel at all - a reset instead of a power
                   cycle after installing, or config.txt starting another one -
                   and the toggle used to say nothing: games just ran on the
                   real chipset. */
                int kb, kr;
                kernel_boards(&kb, &kr);
                if (!kb || kb != kr) {
                    struct EasyStruct ws = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM",
                        (UBYTE *)(!kb ? "The AGA-PISTORM kernel is NOT running,\n"
                                        "so games run on the real chipset.\n\n"
                                        "Power cycle the Amiga after installing\n"
                                        "(Ctrl-Amiga-Amiga is not enough). If\n"
                                        "it still does not run, config.txt\n"
                                        "starts another kernel: run the\n"
                                        "AGA-PISTORM installer again."
                                : kb == 3 ? "This AGA-PISTORM kernel was installed\n"
                                            "for a Raspberry Pi 3, but the PiStorm\n"
                                            "has a Pi 4 now.\n\n"
                                            "Games still run. Run the AGA-PISTORM\n"
                                            "installer again for the Pi 4 kernel."
                                          : "This AGA-PISTORM kernel was installed\n"
                                            "for a Raspberry Pi 4, but the PiStorm\n"
                                            "has a Pi 3 now.\n\n"
                                            "Games run without the AGA picture.\n"
                                            "Run the AGA-PISTORM installer again\n"
                                            "for the Pi 3 kernel."),
                        (UBYTE *)"OK" };
                    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
                    if (IntuitionBase) {
                        EasyRequestArgs(NULL, &ws, NULL, NULL);
                        CloseLibrary((struct Library *)IntuitionBase);
                    }
                }
            }
            if (kernel_stale()) {
                /* installed, then reset instead of switched off: the old
                   kernel still runs beside the new agaboot (1.117) */
                static char sm[320];
                snprintf(sm, sizeof sm, "The AGA-PISTORM kernel running is older\n"
                                        "than this AGA-PISTORM (%s).\n\n"
                                        "A new kernel starts only when the Amiga\n"
                                        "is switched off and on - Ctrl-Amiga-Amiga\n"
                                        "keeps the old one. Switch it off, wait a\n"
                                        "few seconds and switch it on again.", AGA_PISTORM_VERSION);
                struct EasyStruct ks = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM", (UBYTE *)"%s", (UBYTE *)"OK" };
                ULONG arg = (ULONG)sm;
                IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
                if (IntuitionBase) {
                    EasyRequestArgs(NULL, &ks, NULL, &arg);
                    CloseLibrary((struct Library *)IntuitionBase);
                }
            }
            int on = armed();
            int pref = ecs_pref();
            struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM",
                                     (UBYTE *)(!on ? "WHDLoad games run on the REAL chipset.\n\n"
                                                     "Automatic gives each game the chipset its\n"
                                                     "WHDLoad slave asks for. ECS forces an ECS\n"
                                                     "Denise on every game."
                                               : pref < 0 ? "Games run on the emulated chipset,\n"
                                                     "AUTOMATIC: each one gets the chipset its\n"
                                                     "WHDLoad slave asks for.\n\n"
                                                     "Pick ECS only for a game that still\n"
                                                     "looks dark."
                                               : pref ? "Games run on the emulated chipset,\n"
                                                     "forced to ECS DENISE.\n\n"
                                                     "AGA games look wrong like this - go back\n"
                                                     "to Automatic for them."
                                               : "Games run on the emulated chipset,\n"
                                                     "forced to AGA.\n\n"
                                                     "Some ECS games come out very dark."),
                                     (UBYTE *)(on ? "Automatic|ECS mode|Turn off|Cancel"
                                                  : "Automatic|ECS mode|Leave off") };
            IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
            if (!IntuitionBase) return 20;
            int r = EasyRequestArgs(NULL, &es, NULL, NULL);
            CloseLibrary((struct Library *)IntuitionBase);
            if (r == 1)      { ecs_pref_set(-1); arm_set(1); }
            else if (r == 2) { ecs_pref_set(1);  arm_set(1); }
            else if (r == 3 && on) arm_set(0);
        }
        {
            int pref = ecs_pref();
            Printf("agaboot: AGA for WHDLoad games is %s, chipset %s\n",
                   (ULONG)(armed() ? "ON" : "OFF"),
                   (ULONG)(pref < 0 ? "per game" : pref ? "forced to ECS" : "forced to AGA"));
        }
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "ECSGAMES")) {
        /* Which chipset the emulation presents. Same requester-not-label
           reasoning as AGAGAMES above. Takes effect on the NEXT game: the
           word is sent at sandbox entry, so a game already running keeps
           the chipset it started with. */
        if (argc >= 3 && !stricmp(argv[2], "ON"))  ecs_pref_set(1);
        else if (argc >= 3 && !stricmp(argv[2], "OFF")) ecs_pref_set(0);
        else if (argc >= 3 && !stricmp(argv[2], "AUTO")) ecs_pref_set(-1);
        else if (argc >= 3 && !stricmp(argv[2], "?")) {
            /* What AUTO would decide for the CURRENT directory. Run it inside a
               game's drawer to see what that game will get, without launching
               it - the only way to check the slave reading on a real install. */
            int pref = ecs_pref();
            Printf("agaboot: setting %s; this directory reads as %s\n",
                   (ULONG)(pref < 0 ? "AUTO (per game)" : pref ? "always ECS" : "always AGA"),
                   (ULONG)(game_is_ecs() ? "ECS/OCS" : "AGA, or no slave to judge by"));
            return 0;
        }
        else if (argc < 3 || !stricmp(argv[2], "ASK")) {
            int pref = ecs_pref();
            struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM",
                                     (UBYTE *)(pref < 0
                                       ? "Each game gets the chipset its WHDLoad\n"
                                         "slave asks for. This is the normal\n"
                                         "setting.\n\n"
                                         "Override it only if a game looks wrong."
                                       : pref
                                       ? "Every game runs on an ECS DENISE.\n\n"
                                         "ECS games get their full palette,\n"
                                         "but AGA games look wrong."
                                       : "Every game runs on the AGA chipset.\n\n"
                                         "Some ECS games come out very dark."),
                                     (UBYTE *)"Automatic|Always ECS|Always AGA" };
            IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
            if (!IntuitionBase) return 20;
            int r = EasyRequestArgs(NULL, &es, NULL, NULL);
            CloseLibrary((struct Library *)IntuitionBase);
            if (r == 1) ecs_pref_set(-1);
            else if (r == 2) ecs_pref_set(1);
            else ecs_pref_set(0);
        }
        {
            int pref = ecs_pref();
            Printf("agaboot: emulated chipset is %s\n",
                   (ULONG)(pref < 0 ? "chosen per game" : pref ? "ECS Denise" : "AGA"));
        }
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "RGBPORT")) {
        /* RGBPORT ASK: the RGB port window (Chipset menu - RGB port).
           RGBPORT POSITION: its calibration frame straight from a Shell.
           RGBPORT HAM6|ONLY|OFF [X=n] [Y=n]: the game's picture on the
           Amiga's own RGB port as HAM6 - next to HDMI (HAM6), or with HDMI
           black (ONLY) - moved right / down by X lores pixels and Y lines.
           Off by default; saved in ENVARC:AGA_RGBPORT, used from the next game. */
        struct rgb_port rp;
        char v[64];
        if (argc < 3 || !stricmp(argv[2], "ASK")) return rgbport_window();
        rgbport_load(&rp);
        if (!stricmp(argv[2], "POSITION")) {
            if (!rgbport_position(&rp)) { PutStr("agaboot: RGB port position left alone\n"); return 0; }
        } else if (!rgbport_parse(args_from(argc, argv, 2), &rp)) {
            PutStr("agaboot: RGBPORT HAM6|ONLY|OFF 32COLOURS|4096COLOURS X=n Y=n (any of them), POSITION or ASK\n");
            return 5;
        }
        rgbport_store(&rp, 1);
        rgbport_format(&rp, v);
        Printf("agaboot: RGB port %s, saved\n", (ULONG)v);
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "SOUND")) {
        /* SOUND ASK: the Chipset menu's Sound window (sound.c). From a Shell:
           SOUND HDMI ON|OFF, SOUND FILTER A500|A1200|OFF, SOUND VOLUME
           100|150|200|300|400 (HDMI), SOUND STEREO AMIGA|75|50|25|MONO and
           SOUND SWAP ON|OFF (both outputs), SOUND PAULA CLASSIC|BANDLIMITED
           (HDMI). SOUND alone: what is set, what the kernel says. Saved in
           ENVARC: and in force at once. */
        if (argc >= 3 && !stricmp(argv[2], "MONITOR")) {   /* the Sound monitor (monitor.c) */
            static char mmsg[160];
            int r = monitor_window(mmsg, sizeof mmsg);
            if (r && mmsg[0]) {
                Printf("agaboot: %s\n", (ULONG)mmsg);
                if (!IsInteractive(Output())) sound_tell(mmsg);   /* started by the Sound window: no Shell to read it */
            }
            return r;
        }
        if (argc >= 3 && !stricmp(argv[2], "VOICES")) {    /* the voice mixer (vm.c) */
            struct vm_set vs;
            char vd[128];
            vm_load(&vs);
            const char *w = argc >= 4 ? argv[3] : "";
            int ok = 1;
            if (!*w) {
                vm_describe(&vs, vd, sizeof vd);
                Printf("agaboot: voice mixer %s\n", (ULONG)vd);
                return 0;
            }
            if (!stricmp(w, "ASK")) { vm_window(&vs); return 0; }   /* stored by the window */
            if (!stricmp(w, "ON") || !stricmp(w, "OFF")) vs.on = !stricmp(w, "ON");
            else if (!stricmp(w, "PRESET") && argc >= 5) {
                char nm[32] = "";
                for (int i = 4; i < argc; i++) strncat(nm, argv[i], sizeof nm - strlen(nm) - 1);
                int pi = vm_parse_preset(nm);
                if (pi < 0) {
                    PutStr("agaboot: SOUND VOICES PRESET - one of:");
                    for (int i = 1; vm_preset_name(i); i++) Printf("%s %s", (ULONG)(i > 1 ? "," : ""), (ULONG)vm_preset_name(i));
                    PutStr("\n");
                    return 5;
                }
                vm_preset(&vs, pi);
            }
            else if ((!stricmp(w, "PAN") || !stricmp(w, "LEVEL")) && argc >= 6 && atoi(argv[4]) >= 1 && atoi(argv[4]) <= 4) {
                int n = atoi(argv[4]) - 1, x = atoi(argv[5]);
                if (!stricmp(w, "PAN")) vs.pan[n] = (BYTE)(x < -8 ? -8 : x > 8 ? 8 : x);
                else vs.lev[n] = (BYTE)(x < 0 ? 0 : x > 16 ? 16 : x);
                vs.on = 1;
                vs.preset = 0;
            }
            else ok = 0;
            if (!ok) {
                PutStr("agaboot: SOUND VOICES ASK, ON|OFF, PRESET name, PAN voice -8..8, LEVEL voice 0..16\n");
                return 5;
            }
            vm_store(&vs, 1);
            vm_send(&vs);
            vm_describe(&vs, vd, sizeof vd);
            Printf("agaboot: voice mixer %s, saved\n", (ULONG)vd);
            return 0;
        }
        if (argc >= 3 && !stricmp(argv[2], "EQ")) {        /* the effects on HDMI (eq.c) */
            struct eq_set es;
            char ed[96];
            eq_load(&es);
            const char *w = argc >= 4 ? argv[3] : "", *v = argc >= 5 ? argv[4] : "";
            int ok = 1;
            if (!*w) {
                eq_describe(&es, ed, sizeof ed);
                Printf("agaboot: effects on HDMI: %s\n", (ULONG)ed);
                return 0;
            }
            if (!stricmp(w, "ASK")) return eq_window(&es) ? 0 : 0;   /* stored by the window */
            if (!stricmp(w, "ON") || !stricmp(w, "OFF")) es.on = !stricmp(w, "ON");
            else if (!stricmp(w, "PRESET")) {
                /* the name may come in pieces: PRESET Bass boost */
                char nm[32] = "";
                for (int i = 4; i < argc; i++) { strncat(nm, argv[i], sizeof nm - strlen(nm) - 1); }
                int pi = eq_parse_preset(nm);
                if (pi < 0) {
                    PutStr("agaboot: SOUND EQ PRESET - one of:");
                    for (int i = 1; eq_preset_name(i); i++) Printf("%s %s", (ULONG)(i > 1 ? "," : ""), (ULONG)eq_preset_name(i));
                    PutStr("\n");
                    return 5;
                }
                eq_preset(&es, pi);
            }
            else if (!stricmp(w, "LIMITER") && (!stricmp(v, "ON") || !stricmp(v, "OFF"))) es.lim = !stricmp(v, "ON");
            else if (!stricmp(w, "CROSSFEED") && (!stricmp(v, "OFF") || !stricmp(v, "LIGHT") || !stricmp(v, "STRONG"))) {
                es.xfeed = !stricmp(v, "LIGHT") ? 1 : !stricmp(v, "STRONG") ? 2 : 0;
                if (es.xfeed) es.on = 1;
            }
            else if (!stricmp(w, "BALANCE") && *v) { int b = atoi(v); es.bal = (BYTE)(b < -12 ? -12 : b > 12 ? 12 : b); }
            else if (!stricmp(w, "PREAMP") && *v) { int b = atoi(v); es.pre = (BYTE)(b < -12 ? -12 : b > 12 ? 12 : b); es.on = 1; es.preset = 0; }
            else if (!stricmp(w, "BANDS") && *v) {
                const char *q = v;
                for (int b = 0; b < EQ_BANDS && *q; b++) {
                    int g = atoi(q);
                    es.gain[b] = (BYTE)(g < -12 ? -12 : g > 12 ? 12 : g);
                    while (*q && *q != ',') q++;
                    if (*q == ',') q++;
                }
                es.on = 1;
                es.preset = 0;
            }
            else ok = 0;
            if (!ok) {
                PutStr("agaboot: SOUND EQ ASK, ON|OFF, PRESET name, LIMITER ON|OFF, CROSSFEED OFF|LIGHT|STRONG,\n"
                       "         BALANCE -12..12, PREAMP -12..12, BANDS g,g,g,g,g,g,g,g,g,g (31 Hz..16 kHz, -12..12)\n");
                return 5;
            }
            eq_store(&es, 1);
            eq_send(&es);
            eq_describe(&es, ed, sizeof ed);
            Printf("agaboot: effects on HDMI: %s, saved\n", (ULONG)ed);
            return 0;
        }
        if (argc >= 4 && !stricmp(argv[2], "HIFIVOL")) {   /* AHI programs' volume on HDMI */
            int mu, db = hifivol_pref(&mu);
            if (!hifivol_parse(argv[3], &db, &mu) || (argc >= 5 && !hifivol_parse(argv[4], &db, &mu))) {
                PutStr("agaboot: SOUND HIFIVOL -n (n dB down, 0 to 62), MUTE or UNMUTE\n");
                return 5;
            }
            hifivol_store(db, mu, 1);
            hifivol_send(db, mu);
            Printf("agaboot: HiFi volume %s%ld dB%s, saved\n", (ULONG)(db ? "-" : ""), (LONG)db, (ULONG)(mu ? ", muted" : ""));
            return 0;
        }
        int on = hdmisound_pref(), ask = 0, crc = 0, filt = -1;
        struct sound_prefs sp;
        sound_load(&sp);
        if (argc >= 4 && (!stricmp(argv[2], "VOLUME") || !stricmp(argv[2], "STEREO") || !stricmp(argv[2], "SWAP")
                          || !stricmp(argv[2], "PAULA") || !stricmp(argv[2], "RATE"))) {
            int ok = 1;
            if (!stricmp(argv[2], "VOLUME")) ok = (sp.vol = sound_parse_vol(argv[3])) >= 0;
            else if (!stricmp(argv[2], "RATE")) ok = (sp.rate = sound_parse_rate(argv[3])) >= 0;
            else if (!stricmp(argv[2], "STEREO")) ok = (sp.width = sound_parse_width(argv[3])) >= 0;
            else if (!stricmp(argv[2], "PAULA")) {
                if (!stricmp(argv[3], "CLASSIC") || !stricmp(argv[3], "BANDLIMITED")) sp.paula = !stricmp(argv[3], "BANDLIMITED");
                else ok = 0;
            }
            else if (!stricmp(argv[3], "ON") || !stricmp(argv[3], "OFF")) sp.swap = !stricmp(argv[3], "ON");
            else ok = 0;
            if (!ok) { PutStr("agaboot: SOUND VOLUME 100|150|200|300|400, STEREO AMIGA|75|50|25|MONO, SWAP ON|OFF,\n"
                              "         PAULA CLASSIC|BANDLIMITED or RATE SCREEN|ANY|96|192\n"); return 5; }
            sound_store_mix(&sp, 1);
            sound_send_mix(&sp);
            Printf("agaboot: HDMI volume %s, stereo %s, left and right %s, Paula on HDMI %s, sound rate %s, saved\n",
                   (ULONG)sound_vol_name(sp.vol), (ULONG)sound_width_name(sp.width), (ULONG)(sp.swap ? "swapped" : "as they are"),
                   (ULONG)(sp.paula ? "band-limited" : "classic"), (ULONG)sound_rate_name(sp.rate));
            return 0;
        }
        static char cmsg[512], amsg[512];                /* off the small stack the menu gives me */
        cmsg[0] = amsg[0] = 0;
        int arc = 0;
        if (argc >= 4 && !stricmp(argv[2], "AHI16")) {   /* HiFi sound: DEVS: and AHI's settings, no kernel word */
            int lv = !stricmp(argv[3], "ON") ? 1 : !stricmp(argv[3], "LOWLATENCY") ? 2 : !stricmp(argv[3], "OFF") ? 0 : -1;
            if (lv < 0) { PutStr("agaboot: SOUND AHI16 ON|LOWLATENCY|OFF\n"); return 5; }
            arc = ahi16_switch(lv, on, amsg, sizeof amsg);
            Printf("agaboot: %s\n", (ULONG)amsg);
            return arc >= 20 ? 20 : arc;
        }
        if (argc >= 4 && !stricmp(argv[2], "HDMI") && (!stricmp(argv[3], "ON") || !stricmp(argv[3], "OFF"))) {
            on = !stricmp(argv[3], "ON");
        } else if (argc >= 4 && !stricmp(argv[2], "FILTER")) {
            filt = !stricmp(argv[3], "A500") ? 1 : !stricmp(argv[3], "A1200") ? 2 : !stricmp(argv[3], "OFF") ? 0 : -1;
            if (filt < 0) { PutStr("agaboot: SOUND FILTER A500|A1200|OFF\n"); return 5; }
        } else if (argc >= 3 && !stricmp(argv[2], "ASK")) {
            int r = sound_window(&sp);
            if (r == 0) return 0;                        /* Cancel: the old mix was sent back */
            sound_store_mix(&sp, r == 2);                /* Paula, filter, volume, stereo, swap: heard already */
            if (sp.ahi16 != ahi16_pref()) arc = ahi16_switch(sp.ahi16, sp.hdmi, amsg, sizeof amsg);
            if (sp.hdmi == on) {                         /* HDMI on/off as it was: nothing more */
                if (amsg[0]) sound_tell(amsg);
                return arc >= 20 ? 5 : 0;
            }
            on = sp.hdmi;
            ask = 1;
        } else if (argc >= 3) {
            PutStr("agaboot: SOUND HDMI ON|OFF, AHI16 ON|OFF, FILTER, VOLUME, STEREO, SWAP, HIFIVOL, EQ, VOICES, MONITOR or ASK\n");
            return 5;
        }
        if (filt >= 0) {                                 /* the filter alone: no config.txt, no wait */
            hdmifilter_set(filt);
            hdmifilter_send(filt);
            Printf("agaboot: HDMI sound filter %s, saved%s\n", (ULONG)filter_names[filt],
                   (ULONG)(on ? "" : " (sound on HDMI is off)"));
            return 0;
        }
        if (argc >= 3) {
            hdmisound_set(on);
            hdmisound_send(on);
            crc = sound_config(on, cmsg, sizeof cmsg);
            /* the kernel's answer: it starts at once, or the housekeeper
               tries again within a second */
            int built, runs;
            kernel_boards(&built, &runs);
            for (int k = 0; built && k < 5; k++) {
                Delay(10);
                ULONG st = diag_read(D_HDMI_STATE);
                if (on ? st != 0 : st != 1) break;
            }
        }
        const char *why = hdmisound_why_not();
        Printf("agaboot: HDMI sound %s%s%s%s\n", (ULONG)(on ? "on" : "off"), (ULONG)(argc >= 3 ? ", saved" : ""),
               (ULONG)(why && on ? " - but " : ""), (ULONG)(why && on ? why : ""));
        if (cmsg[0]) Printf("%s\n", (ULONG)cmsg);
        if (argc < 3) {
            Printf("agaboot: HDMI sound filter %s\n", (ULONG)filter_names[hdmifilter_pref()]);
            Printf("agaboot: HDMI volume %s, stereo %s, left and right %s\n", (ULONG)sound_vol_name(sp.vol),
                   (ULONG)sound_width_name(sp.width), (ULONG)(sp.swap ? "swapped" : "as they are"));
            Printf("agaboot: Paula on HDMI %s, sound rate %s, HiFi sound (AHI) %s\n", (ULONG)(sp.paula ? "band-limited" : "classic"),
                   (ULONG)sound_rate_name(sp.rate), (ULONG)(ahi16_pref() == 2 ? "on, low latency" : ahi16_pref() ? "on" : "off"));
            int mu, db = hifivol_pref(&mu);
            Printf("agaboot: HiFi volume %s%ld dB%s\n", (ULONG)(db ? "-" : ""), (LONG)db, (ULONG)(mu ? ", muted" : ""));
            struct eq_set es;
            char ed[96];
            eq_load(&es);
            eq_describe(&es, ed, sizeof ed);
            Printf("agaboot: effects on HDMI: %s\n", (ULONG)ed);
        }
        /* a second requester, but only when something needs doing - sound
           asked for that does not play, or config.txt that could not follow -
           or HiFi sound was switched. Sound that plays needs no
           hdmi_drive=2 on this display, mounted or not. */
        if (ask && ((on && why) || crc >= 20 || amsg[0])) {
            static char body[1536];
            int top = (why && on) || crc >= 20;
            snprintf(body, sizeof body, "%s%s%s%s%s%s", why && on ? "Sound on HDMI is ON, but it does not play:\n" : "",
                     why && on ? why : "", why && on && cmsg[0] ? "\n\n" : "", top ? cmsg : "",
                     top && amsg[0] ? "\n\n" : "", amsg);
            sound_tell(body);
        }
        return crc >= 20 || arc >= 20 ? 5 : 0;
    }
    if (argc >= 3 && !stricmp(argv[1], "OVERLAY")) {
        /* OVERLAY ON|IDLE|OFF: the display registers drawn over the AGA picture.
           ON  - over every AGA screen: photograph a game that has no quit,
                 since on a classic PiStorm the reset that ends it reboots the
                 Pi and takes the kernel's own records with it
           IDLE- ON, and also on a blank plane while the sandbox idles (for a
                 game that hangs before it draws; hides WHDLoad's splash menu)
           OFF - neither (the default). Until the next reboot either way. */
        UWORD w = !stricmp(argv[2], "ON") ? 2 : !stricmp(argv[2], "IDLE") ? 3 : 0;
        Disable(); AGA_CTRL = (UWORD)(0x5F00 | w); Enable();
        Printf("agaboot: overlay %s\n", (ULONG)(w == 2 ? "ON" : w == 3 ? "ON, idle plane too" : "OFF"));
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "DISPLAY")) {
        GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
        if (!GfxBase) return 20;
        if (!aga_active()) { PutStr("agaboot: sandbox not active\n"); CloseLibrary((struct Library *)GfxBase); return 5; }
        display_on(GfxBase);
        Printf("agaboot: copper/bitplane/sprite DMA on, COP1LC = copinit $%08lx\n", (ULONG)GfxBase->copinit);
        CloseLibrary((struct Library *)GfxBase);
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "EXPERT")) {
        /* EXPERT [ASK]: the Chipset menu's Expert - System wide (on to
           SYSTEMWIDE ASK below), or Run program... (run_picked). EXPERT PICK
           goes straight to the file requester: what EXPERT runs itself as
           when the menu gave it too small a stack for one. EXPERT RUN: the
           Expert sub-menu's Run program... (menu.h) - the file requester
           too, through that same stack check. */
        static char *sw[4];
        int pick = argc >= 3 && !stricmp(argv[2], "PICK");
        int run = argc >= 3 && !stricmp(argv[2], "RUN");
        int r = (pick || run) ? 2 :
                ask3("AGA-PISTORM - Expert",
                     "For when you know what you are doing:\n\n"
                     "System wide: the whole Amiga on the\n"
                     "emulated chipset. UNSTABLE - it can\n"
                     "freeze the machine; save your work first.\n\n"
                     "Run program: one program that is not a\n"
                     "WHDLoad game on the emulated chipset,\n"
                     "picked in a file requester.",
                     "System wide...|Run program...|Cancel");
        if (r == 0) return 0;
        if (r == 2) {
            if (pick || own_stack() >= 12000) return run_picked();   /* PICK: never again */
            /* again, in a shell with a stack the file requester can use */
            struct CommandLineInterface *cli = Cli();
            LONG stack = cli ? cli->cli_DefaultStack : 0;
            if (cli && cli->cli_DefaultStack < 16384 / 4) cli->cli_DefaultStack = 16384 / 4;
            BPTR nin = Open((STRPTR)"NIL:", MODE_OLDFILE), nout = Open((STRPTR)"NIL:", MODE_NEWFILE);
            struct TagItem t[] = { { SYS_Input, (ULONG)nin }, { SYS_Output, (ULONG)nout },
                                   { SYS_UserShell, TRUE }, { NP_StackSize, 16384 }, { TAG_DONE, 0 } };
            LONG rc = (nin && nout) ? SystemTagList((STRPTR)"C:agaboot EXPERT PICK", t) : -1;
            if (nin) Close(nin);
            if (nout) Close(nout);
            if (cli) cli->cli_DefaultStack = stack;
            return rc < 0 ? 10 : (int)rc;
        }
        sw[0] = argv[0]; sw[1] = (char *)"SYSTEMWIDE"; sw[2] = (char *)"ASK"; sw[3] = NULL;
        argv = sw;
        argc = 3;
    }
    if (argc >= 2 && !stricmp(argv[1], "SYSTEMWIDE")) {
        /* The whole machine on the emulated chipset. SANDBOX ON already does
           this; the menu item only puts a warning in front of it, because
           there it is one click and the failure mode is a frozen machine
           with unsaved work on it.
             SYSTEMWIDE [ASK]   the menu's: on, A1kmode, or off when it is on
             SYSTEMWIDE A1K     A1kmode - on now and at every boot
             SYSTEMWIDE OFF     off, and A1kmode with it */
        if (argc >= 3 && !stricmp(argv[2], "OFF")) {
            a1k_set(0);
            return aga_active() ? sandbox("OFF") : 0;
        }
        if (argc >= 3 && !stricmp(argv[2], "A1K")) {
            a1k_set(1);
            return aga_active() ? 0 : systemwide_on();
        }
        if (argc < 3 || !stricmp(argv[2], "ASK")) {
            int on = aga_active(), a1k = a1k_mode();
            struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM",
                (UBYTE *)"THIS IS UNSTABLE\nit might freeze your machine",
                (UBYTE *)"Turn it on|Keep it on (A1kmode)|Cancel" };
            if (on) {
                es.es_TextFormat = (UBYTE *)(a1k ? "System wide is on, in A1kmode:\nit comes back at every boot."
                                                 : "System wide is on.");
                es.es_GadgetFormat = (UBYTE *)"Turn it off|Cancel";
            } else if (a1k) {               /* skipped at boot with the left mouse button */
                es.es_TextFormat = (UBYTE *)"A1kmode is set, but System wide\nis off this time.";
                es.es_GadgetFormat = (UBYTE *)"Turn it on|Stop A1kmode|Cancel";
            }
            IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
            if (!IntuitionBase) return 20;
            int r = EasyRequestArgs(NULL, &es, NULL, NULL);
            CloseLibrary((struct Library *)IntuitionBase);
            if (r == 0) { PutStr("agaboot: left alone\n"); return 0; }
            if (on) { a1k_set(0); return sandbox("OFF"); }       /* Turn it off */
            if (r == 2) {                                         /* Keep it on (A1kmode) / Stop A1kmode */
                a1k_set(!a1k);
                if (a1k) { PutStr("agaboot: A1kmode off\n"); return 0; }
            }
        }
        if (argc >= 3) zoom_req = args_from(argc, argv, 2);   /* SYSTEMWIDE [ASK] [picture words] */
        return systemwide_on();
    }
    if (argc >= 3 && !stricmp(argv[1], "RUN")) {
        /* RUN <program> [args...]: one ordinary AGA program on the emulated
           chipset - for a program that is not a WHDLoad game (those need
           nothing: the WHDLoad hook does it). The sandbox is entered before
           the program starts, so an AGA-only program finds AGA and opens its
           screen, and left when it ends: System wide for as long as one
           program runs, with System wide's caveat - save your work first.
           Already in the sandbox (System wide), it only runs the program.
           The program's return code is agaboot's. */
        static char cmd[512];
        cmd[0] = 0;
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            int q = !a[0] || strchr(a, ' ') != NULL;
            if (strlen(cmd) + strlen(a) + 4 >= sizeof cmd) { PutStr("agaboot: RUN - the command line is too long\n"); return 10; }
            if (cmd[0]) strcat(cmd, " ");
            if (q) strcat(cmd, "\"");
            strcat(cmd, a);
            if (q) strcat(cmd, "\"");
        }
        int failed;
        int rc = run_program(cmd, &failed);
        return failed ? 10 : rc;
    }
    if (argc >= 2 && !stricmp(argv[1], "MODE")) {
        int aga = aga_active();
        PutStr(aga ? "AGA\n" : "ECS\n");
        return aga ? 5 : 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "MENU")) {
        /* MENU runs from S:User-Startup and never returns an error: a failing
           command there ends User-Startup and the Startup-Sequence before
           LoadWB. A tester's DOpus5 user menu, in a format I do not write
           (-2), left the poor bastard at a 1> prompt (2026-10-02). For
           fuck's sake. Problems are printed, and Workbench's own menu stands
           in when no menu file was written. */
        int toolsdaemon = 0, dopus = 0, wb = 0;
        log_kept_session();                  /* at boot: a hang logs itself */
        {   /* at boot too: a saved TV border for the RTG desktop, which the
               card forgets at every reboot (the games' is sent as they start) */
            struct aga_picture pic;
            picture_load(&pic, 0);
            if (pic.bx || pic.by) picture_border_desktop(pic.bx, pic.by);
        }
        if (file_exists(MENU_FILE)) {
            if (write_menu(MENU_FILE, 0)) toolsdaemon = 1;
            else Printf("agaboot: cannot write %s\n", (ULONG)MENU_FILE);
        }
        for (int i = 0; i < 2; i++) {
            const char *m = dopus_menus[i];
            if (!file_exists(m)) continue;
            char bak[64];
            snprintf(bak, sizeof bak, "%s.pre-aga", m);
            if (!file_exists(bak)) copy_file(m, bak);
            int r = dopus_menu_update(m, "Chipset", chipset_menu, CHIPSET_N);
            /* -2: neither FORM OPUS nor FORM EPUS - left as it is */
            if (r) Printf("agaboot: cannot update %s (%ld) - left as it is\n", (ULONG)m, (long)r);
            else dopus = 1;
        }
        int start = start_menus(0) > 0;          /* CaffeineOS's Caffeine button */
        /* Workbench's own menu bar: ToolsDaemon's file covers it when the
           title went in there; otherwise Workbench's AppMenu does, from a
           process that waits for LoadWB - with Directory Opus or without,
           whose menus belong to its own windows */
        if (!toolsdaemon) {
            if (wbmenu_start()) wb = 1;
            else PutStr("agaboot: the Chipset menu for Workbench could not be started\n");
        }
        {   /* where the title went, for the Installer's last message */
            char where[96] = "";
            if (toolsdaemon) strcat(where, "ToolsDaemon");
            if (dopus) strcat(where, where[0] ? ", Directory Opus" : "Directory Opus");
            if (start) strcat(where, where[0] ? ", the start menu" : "the start menu");
            if (wb) strcat(where, where[0] ? ", Workbench" : "Workbench");
            SetVar((STRPTR)"AGAPISTORM_MENUS", (STRPTR)where, -1, GVF_GLOBAL_ONLY);
        }
        /* A1kmode: System wide from the start - unless the left mouse button
           is held while the machine boots, the way out of a setup that
           freezes in it (CIA-A PRA bit 6, port 0's fire line, low when held) */
        /* the HDMI sound filter before A1kmode's System wide: outside the
           sandbox a kernel that does not know the word cannot mistake it */
        if (hdmifilter_pref()) hdmifilter_send(hdmifilter_pref());
        {
            struct sound_prefs sp;
            sound_load(&sp);
            if (sp.vol != 2 || sp.width || sp.swap || sp.paula || sp.rate) sound_send_mix(&sp);
            int mu, db = hifivol_pref(&mu);       /* the Sound monitor's HiFi volume; the kernel starts at 0 dB */
            if (db || mu) hifivol_send(db, mu);
            struct eq_set es;                     /* the effects (eq.c); the kernel starts off and flat */
            eq_load(&es);
            if (eq_is_set(&es)) eq_send(&es);
            struct vm_set vs;                     /* the voice mixer (vm.c); the kernel starts with it off */
            vm_load(&vs);
            if (vs.on) vm_send(&vs);
        }
        if (a1k_mode() && !aga_active()) {
            if (*(volatile UBYTE *)0xBFE001 & 0x40) systemwide_on();
            else PutStr("agaboot: left mouse button held - A1kmode skipped this boot\n");
        }
        /* Sound - HDMI On and its filter are kept across boots: the kernel
           starts with both off */
        if (hdmisound_pref()) hdmisound_send(1);
        ahi16_refresh();                      /* HiFi sound: the driver this agaboot carries */
        return 0;
    }
    if (argc >= 2 && !stricmp(argv[1], "WBMENU")) {
        /* SERVE is the background process itself (wbmenu_start runs it) */
        if (argc >= 3 && !stricmp(argv[2], "SERVE")) return wbmenu_serve("Chipset", chipset_menu, CHIPSET_N);
        if (argc >= 3 && !stricmp(argv[2], "OFF")) {
            PutStr(wbmenu_stop() ? "agaboot: Chipset menu removed from Workbench\n"
                                 : "agaboot: no Chipset menu in Workbench\n");
            return 0;
        }
        if (!wbmenu_start()) { PutStr("agaboot: cannot start the Chipset menu\n"); return 10; }
        PutStr("agaboot: Chipset menu in Workbench's menu bar\n");
        return 0;
    }
    if (argc < 2 || (stricmp(argv[1], "AGA") && stricmp(argv[1], "ECS"))) {
        PutStr("usage: agaboot AGA|ECS [ASK] | MODE | MENU | WBMENU [OFF] | ABOUT\n"
               "       agaboot AGAGAMES ON|OFF|ASK   emulate AGA for WHDLoad games\n"
               "       agaboot SYSTEMWIDE [ASK|A1K|OFF] [zoom]  the whole machine on the\n"
               "                                     emulated chipset (unstable); A1K = at\n"
               "                                     every boot too, OFF ends both\n"
               "       agaboot RUN program [args]  one AGA program (not a WHDLoad game) on\n"
               "                                     the emulated chipset until it ends\n"
               "       agaboot ZOOM [FULL|720x272|640x256|CUSTOM|AUTO] [SHARP|SMOOTH|TV] [4:3|INTEGER|FILL|...]\n"
               "                    [SCANLINES=0-4] [MASK=0-4] [X=n] [Y=n] | ASK\n"
               "                                     the picture: how much of the frame fills\n"
               "                                     the screen, the look, the position\n"
               "                                     (ENV:AGA_ZOOM; ASK = the RTG Image window)\n"
               "       agaboot POSITION           move the picture on a calibration screen\n"
               "       agaboot CROP               cut the edges off the last game's picture\n"
               "                                     (size CUSTOM: CUT=l,t,r,b NTSCCUT=l,t,r,b)\n"
               "       agaboot RGBPORT HAM6|ONLY|OFF [X=n] [Y=n] | POSITION | ASK\n"
               "                                     the game on the RGB port as HAM6, too or\n"
               "                                     instead of HDMI (ONLY), and where\n"
               "       agaboot SOUND [HDMI ON|OFF | ASK]  the emulated chipset's sound on\n"
               "                                     HDMI too (ENVARC:AGA_HDMISOUND)\n"
               "       agaboot OUTPUT ASK | EXPERT ASK  the Chipset menu's Output and Expert\n"
               "       agaboot SOUND VOLUME n | STEREO w | SWAP ON|OFF  HDMI volume 100-400 %,\n"
               "                                     stereo AMIGA|75|50|25|MONO (both outputs)\n"
               "       agaboot SOUND PAULA CLASSIC|BANDLIMITED  that sound on HDMI as before, or\n"
               "                                     band-limited: no aliasing (ENVARC:AGA_HDMIPAULA)\n"
               "       agaboot SOUND RATE SCREEN|ANY|96|192  HDMI's rate: the program's, as the\n"
               "                                     screen allows or any, or fixed (ENVARC:AGA_AUDIORATE)\n"
               "       agaboot SOUND FILTER A500|A1200|OFF  that sound through an A500's or\n"
               "                                     an A1200's filters (ENVARC:AGA_HDMIFILTER)\n"
               "       agaboot SOUND AHI16 ON|OFF  HiFi sound: AHI programs on HDMI, 24 bit: the driver\n"
               "                                     into DEVS:, AHI's units 0 and Music at it\n"
               "       agaboot ECSGAMES AUTO|ON|OFF|ASK  chipset per game, or forced\n"
               "       agaboot SANDBOX ON|OFF|?   ON takes an optional zoom, OFF a bisect level 0-4\n"
               "       agaboot REDIRECT 0|1|2     chip RAM via the abort handler, chipset untouched\n");
        return 5;
    }
    int aga = stricmp(argv[1], "AGA") == 0;
    int with_ask = argc >= 3 && !stricmp(argv[2], "ASK");
    if (with_ask) {
        if (aga_active() == aga) {
            struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"AGA-PISTORM",
                                     (UBYTE *)(aga ? "AGA mode is already active." : "ECS mode is already active."),
                                     (UBYTE *)"OK" };
            IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
            if (IntuitionBase) { EasyRequestArgs(NULL, &es, NULL, NULL); CloseLibrary((struct Library *)IntuitionBase); }
            return 0;
        }
        if (!ask(aga ? "Reboot the PiStorm into AGA mode?\n(one boot; the next reboot returns to ECS)"
                     : "Reboot the PiStorm into ECS mode?"))
            return 0;
    } else {
        PutStr(aga ? "Rebooting into AGA mode (one-shot; a normal reboot returns to ECS)...\n"
                   : "Rebooting into ECS mode...\n");
        Delay(25);
    }
    Disable();
    AGA_CTRL = aga ? MAGIC_AGA : MAGIC_ECS;
    /* if I am still here the Emu68 build does not support the switch, so
       much for that shit */
    Enable();
    Delay(50);
    PutStr("This Emu68 does not support the AGA/ECS switch (no AGA-PISTORM build?)\n");
    return 10;
}
