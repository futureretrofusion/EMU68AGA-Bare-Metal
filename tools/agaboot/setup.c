/* setup.c - install / uninstall the AGA-PISTORM hooks and config.txt line.
 *
 * Everything the installer changes is a text edit, and every bloody edit is
 * done here rather than in the Installer script, because that miserable
 * Installer language has no way to edit a file in place. Portable C (stdio
 * only), so the exact code that runs on the Amiga is unit-tested on the host
 * against copies of real CaffeineOS files - see HOST_TEST at the bottom.
 *
 * What is changed, and how each change is undone:
 *
 *   S:WHDLoad-Startup   my ;BEGIN/;END AGA-PISTORM block appended
 *                       (agaboot SANDBOX AUTO: enter the AGA sandbox if the
 *                       toggle is on, do nothing if it is off)
 *   S:WHDLoad-Cleanup   the same kind of block, after the header comments
 *                       (agaboot SANDBOX OFF: back to the real chipset)
 *   S:User-Startup      a block that rebuilds the Chipset menu at boot
 *   EMU68:config.txt    in each board's section - [gpio17=0] for the classic
 *                       PiStorm, [gpio24=1] for the PiStorm16 - the kernel=
 *                       line points at that board's AGA kernel (both at once,
 *                       see setup_config). The old line is kept, as a comment
 *                       marked #AGA-PISTORM-WAS:, and uninstall puts it back -
 *                       so no backup file is needed to undo it. A kernel=
 *                       line every board reads after those sections would
 *                       win over mine, and is kept the same way. A card with
 *                       no board sections (plain AmigaOS 3.x) gets mine
 *                       under each kernel= line it has, the same way.
 *
 * A block that is already there is REPLACED, so re-running the installer
 * updates the damn thing. Each file gets a <name>.pre-aga copy the first
 * time it is changed and never again, so that copy is always the original.
 * A config.txt whose block has lost its end line is not touched at all
 * (setup_config).
 *
 * AmigaDOS scripts must never contain CRLF (return code 20, and a
 * User-Startup that no longer boots), so the blocks are LF and the scripts
 * keep whatever the hell they had. config.txt keeps its own line endings -
 * CaffeineOS ships it CRLF.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_BEGIN ";BEGIN AGA-PISTORM"
#define TAG_END   ";END AGA-PISTORM"
#define CFG_MARK  "# AGA-PISTORM: kernel with the emulated AGA chipset. Uninstalling it restores the #AGA-PISTORM-WAS: line(s)."
#define CFG_WAS   "#AGA-PISTORM-WAS: "

/* Each board: the config.txt section whose kernel= line it boots, and the
   kernel it gets - one per board, the protocols differ and a classic kernel
   stops on the logo on a PiStorm16. CaffeineOS pulls every GPIO up and picks
   the kernel by pin level: [gpio17=0] is the classic PiStorm, [gpio24=1] the
   PiStorm16. When a board matches more than one section the later kernel=
   line wins, and [gpio17=0] comes last - so the classic board's line is
   always that one. */
static const char *const board_sect[2]   = { "[gpio17=0]", "[gpio24=1]" };
static const char *const board_kernel[2] = { "kernel=kernel/Emu68-classic-aga.gz",
                                             "kernel=kernel/Emu68-pistorm16-aga.gz" };
static int cfg_board = 0;                     /* the board the installer runs on */
void setup_board(int ps16) { cfg_board = !!ps16; }
const char *setup_section(void) { return board_sect[cfg_board]; }

/* First in S:WHDLoad-Startup, not last. WHDLoad does not wait for this
   script, and CaffeineOS's own part stops the network and waits a second:
   behind that, WHDLoad could take the machine over before agaboot ever ran,
   and the game started on the real chipset (2026-09-28). */
const char *const setup_whd_on =
    TAG_BEGIN "\n"
    "; AGA-PISTORM: run WHDLoad games on the emulated AGA chipset when the\n"
    "; Chipset menu's Init has it on. Does nothing when it is off.\n"
    "FAILAT 21\n"
    "If Exists C:agaboot\n"
    "  C:agaboot SANDBOX AUTO >NIL:\n"
    "EndIf\n"
    "FAILAT 10\n"
    TAG_END "\n";

const char *const setup_whd_off =
    TAG_BEGIN "\n"
    "; AGA-PISTORM: back to the real chipset once WHDLoad has restored the OS.\n"
    "FAILAT 21\n"
    "If Exists C:agaboot\n"
    "  C:agaboot SANDBOX OFF >NIL:\n"
    "EndIf\n"
    "FAILAT 10\n"
    TAG_END "\n";

/* FAILAT 21 round it, like the WHDLoad blocks: in S:User-Startup any error
   ends the boot before LoadWB, and an agaboot that returned 10 there left a
   tester at a 1> prompt (2026-10-02). agaboot MENU no longer fails, but this
   block must not be able to stop a boot whatever agaboot does. */
const char *const setup_user_startup =
    TAG_BEGIN "\n"
    "FAILAT 21\n"
    "If Exists C:agaboot\n"
    "  C:agaboot MENU >NIL:\n"
    "EndIf\n"
    "FAILAT 10\n"
    TAG_END "\n";

/* ---- small string helpers ------------------------------------------------ */

/* a line that starts with `tag` at column 0 */
static const char *find_line(const char *text, const char *tag)
{
    size_t n = strlen(tag);
    for (const char *p = text; p && *p; ) {
        if (!strncmp(p, tag, n)) return p;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

/* just past the line's ending: '\n', or a CR with no '\n' after it. A lone
   CR is a line ending too (a last line ending in one, or a whole file with
   classic Mac line endings). Take it for none and my comment goes in glued
   onto that line, where neither a second install nor uninstall finds the
   damn thing again */
static const char *line_end(const char *p)
{
    for (; *p; p++)
        if (*p == '\n' || (*p == '\r' && p[1] != '\n')) return p + 1;
    return p;
}

/* the line [p, e) has a line ending */
static int has_eol(const char *p, const char *e)
{
    return e > p && (e[-1] == '\n' || e[-1] == '\r');
}

/* my block in `text`, [*b, *e) - returns 0 when there is none */
static int block_span(const char *text, const char **b, const char **e)
{
    const char *s = find_line(text, TAG_BEGIN);
    if (!s) return 0;
    const char *t = find_line(s, TAG_END);
    if (!t) return 0;                               /* half a block: leave it alone */
    *b = s;
    *e = line_end(t);
    return 1;
}

static char *join3(const char *a, size_t na, const char *b, const char *c)
{
    size_t nb = strlen(b), nc = strlen(c);
    char *r = malloc(na + nb + nc + 2);
    if (!r) return NULL;
    memcpy(r, a, na);
    memcpy(r + na, b, nb);
    memcpy(r + na + nb, c, nc + 1);
    return r;
}

/* ---- AmigaDOS scripts ------------------------------------------------------ */

/* Where the leading ';' header ends. My own block starts with ';' lines
   too, so the header stops at its first line - otherwise a block right
   below the header counts as header and looks misplaced every bloody time. */
static const char *after_header(const char *text)
{
    const char *p = text;
    while (*p == ';' && strncmp(p, TAG_BEGIN, strlen(TAG_BEGIN))) p = line_end(p);
    return p;
}

/* Put `block` into `text`. where: 0 = at the end, 1 = after the leading ';'
   header comment lines. Returns a new string, or NULL when `text` already
   holds exactly this block (nothing to write). A block already in the file is
   replaced where it is - unless it belongs at the top and is not there: an
   install before 1.107 put the WHDLoad-Startup one at the end, and it moves. */
char *setup_script_set(const char *text, const char *block, int where)
{
    const char *b, *e;
    if (block_span(text, &b, &e)) {
        if (where == 1 && b != after_header(text)) {
            char *rest = join3(text, b - text, "", e);      /* the script without it */
            if (!rest) return NULL;
            char *r = setup_script_set(rest, block, where);
            free(rest);
            return r;
        }
        if ((size_t)(e - b) == strlen(block) && !strncmp(b, block, e - b)) return NULL;
        return join3(text, b - text, block, e);
    }
    if (where == 0) {
        size_t n = strlen(text);
        if (n && text[n - 1] != '\n') {
            char *t = join3(text, n, "\n", "");
            if (!t) return NULL;
            char *r = join3(t, strlen(t), block, "");
            free(t);
            return r;
        }
        return join3(text, n, block, "");
    }
    const char *p = after_header(text);
    return join3(text, p - text, block, p);
}

/* Rip my block out. NULL when there is none. */
char *setup_script_remove(const char *text)
{
    const char *b, *e;
    if (!block_span(text, &b, &e)) return NULL;
    return join3(text, b - text, "", e);
}

/* ---- config.txt ------------------------------------------------------------ */

/* ---- config.txt: the overlay block --------------------------------------------
 *
 * Right under the global cmdline= line, one marked block holding what this
 * installation needs and the card does not already have, the stubborn
 * bastard:
 *
 *   dtoverlay=pal                 NTSC Agnus: run the system as PAL. First, so
 *                                 the mode is set before unicam sizes its capture
 *   dtoverlay=unicam,boot,smooth  a Framethrower is fitted
 *   gpu_mem=128                   see below
 *   hdmi_drive=2                  Sound on HDMI is ON (agaboot SOUND): HDMI
 *                                 mode with audio, where the firmware would
 *                                 otherwise drive a display as DVI, which
 *                                 carries no sound at all
 *   [pi3] dtoverlay=sdhc,unit0=rw   the Amiga may write this partition. Each
 *         temp_soft_limit=70        (classic only: a Pi 3 B+/3 A+ keeps 1.4 GHz to 70 C)
 *   [pi02] dtoverlay=sdhc,unit0=rw  (the Zero 2 W: the Pi 3's SD, its own filter)
 *   [pi4] dtoverlay=emmc,unit0=rw   overlay also switches its SD driver ON, so
 *         force_turbo=1             each Pi gets only its own
 *         core_freq=500             (these two: classic PiStorm only)
 *   [all]
 *
 * The two clock lines are for the classic PiStorm alone, whose GPIO bus the
 * core clock times. On a PiStorm16 the installer leaves the clocks to the
 * card: core_freq=500 gave a tester's PiStorm16 trouble, which needs 400 there
 * (2026-10-02), and an installer has no goddamn business tuning anyone's
 * clocks.
 *
 * A PiStorm16 gets nothing but its kernel= line, as its tester asked
 * (2026-10-02) - with one exception: the [pi4] emmc line when the card has
 * none, without which my 1.1 kernel cannot find the SD card at all (a stock
 * CaffeineOS card has only the sdhc one). A card that already boots Emu68 1.1
 * has it, so there it really is the kernel= line alone. No gpu_mem: the AGA
 * picture lives in ARM memory (aga_glue_init), not the GPU's, so my kernel
 * needs no more than a stock one. No sdhc lines: a PiStorm16 carries a Pi 4
 * or a CM4. No pal or unicam either: the installer does not ask there. A
 * block an older install left is rebuilt to that.
 *
 * Emu68 1.0.x took sd.unit0=rw on the command line; 1.1 ignores that and knows
 * only the overlay parameter - its SD drivers read their settings from the
 * /emu68 nodes the overlay adds, and without the emmc one it cannot even find
 * the Pi 4's card. Stock CaffeineOS 9317 ships Emu68 1.0.7, and my kernel is
 * 1.1: tested on a fresh 9317 card (2026-09-10), the 1.1 kernel stopped right
 * after its splash until gpu_mem=128 and the [pi4] clock lines were added as
 * well (9317 has gpu_mem=32 and a dynamic core clock, the one that times the
 * classic PiStorm's GPIO bus); with them it boots on 9317's own firmware.
 * Each line is added only when the card lacks it; lines further down the
 * file win, so a user's own settings there still apply. Uninstall removes the
 * block, and good riddance.
 *
 * gpu_mem is the one line outside the block that is changed: any gpu_mem
 * below 128 that a Pi 4 on either board reads - CaffeineOS has gpu_mem=32 at
 * the top - is commented out as #AGA-PISTORM-WAS:, like an old kernel= line,
 * and uninstall puts it back. Adding mine alone left the file with two
 * gpu_mem lines, where only their order decided which one counted. Sloppy
 * as hell, but that is the firmware. */
#define BLK_BEGIN "# AGA-PISTORM { added by the AGA-PISTORM installer - its uninstaller removes this block"
#define BLK_END   "# AGA-PISTORM }"
/* The low speed option (1.116, RGB2RTG 924ba03): the Pi 4's SD card at 25 MHz
   instead of 50 - some cards lost single writes at 50 MHz, the worthless
   little shits. The word goes into the command line file config.txt names
   (agaboot), which my kernel and Emu68 1.0.x take it from, not onto an
   overlay line: Emu68 1.0.x hangs after its logo on a 1.1 SD overlay line
   (another board's kernel on the same card), and a second emmc overlay line
   would set the partition's access back to its read-only default. This line
   in my block says the word is mine, for Uninstall and for an install
   without the option. */
#define LOWSPEED_WORD "emmc.low_speed"
#define BLK_SLOW  "# the Pi 4's SD card at 25 MHz: AGA-PISTORM put " LOWSPEED_WORD " in the cmdline"

static int line_is(const char *q, size_t qn, const char *s)
{
    size_t n = strlen(s);
    return qn >= n && !strncmp(q, s, n);
}

/* 1 when a block start has no end line after it - a hand edit gone wrong.
   Taking such a block out would take every line to the end of the file with
   it: an uninstall gave back only what came before, said it had worked, and
   the card no longer booted (found by RGB2RTG's installer review). */
static int block_open(const char *text)
{
    int in_blk = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (line_is(q, qn, BLK_BEGIN))    in_blk = 1;
        else if (line_is(q, qn, BLK_END)) in_blk = 0;
    }
    return in_blk;
}

/* the line ending the output ends in, taken off again: a last line that had
   none gets one while a line of mine follows it, and loses the damn thing
   when that goes */
static char *unend(char *o, const char *start)
{
    if (o > start && o[-1] == '\n') o--;
    if (o > start && o[-1] == '\r') o--;
    return o;
}

/* an active (uncommented) line starting with `s`, outside my block - with
   `also` somewhere on it. For an overlay also a dtparam= line below it: the
   Pi gives a dtparam= to the overlay loaded last, and Emu68 Imager writes
   every setting on a line of its own, the awkward bastard - "dtoverlay=emmc",
   a blank line, "dtparam=unit0=rw". Such lines are the overlay's up to the
   next dtoverlay= line or [filter] (RGB2RTG 0c5af86). */
static int has_active(const char *text, const char *s, const char *also)
{
    int in_blk = 0, group = 0;                  /* group: below a dtoverlay= line that is `s` */
    int ovl = also && line_is(s, strlen(s), "dtoverlay=");
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; group = 0; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk) continue;
        int head = line_is(q, qn, s);
        if (line_is(q, qn, "dtoverlay=") || (qn && *q == '['))
            group = ovl && head;                /* another overlay, or a filter: the group ends */
        if (!head && !(group && line_is(q, qn, "dtparam="))) continue;
        if (!also) return 1;
        const char *e = line_end(p);
        for (const char *r = q; r + strlen(also) <= e; r++)
            if (!strncmp(r, also, strlen(also))) return 1;
    }
    return 0;
}

/* an active dtoverlay=<name> line outside my block, in any damn case: Emu68
   Imager writes dtoverlay=PAL and dtoverlay=NTSC, and the card's FAT finds
   pal.dtbo by either name (RGB2RTG 0c5af86) */
static int has_overlay(const char *text, const char *name)
{
    int in_blk = 0;
    size_t n = strlen(name);
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk || !line_is(q, qn, "dtoverlay=") || qn < 10 + n) continue;
        size_t k = 0;
        while (k < n && (q[10 + k] | 0x20) == (name[k] | 0x20)) k++;
        char c = qn > 10 + n ? q[10 + n] : 0;
        if (k == n && (c == 0 || c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#'))
            return 1;
    }
    return 0;
}

/* an active line starting with `s`, outside my block, that every Pi reads:
   before any filter, or under [all] */
static int has_active_every(const char *text, const char *s)
{
    int in_blk = 0, every = 1;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk) continue;
        if (*q == '[') { every = line_is(q, qn, "[all]"); continue; }
        if (every && line_is(q, qn, s)) return 1;
    }
    return 0;
}

/* an active line starting with `s`, outside my block, in a part of the file
   a Pi 4 on this board reads: before any filter, [all], [pi4] or the board's
   own section. `min` > 0: the value after `s` must also be at least that. */
static int has_active_pi4(const char *text, const char *s, long min)
{
    int in_blk = 0, applies = 1, found = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk) continue;
        if (*q == '[') {
            applies = line_is(q, qn, "[all]") || line_is(q, qn, "[pi4]") || line_is(q, qn, board_sect[cfg_board]);
            continue;
        }
        if (!applies || !line_is(q, qn, s)) continue;
        /* the last one wins, as in the firmware */
        found = min <= 0 || strtol(q + strlen(s), NULL, 10) >= min;
    }
    return found;
}

#define GPU_KEY  "gpu_mem="
#define GPU_MIN  128

/* Comment out, as #AGA-PISTORM-WAS:, every damn active gpu_mem below GPU_MIN
   that a Pi 4 on either board reads: before any filter, [all], [pi4], [cm4]
   or a board section. *kept: an active one of GPU_MIN or more stays where
   EVERY board reads it (before any filter, [all], [pi4]) - then mine is not
   needed. *changed: a line was commented out. Returns a new string. */
static char *gpu_mem_low_out(const char *text, int *kept, int *changed)
{
    size_t n = 0;
    for (const char *s = text; (s = strstr(s, GPU_KEY)); s++) n++;
    char *out = malloc(strlen(text) + n * strlen(CFG_WAS) + 1), *o = out;
    if (!out) return NULL;
    int any = 1, every = 1;                  /* before any filter: both */
    *kept = *changed = 0;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p), *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (*q == '[') {
            every = line_is(q, qn, "[all]") || line_is(q, qn, "[pi4]");
            any = every || line_is(q, qn, "[cm4]") || line_is(q, qn, board_sect[0]) || line_is(q, qn, board_sect[1]);
        } else if (any && line_is(q, qn, GPU_KEY)) {
            if (strtol(q + strlen(GPU_KEY), NULL, 10) < GPU_MIN) {
                memcpy(o, p, q - p); o += q - p;                /* indentation */
                o += sprintf(o, "%s", CFG_WAS);
                memcpy(o, q, e - q); o += e - q;
                *changed = 1;
                p = e;
                continue;
            }
            if (every) *kept = 1;
        }
        memcpy(o, p, e - p); o += e - p;
        p = e;
    }
    *o = 0;
    return out;
}

#define HDMI_DRIVE "hdmi_drive=2"

/* HDMI rate (agaboot RTG Image, 1.114): opts 8 = 1080p50, 16 = 1080p60 -
   RGB2RTG's lines. 1080p50 has only the TV mode, CEA 31, which sends TV
   (limited) range colours unless hdmi_pixel_encoding=2 asks for 0-255 - a
   PC monitor shows them grey and pale; 60 Hz is the PC mode, DMT 82, which a
   PC monitor takes where it left CEA 16 for 640 x 480, and TVs take too.
   Forced (hdmi_force_mode=1): with 0 a display without the mode still got
   no picture, the useless lump. My block comes after the card's own lines
   above it, so my lines win; a line of the card's further down wins over
   mine (agaboot says so). */
#define RATE_FULL "hdmi_pixel_encoding=2"
/* The firmware frames a TV (CEA) mode with an overscan border unless
   disable_overscan=1 says otherwise - 1080p50 is such a mode, the 60 Hz one
   (DMT 82) has none. CaffeineOS and MultibootOS set it; AmiKit's config.txt
   does not, and its Workbench came up inside a black frame on HDMI (RGB2RTG,
   2026-10-15). Only when the card says nothing about it at all. */
#define RATE_NO_OVERSCAN "disable_overscan=1"
/* 1080p at 50.2 Hz (1.118, RGB2RTG 0.76-dev81, EMM #2195): CEA 1080p50's
   porches and totals on a 149.094 MHz pixel clock, the room above 50 Hz
   that HDMI sync needs. It can only lengthen HDMI's frame, so on 1080p50's
   50.000 Hz the 312-line PAL frames of a game that clears LOF (50.08 Hz)
   were out of its reach - a frame shown twice or not at all every 12.5 s;
   at 50.2 Hz both they (+2.7 lines) and the 313-line ones (+6.3) are
   followed. A custom mode in the DMT group (mode 87): full range, no
   overscan border. PC monitors take it; a TV may not, so it is a choice of
   its own. opts 8 and 16 together; rate RATE_ROOM. */
#define RATE_ROOM 502
#define RATE_ROOM_TIMINGS "hdmi_timings=1920 1 528 44 148 1080 1 4 5 36 0 0 0 50 0 149094000 3"
static const char *const rate_keys[] = { "hdmi_group=", "hdmi_mode=", "hdmi_force_mode=", RATE_FULL, "disable_overscan=",
                                         "hdmi_timings=", NULL };

/* the lines for `rate` (50, 60 or RATE_ROOM, else none) into out, each
   ending in eol: the full range only where the card does not say it for
   every Pi already, no overscan border only where the card does not mention
   it (both only for the TV mode, 1080p50) */
static int rate_lines(char *out, int rate, const char *base, const char *eol)
{
    int n = 0;
    if (rate == 50) n += sprintf(out + n, "hdmi_group=1%shdmi_mode=31%shdmi_force_mode=1%s", eol, eol, eol);
    if (rate == 60) n += sprintf(out + n, "hdmi_group=2%shdmi_mode=82%shdmi_force_mode=1%s", eol, eol, eol);
    if (rate == RATE_ROOM)
        n += sprintf(out + n, "hdmi_group=2%shdmi_mode=87%s" RATE_ROOM_TIMINGS "%shdmi_force_mode=1%s", eol, eol, eol, eol);
    if (rate == 50 && !has_active_every(base, RATE_FULL)) n += sprintf(out + n, RATE_FULL "%s", eol);
    if (rate == 50 && !has_active(base, "disable_overscan", NULL)) n += sprintf(out + n, RATE_NO_OVERSCAN "%s", eol);
    out[n] = 0;
    return n;
}

/* one of the HDMI rate's lines in my block, the noisy bugger: q after its
   leading blanks */
static int is_rate_line(const char *q, const char *e)
{
    for (int i = 0; rate_keys[i]; i++)
        if (line_is(q, (size_t)(e - q), rate_keys[i])) return 1;
    return 0;
}

/* 1 when an active line from `p` on sets a display setting my block may set
   too: in config.txt the last one counts, so mine above it would not stick,
   the bugger (RGB2RTG 0c5af86) */
static int late_display(const char *p)
{
    static const char *const keys[] = { "hdmi_group", "hdmi_mode", "hdmi_force_mode", "hdmi_pixel_encoding",
                                        "hdmi_drive", NULL };
    for (; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        for (int i = 0; keys[i]; i++)
            if (line_is(q, qn, keys[i])) return 1;
    }
    return 0;
}

/* the first line of a block at the end of the file, which may follow a
   [filter] for one Pi; no setting of mine, so the editors pass over it */
#define BLK_ALL "[all]"

/* 1 when q (after its blanks, at most qn long) is that line - the block's
   first, which `lead` says */
static int is_lead_all(int lead, const char *q, size_t qn)
{
    return lead && line_is(q, qn, BLK_ALL);
}

/* blk (a whole block, its end line ending in eol) into base: after the first
   cmdline= line of the global section; else before the first [filter]; else
   at the end. At the end too, headed [all], when a display setting of the
   card's own comes after that point: Emu68 Imager writes no cmdline= line,
   the awkward bastard, and sets its screen mode below the overlays, after
   the first [filter], where it put my HDMI lines out of force. NULL when
   out of memory. */
static char *place_block(const char *base, const char *blk, const char *eol)
{
    const char *at = NULL, *first_sect = NULL;
    for (const char *p = base; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '[') { first_sect = p; break; }
        if (!strncmp(q, "cmdline=", 8)) { at = line_end(p); break; }
    }
    if (!at) at = first_sect ? first_sect : base + strlen(base);
    const char *body = blk;                        /* where [all] goes: after the start line */
    if (late_display(at)) {
        at = base + strlen(base);
        body = line_end(blk);
    }
    size_t head = at - base;
    char *out = malloc(strlen(base) + strlen(blk) + strlen(BLK_ALL) + 8);
    if (!out) return NULL;
    memcpy(out, base, head);
    out[head] = 0;
    int noeol = head && !has_eol(base, at);
    /* the last line had no ending (so this is the end of the file): it gets
       one, and the block's end line goes without, which tells
       setup_config_block's 1. to take that one off again - uninstall gives
       the file back byte for byte */
    if (noeol) strcat(out, eol);
    strncat(out, blk, body - blk);
    if (body != blk) { strcat(out, BLK_ALL); strcat(out, eol); }
    strcat(out, body);
    if (noeol) unend(out + strlen(out), out)[0] = 0;
    strcat(out, at);
    return out;
}

/* opts: bit 0 = Framethrower (unicam), bit 1 = NTSC Agnus (pal), bit 2 =
   Sound on HDMI (hdmi_drive=2), bits 3-4 = HDMI rate 50/60 (both: 50.2), bit 5 = leave
   temp_soft_limit out (the installer asks, 1.115). The caller has made sure
   every block start has its end line (block_open). *err: 2 out of memory. */
#define OPT_NO_SOFT_LIMIT 32
#define OPT_SLOW 128                     /* my block's note: the cmdline's LOWSPEED_WORD is mine */
char *setup_config_block(const char *text, unsigned opts, int install, int *err)
{
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    *err = 0;
    /* 1. take out any block from an earlier install, and put back the gpu_mem
          lines it commented out - so what follows starts from the file as the
          user left it, and installing twice changes nothing the second time */
    size_t cap = strlen(text) + 1024;
    char *base = malloc(cap), *o = base;
    if (!base) { *err = 2; return NULL; }
    int in_blk = 0, had = 0;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p);
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; had = 1; p = e; continue; }
        if (in_blk) {
            if (line_is(q, qn, BLK_END)) {
                in_blk = 0;
                /* the block ends the file without a line ending: it went in
                   after a last line that had none (3. below), which gets
                   its own state back */
                if (!has_eol(p, e)) o = unend(o, base);
            }
            p = e;
            continue;
        }
        if (line_is(q, qn, CFG_WAS GPU_KEY)) {
            size_t k = strlen(CFG_WAS);
            memcpy(o, p, q - p); o += q - p;                    /* indentation */
            memcpy(o, q + k, e - q - k); o += e - q - k;
            had = 1;
            p = e;
            continue;
        }
        memcpy(o, p, e - p); o += e - p;
        p = e;
    }
    *o = 0;
    if (!install) {
        if (!had) { free(base); return NULL; }
        return base;
    }
    /* 2. what is missing - gpu_mem first, as it also edits the rest of the file.
          A PiStorm16: the emmc line alone, see above. */
    int ps16 = cfg_board == 1;
    int gpu_kept = 1, gpu_changed = 0;
    if (!ps16) {
        char *g = gpu_mem_low_out(base, &gpu_kept, &gpu_changed);
        free(base);
        if (!g) { *err = 2; return NULL; }
        base = g;
    }
    /* static: the Installer runs agaboot on its own small stack, and a buffer
       this size on it crashed RGB2RTG's installer (RGB2RTG 4e90e1c). Nothing
       here is ever re-entered. */
    static char blk[1024];
    int n = sprintf(blk, "%s%s", BLK_BEGIN, eol);
    int lines = 0, filt = 0;
    if (!ps16 && (opts & 2) && !has_overlay(base, "pal"))                   { n += sprintf(blk + n, "dtoverlay=pal%s", eol); lines++; }
    if (!ps16 && (opts & 1) && !has_active(base, "dtoverlay=unicam", NULL)) { n += sprintf(blk + n, "dtoverlay=unicam,boot,smooth%s", eol); lines++; }
    if (!gpu_kept)                                                          { n += sprintf(blk + n, GPU_KEY "%d%s", GPU_MIN, eol); lines++; }
    /* Sound on HDMI (agaboot SOUND, and kept by every install after it): HDMI
       mode with audio, before the filters below so every Pi reads it. Not on
       a PiStorm16, whose config.txt keeps its own settings (see above). */
    if (!ps16 && (opts & 4) && !has_active_every(base, HDMI_DRIVE))         { n += sprintf(blk + n, HDMI_DRIVE "%s", eol); lines++; }
    /* HDMI rate (agaboot RTG Image, kept by every install after it): after
       Sound's line, before the filters so every Pi reads it */
    if (!ps16 && (opts & 24))                                               { n += rate_lines(blk + n, (opts & 24) == 24 ? RATE_ROOM : (opts & 8) ? 50 : 60, base, eol); lines++; }
    /* the Zero 2 W has the Pi 3's SD controller but its own filter: [pi3]
       does not match it, [pi02] does (a firmware too old to know [pi02] cannot
       boot a Zero 2 W at all, and skips the section on any other Pi) */
    /* A Pi 3 B+ or 3 A+ keeps its 1.4 GHz only up to 60 C, then drops to
       1.2 GHz: temp_soft_limit=70, the most the firmware allows (RGB2RTG
       0.75: a tester's Pi 3 lost ever more frames as it warmed up; the A500's
       Pi 3 A+ showed the soft limit active through Jim Power, 2026-10-10).
       Classic PiStorm only, as the clocks below; a card's own line wins, and
       the installer asks first (OPT_NO_SOFT_LIMIT: a player with no more
       cooling to add would rather keep the Pi 3's own 60 C, 2026-10-11). */
    int sdhc = !ps16 && !has_active(base, "dtoverlay=sdhc", "unit0=rw");
    int tsl = cfg_board == 0 && !(opts & OPT_NO_SOFT_LIMIT) && !has_active(base, "temp_soft_limit=", NULL);
    if (sdhc || tsl) {
        n += sprintf(blk + n, "[pi3]%s", eol);
        if (sdhc) { n += sprintf(blk + n, "dtoverlay=sdhc,unit0=rw%s", eol); lines++; }
        if (tsl)  { n += sprintf(blk + n, "temp_soft_limit=70%s", eol); lines++; }
        if (sdhc) { n += sprintf(blk + n, "[pi02]%sdtoverlay=sdhc,unit0=rw%s", eol, eol); lines++; }
        filt = 1;
    }
    int emmc = !has_active(base, "dtoverlay=emmc", "unit0=rw");
    int turbo = cfg_board == 0 && !has_active_pi4(base, "force_turbo=1", 0);   /* classic only: see above */
    int core = cfg_board == 0 && !has_active_pi4(base, "core_freq=500", 0);
    if (emmc || turbo || core) {
        n += sprintf(blk + n, "[pi4]%s", eol);
        if (emmc)  { n += sprintf(blk + n, "dtoverlay=emmc,unit0=rw%s", eol); lines++; }
        if (turbo) { n += sprintf(blk + n, "force_turbo=1%s", eol); lines++; }
        if (core)  { n += sprintf(blk + n, "core_freq=500%s", eol); lines++; }
        filt = 1;
    }
    if (filt) n += sprintf(blk + n, "[all]%s", eol);
    /* the low speed option: my note that the cmdline's word is mine */
    if (opts & OPT_SLOW) { n += sprintf(blk + n, "%s%s", BLK_SLOW, eol); lines++; }
    sprintf(blk + n, "%s%s", BLK_END, eol);
    if (!lines) {                                    /* the card already has it all */
        if (!strcmp(base, text)) { free(base); return NULL; }
        return base;
    }
    /* 3. the block in its place (place_block) */
    char *out = place_block(base, blk, eol);
    free(base);
    if (!out) { *err = 2; return NULL; }
    if (!strcmp(out, text)) { free(out); return NULL; }
    return out;
}

/* The line at p is one of my own kernel= lines - after its leading blanks,
   without its line ending. Only such a line goes with my comment above it:
   the line under the comment is anything at all once the card has been
   edited by hand. Uninstall's own failure message asks for such edits, and
   people make them, the careless bastards: the kernel= line deleted and my
   comment left, then the old line typed back under it, or a
   disable_splash=1 (a plain 3.2 card's layout) or a [pi4] header there -
   all of which an uninstall deleted with the comment, the [pi4] case
   leaving its arm_freq= to every Pi. The two names have not changed since
   the first installer (733a528). */
static int is_our_kernel(const char *p)
{
    const char *e = line_end(p);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = e - p;
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    for (int i = 0; i < 2; i++)
        if (n == strlen(board_kernel[i]) && !strncmp(p, board_kernel[i], n)) return 1;
    return 0;
}

/* my comment line: q (qn chars, leading blanks and line ending gone) - with
   any blanks a hand edit left at its end, as is_our_kernel allows them */
static int is_mark(const char *q, size_t qn)
{
    while (qn && (q[qn - 1] == ' ' || q[qn - 1] == '\t')) qn--;
    return qn == strlen(CFG_MARK) && !strncmp(q, CFG_MARK, qn);
}

/* My kernel= lines are mine wherever they are, once the file carries a mark
   of mine: a hand edit can put a blank line between the comment and the
   kernel= line, delete the comment, or copy the pair. Uninstall then left
   kernel=kernel/Emu68-classic-aga.gz active as an ordinary line - after the
   restored old one on a plain card, so it won and the card kept booting the
   AGA kernel - and the next install turned it into an #AGA-PISTORM-WAS:
   line that brought the damn thing back at every later uninstall (found by
   the RGB2RTG port's review). A file with no mark of mine never had an
   install: there such a line was typed by hand, and it is taken over like
   any other. */
static int has_our_mark(const char *text)
{
    return strstr(text, CFG_MARK) || strstr(text, CFG_WAS);
}

/* a line just dropped had no line ending, so it ended the file: the line it
   went in under had none either */
static char *drop_end(char *o, const char *start, const char *e)
{
    return e[-1] != '\n' && e[-1] != '\r' ? unend(o, start) : o;
}

static int is_kernel_line(const char *q, size_t qn) { return qn >= 7 && !strncmp(q, "kernel=", 7); }

/* a #AGA-PISTORM-WAS: kernel= line (the old line's indentation, if any,
   after the mark - as the installer has always written it) */
static int is_was_kernel(const char *q, size_t qn)
{
    size_t k = strlen(CFG_WAS);
    if (qn <= k || strncmp(q, CFG_WAS, k)) return 0;
    while (k < qn && (q[k] == ' ' || q[k] == '\t')) k++;
    return is_kernel_line(q + k, qn - k);
}

/* Line-by-line rewrite of one section (`sect`, e.g. "[gpio17=0]"). `install`
   1 points kernel= at `kernel`, 0 restores what was there. Returns a new
   string, NULL when nothing changes, and sets *err to 1 when the file has no
   such section at all (not a CaffeineOS config I understand: touch nothing,
   full stop).

   My comment and kernel= lines go wherever they are in `sect` or outside
   both board sections (a hand edit moved the pair above them); the other
   board's section is the other call's, which also keeps an install's pair
   there from the first call.

   The sections are the firmware's filters. A [gpio..] header replaces the
   gpio filter and [all] ends every filter, but any other header - [pi4],
   [cm4], [EDID=..] - is a filter of another kind, which the firmware ANDs
   with the gpio one: "[gpio17=0] [pi4] kernel=..." is still the classic
   board's line, on a Pi 4. And the firmware takes the LAST kernel= line
   that applies, so one that every board reads after the board sections
   ("[all] kernel=...") overrode mine: the installer said the card booted
   the AGA kernel, and it never bloody started. Such a line is put out of
   action as #AGA-PISTORM-WAS: too, and uninstall puts it back. */
char *setup_config_kernel(const char *text, const char *sect, const char *kernel, int install, int *err)
{
    *err = 0;
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    /* each line at most one header with my pair under it, or one kernel=
       line with the mark in front: a fixed allowance for four ran past the
       buffer with five board headers, or forty kernel= lines in one */
    size_t lines = 1;
    for (const char *p = text; *p; p++) lines += *p == '\n' || *p == '\r';
    size_t cap = strlen(text) + lines * (strlen(CFG_MARK) + strlen(kernel) + strlen(CFG_WAS) + 8) + 64;
    char *out = malloc(cap), *o = out;
    if (!out) { *err = 2; return NULL; }
    int in_sect = 0, in_board = 0, seen = 0, changed = 0, marked = has_our_mark(text);
    int past = 0, nobody = 0;          /* after a board section; under a filter neither board passes */
    const char *p = text;
    while (*p) {
        const char *e = line_end(p);
        size_t n = e - p, body = n;                   /* body: without the line ending */
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        const char *q = p;
        while (q < p + body && (*q == ' ' || *q == '\t')) q++;
        size_t qn = body - (q - p);

        if (qn && *q == '[') {
            int head = line_is(q, qn, sect), all = line_is(q, qn, "[all]");
            if (all || line_is(q, qn, "[gpio") || line_is(q, qn, "[none]")) {
                in_sect = head;
                in_board = line_is(q, qn, board_sect[0]) || line_is(q, qn, board_sect[1]);
                nobody = !in_board && !all;
            }
            if (in_board) past = 1;
            if (head) seen = 1;
            size_t h = strlen(sect), g = h;
            while (g < qn && (q[g] == ' ' || q[g] == '\t')) g++;
            if (head && g < qn && is_mark(q + g, qn - g)) {
                /* 1.112 glued my comment onto a header that ended the file
                   without a line ending: the header alone again, with the
                   line ending install gave it */
                memcpy(o, p, q + h - p); o += q + h - p;
                memcpy(o, p + body, n - body); o += n - body;
                changed = 1;
            } else {
                memcpy(o, p, n); o += n;
            }
            if (head && install) {
                /* my two lines go straight under the header - when it ended
                   the file without a line ending, so does my kernel= line */
                if (n == body) o += sprintf(o, "%s%s%s%s", eol, CFG_MARK, eol, kernel);
                else           o += sprintf(o, "%s%s%s%s", CFG_MARK, eol, kernel, eol);
                changed = 1;
            }
            p = e;
            continue;
        }
        if ((in_sect || !in_board) && (is_mark(q, qn) || (marked && is_our_kernel(p)))) {
            /* drop my own lines from any earlier install: re-inserted above.
               A kernel= line only while it is still mine. */
            o = drop_end(o, out, e);
            changed = 1;
            p = e;
            continue;
        }
        /* every board reads it, after a board section: the line my lines
           lose to, the poor buggers */
        int after_all = past && !in_board && !nobody;
        if (in_sect || after_all) {
            if (install && is_kernel_line(q, qn)) {
                o += sprintf(o, "%s", CFG_WAS);
                memcpy(o, p, n); o += n;
                changed = 1;
                p = e;
                continue;
            }
            if (!install && (in_sect ? qn > strlen(CFG_WAS) && !strncmp(q, CFG_WAS, strlen(CFG_WAS)) : is_was_kernel(q, qn))) {
                size_t k = strlen(CFG_WAS);
                memcpy(o, q + k, n - (q - p) - k); o += n - (q - p) - k;
                changed = 1;
                p = e;
                continue;
            }
        }
        memcpy(o, p, n); o += n;
        p = e;
    }
    *o = 0;
    if (!seen) { free(out); *err = 1; return NULL; }
    /* re-installing over my own install produces the identical file, every
       bloody time */
    if (!changed || !strcmp(out, text)) { free(out); return NULL; }
    return out;
}

/* A card set up by hand - a plain AmigaOS 3.x one - has no board sections,
   just kernel= lines: usually one at the top, sometimes one per Pi model
   under [pi4], [pi3] and the like. Every active kernel= line outside a
   [gpio...] section (those belong to other boards) is put out of action as
   #AGA-PISTORM-WAS:, and mine goes under EACH of them - at the top of the
   file when there is none. Uninstall puts them all back.

   Under each, not under the first alone: with [pi4] kernel=... and [pi3]
   kernel=..., mine under [pi4] only left a Pi 3 with no kernel= line at all
   (and with [pi3] first, a Pi 4). Every Pi gets the same file, though: my
   classic kernel is the Pi 4 or the Pi 3 build, whichever Pi ran the install
   (Install copies one or the other) - a Pi 4 kernel on a Pi 3 runs the games
   without the AGA picture, and agaboot says so. Still better than no kernel,
   I suppose.
   A model with no kernel= line of its own - a card with one under [pi3]
   only, run on a Pi 4 - keeps the firmware's default kernel: that card did
   not boot Emu68 on it before the install either.

   An earlier install's pair is rebuilt at each #AGA-PISTORM-WAS: kernel=
   line, the way RGB2RTG's installer does it: every old pair is dropped (its
   kernel= line only while it is still mine, see is_our_kernel) and written
   again under each such line. Installing twice changes nothing, and a card
   whose pair was deleted by hand but whose WAS line stayed gets the damn
   thing back. Uninstall puts back a WAS line wherever it is. NULL when
   nothing changes; *err 2 when out of memory. */
static char *setup_config_kernel_any(const char *text, const char *kernel, int install, int *err)
{
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    size_t lines = 1;
    for (const char *p = text; *p; p++) lines += *p == '\n' || *p == '\r';
    size_t cap = strlen(text) + lines * (strlen(CFG_MARK) + strlen(kernel) + strlen(CFG_WAS) + 8) + 64;
    char *out = malloc(cap), *o = out;
    *err = 0;
    if (!out) { *err = 2; return NULL; }
    int gpio = 0, has_ours = 0, has_kernel = 0, top = 0, marked = has_our_mark(text);
    for (const char *p = text; *p; ) {                   /* is there anything to take over? */
        const char *q = p, *e = line_end(p);
        size_t body = e - p;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        while (q < p + body && (*q == ' ' || *q == '\t')) q++;
        size_t qn = body - (q - p);
        const char *l = p;
        p = e;
        if (qn && *q == '[') { gpio = qn >= 5 && !strncmp(q, "[gpio", 5); continue; }
        if (gpio) continue;
        if (is_mark(q, qn)) has_ours = 1;
        else if (marked && is_our_kernel(l)) continue;    /* mine is not one to take over */
        else if (is_kernel_line(q, qn) || is_was_kernel(q, qn)) has_kernel = 1;
    }
    if (install && !has_kernel) {
        /* the firmware's default kernel: mine first - or, from an earlier
           install, again where my comment is */
        if (has_ours) top = 1;
        else o += sprintf(o, "%s%s%s%s", CFG_MARK, eol, kernel, eol);
    }
    gpio = 0;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p);
        size_t n = e - p, body = n;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        const char *q = p;
        while (q < p + body && (*q == ' ' || *q == '\t')) q++;
        size_t qn = body - (q - p);
        if (qn && *q == '[') gpio = qn >= 5 && !strncmp(q, "[gpio", 5);
        else if (!install && is_was_kernel(q, qn)) {
            /* a line I marked is mine to put back, wherever it is */
            size_t k = strlen(CFG_WAS);
            memcpy(o, q + k, n - (q - p) - k); o += n - (q - p) - k;
            p = e;
            continue;
        } else if (is_mark(q, qn) || (marked && is_our_kernel(p))) {
            /* an earlier install's pair, wherever a hand edit left it (under
               a [gpio..] header, say, where uninstall left that board on the
               AGA kernel): written again at its WAS line */
            if (top && !gpio && *q == '#') { o += sprintf(o, "%s%s%s%s", CFG_MARK, eol, kernel, eol); top = 0; }
            o = drop_end(o, out, e);
            p = e;
            continue;
        } else if (!gpio) {
            if (install && (is_kernel_line(q, qn) || is_was_kernel(q, qn))) {
                if (*q != '#') o += sprintf(o, "%s", CFG_WAS);  /* a WAS line stays as it is */
                memcpy(o, p, n); o += n;
                /* the last line had no ending: my kernel= line has none */
                if (n == body) o += sprintf(o, "%s%s%s%s", eol, CFG_MARK, eol, kernel);
                else           o += sprintf(o, "%s%s%s%s", CFG_MARK, eol, kernel, eol);
                p = e;
                continue;
            }
        }
        memcpy(o, p, n); o += n;
        p = e;
    }
    *o = 0;
    if (!strcmp(out, text)) { free(out); return NULL; }
    return out;
}

/* ---- S:WHDLoad.prefs --------------------------------------------------------
 *
 * WHDLoad runs S:WHDLoad-Startup and -Cleanup only when ExecuteStartup and
 * ExecuteCleanup name them. CaffeineOS sets both; WHDLoad's own sample prefs
 * has them commented out (";ExecuteStartup=S:WHDLoad-Startup"), and a plain
 * AmigaOS 3.x install may have no prefs at all. Each that is not on is
 * switched on - the sample line uncommented, or a line added at the end -
 * under a marker line uninstall reads. A line naming another command is left
 * alone and reported in *other (bit 0 startup, bit 1 cleanup): that user's
 * own script would need the AGA-PISTORM block, and that is theirs to add.
 * NULL when nothing changes. */
#define WHD_ON_MARK  ";AGA-PISTORM: the installer switched the next line on"
#define WHD_ADD_MARK ";AGA-PISTORM: the installer added the next line"
static const char *const whd_key[2] = { "ExecuteStartup=", "ExecuteCleanup=" };
static const char *const whd_val[2] = { "S:WHDLoad-Startup", "S:WHDLoad-Cleanup" };

static int ci_ncmp(const char *a, const char *b, size_t n)     /* case-blind, like WHDLoad's */
{
    for (; n; n--, a++, b++) {
        int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* line q (qn chars, leading blanks gone) sets key k: 1 to mine, 2 to another.
   The value may run the script through Execute, as CaffeineOS's own prefs
   do ("ExecuteStartup=Execute S:WHDLoad-Startup"; found by RGB2RTG,
   2026-10-10 - it was taken for another command, and the installer told
   CaffeineOS users to move the block themselves, the poor sods): blanks,
   then Execute or C:Execute and blanks, then the script's name, ending the
   value or followed by a blank or a ';' comment. */
static int whd_sets(const char *q, size_t qn, int k)
{
    static const char *const exe[2] = { "C:Execute", "Execute" };
    size_t kl = strlen(whd_key[k]), vl = strlen(whd_val[k]);
    if (qn < kl || ci_ncmp(q, whd_key[k], kl)) return 0;
    const char *v = q + kl, *end = q + qn;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    for (int i = 0; i < 2; i++) {
        size_t el = strlen(exe[i]);
        if ((size_t)(end - v) > el && !ci_ncmp(v, exe[i], el) && (v[el] == ' ' || v[el] == '\t')) {
            for (v += el; v < end && (*v == ' ' || *v == '\t'); v++) ;
            break;
        }
    }
    size_t rest = (size_t)(end - v);
    if (rest >= vl && !ci_ncmp(v, whd_val[k], vl) && (rest == vl || v[vl] == ' ' || v[vl] == '\t' || v[vl] == ';'))
        return 1;
    return 2;
}

char *setup_whdprefs(const char *text, int install, int *other)
{
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    size_t cap = strlen(text) + 2 * (strlen(WHD_ADD_MARK) + 64) + 8;
    char *out = malloc(cap), *o = out;
    if (!out) return NULL;
    int on[2] = { 0, 0 }, sample[2] = { 0, 0 };
    *other = 0;
    if (install)                                        /* what is there already */
        for (const char *p = text; *p; p = line_end(p)) {
            const char *e = line_end(p), *q = p;
            size_t body = e - p;
            while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
            while (q < p + body && (*q == ' ' || *q == '\t')) q++;
            size_t qn = body - (q - p);
            for (int k = 0; k < 2; k++) {
                int s = whd_sets(q, qn, k);
                if (s == 1) on[k] = 1;
                if (s == 2) *other |= 1 << k;
                if (qn > 1 && *q == ';' && whd_sets(q + 1, qn - 1, k) == 1) sample[k] = 1;
            }
        }
    int drop_next = 0, comment_next = 0;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p), *q = p;
        size_t n = e - p, body = n;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        while (q < p + body && (*q == ' ' || *q == '\t')) q++;
        size_t qn = body - (q - p);
        if (drop_next) { drop_next = 0; p = e; continue; }
        if (comment_next) { comment_next = 0; *o++ = ';'; memcpy(o, p, n); o += n; p = e; continue; }
        if (!install && qn == strlen(WHD_ADD_MARK) && !strncmp(q, WHD_ADD_MARK, qn)) { drop_next = 1; p = e; continue; }
        if (!install && qn == strlen(WHD_ON_MARK) && !strncmp(q, WHD_ON_MARK, qn)) { comment_next = 1; p = e; continue; }
        int k = -1;
        if (install && qn > 1 && *q == ';')
            for (int j = 0; j < 2; j++)
                if (!on[j] && !(*other & (1 << j)) && sample[j] && whd_sets(q + 1, qn - 1, j) == 1) k = j;
        if (k >= 0) {                                   /* the sample line: on */
            o += sprintf(o, "%s%s", WHD_ON_MARK, eol);
            memcpy(o, q + 1, n - (q - p) - 1); o += n - (q - p) - 1;
            on[k] = 1;
            p = e;
            continue;
        }
        memcpy(o, p, n); o += n;
        p = e;
    }
    if (install)
        for (int k = 0; k < 2; k++)
            if (!on[k] && !(*other & (1 << k))) {       /* no sample line: added */
                if (o > out && o[-1] != '\n') o += sprintf(o, "%s", eol);
                o += sprintf(o, "%s%s%s%s%s", WHD_ADD_MARK, eol, whd_key[k], whd_val[k], eol);
            }
    *o = 0;
    if (!strcmp(out, text)) { free(out); return NULL; }
    return out;
}

/* 1 when `text` has the board's section at all */
int setup_has_section(const char *text, int ps16)
{
    const char *s = board_sect[!!ps16];
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, s, strlen(s))) return 1;
    }
    return 0;
}

/* 1 when my block has lost its bloody end line (block_open): INSTALL and
   UNINSTALL CONFIG would leave the file alone. CHECK says so first, so the
   Installer can stop before it has copied anything. */
int setup_config_open(const char *text)
{
    return block_open(text);
}

/* an active line starting with `s` outside my block - the card's own, the
   sneaky sod */
int setup_config_has(const char *text, const char *s)
{
    return has_active(text, s, NULL);
}

/* the line my block holds for Sound on HDMI: q after its leading blanks, e
   its line's end */
static int is_hdmi_line(const char *q, const char *e)
{
    size_t n = e - q;
    while (n && (q[n - 1] == '\n' || q[n - 1] == '\r' || q[n - 1] == ' ' || q[n - 1] == '\t')) n--;
    return n == strlen(HDMI_DRIVE) && !strncmp(q, HDMI_DRIVE, n);
}

/* hdmi_drive=2 in config.txt: 1 in my block, 2 the card's own where every
   Pi reads it, 0 neither */
int setup_config_hdmi_state(const char *text)
{
    int in_blk = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk && is_hdmi_line(q, e)) return 1;
    }
    return has_active_every(text, HDMI_DRIVE) ? 2 : 0;
}

/* Sound on HDMI switched on or off (agaboot SOUND): hdmi_drive=2 put into my
   block or taken out of it, and nothing else, damn it. Not setup_config's
   whole install again, which also adds whatever the block lacks for the board
   agaboot takes this for - the clock lines a PiStorm16 must not get, on a
   board it could not tell. The line goes where an install with Sound on puts
   it, before the block's first [filter] (or its end line), so installing with
   Sound on and switching it on afterwards give the same file. Only on a card
   AGA-PISTORM is installed on (*err 1 otherwise: nothing there is mine to
   change); *err 3 an open block, 2 out of memory. A card without my block -
   one that had every line already - gets one for this line alone, and a
   block left with nothing else in it goes again, so on and off give the file
   back byte for byte. The card's own hdmi_drive=2 where every Pi reads it
   does the job already: nothing then, and leave it alone. NULL when nothing
   changes. */
char *setup_config_hdmi(const char *text, int on, int *err)
{
    *err = 0;
    if (block_open(text)) { *err = 3; return NULL; }
    if (!strstr(text, CFG_MARK) && !strstr(text, BLK_BEGIN)) { *err = 1; return NULL; }
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    int in_blk = 0, blocks = 0, has = 0, others = 0, lead = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; blocks++; lead = 1; continue; }
        if (!in_blk) continue;
        int la = is_lead_all(lead, q, qn);
        lead = 0;
        if (line_is(q, qn, BLK_END)) { in_blk = 0; continue; }
        if (is_hdmi_line(q, e)) has = 1;
        else if (!la && q < e && *q != '\r' && *q != '\n') others = 1;
    }
    if (on ? has || has_active_every(text, HDMI_DRIVE) : !has) return NULL;
    if (on && !blocks) {
        static char blk[256];
        snprintf(blk, sizeof blk, "%s%s%s%s%s%s", BLK_BEGIN, eol, HDMI_DRIVE, eol, BLK_END, eol);
        char *out = place_block(text, blk, eol);
        if (!out) *err = 2;
        return out;
    }
    char *out = malloc(strlen(text) + strlen(HDMI_DRIVE) + 8), *o = out;
    if (!out) { *err = 2; return NULL; }
    int put = 0;                                  /* on: in the first block only */
    in_blk = lead = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        int drop = 0;
        if (line_is(q, qn, BLK_BEGIN)) {
            in_blk = lead = 1;
            drop = !on && !others;                /* off, and nothing else in it: the block goes */
        } else if (in_blk) {
            int end = line_is(q, qn, BLK_END), la = is_lead_all(lead, q, qn);
            lead = 0;
            if (on && !put && !la && (end || *q == '[' || is_rate_line(q, e))) {   /* where an install puts it */
                o += sprintf(o, "%s%s", HDMI_DRIVE, eol);
                put = 1;
            }
            if (!on) drop = !others || is_hdmi_line(q, e);
            if (end) {
                in_blk = 0;
                /* as setup_config_block's 1.: a block that ended the file
                   without a line ending gave the line before it one */
                if (drop && !has_eol(p, e)) o = unend(o, out);
            }
        }
        if (!drop) { memcpy(o, p, e - p); o += e - p; }
    }
    *o = 0;
    return out;
}

/* an active line starting with `s` after my block's end - the card's own,
   which config.txt's later-wins rule puts over mine (HDMI rate's message) */
int setup_config_after_block(const char *text, const char *s)
{
    int after = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_END)) { after = 1; continue; }
        if (line_is(q, qn, BLK_BEGIN)) { after = 0; continue; }
        if (after && line_is(q, qn, s)) return 1;
    }
    return 0;
}

/* HDMI rate in config.txt: 50 or 60 when my block holds its lines, else 0,
   and no arguments */
int setup_config_rate_state(const char *text)
{
    int in_blk = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk && line_is(q, qn, "hdmi_mode=31")) return 50;
        if (in_blk && line_is(q, qn, "hdmi_mode=82")) return 60;
        if (in_blk && line_is(q, qn, "hdmi_mode=87")) return RATE_ROOM;
    }
    return 0;
}

/* HDMI rate switched (agaboot RTG Image): its lines put into my block or
   taken out of it, as setup_config_hdmi does Sound's - before the block's
   first [filter] (or its end line), after Sound's line, so switching it
   after an install gives what installing with it gives. rate 50, 60,
   RATE_ROOM (50.2), or 0 for none of mine. A block left with nothing else
   in it goes, and good riddance. *err as setup_config_hdmi's; NULL when
   nothing changes. */
char *setup_config_rate(const char *text, int rate, int *err)
{
    *err = 0;
    if (block_open(text)) { *err = 3; return NULL; }
    if (!strstr(text, CFG_MARK) && !strstr(text, BLK_BEGIN)) { *err = 1; return NULL; }
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    static char want[256], have[256];
    rate_lines(want, rate, text, eol);
    int in_blk = 0, blocks = 0, others = 0, lead = 0;
    size_t hn = 0;
    have[0] = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; blocks++; lead = 1; continue; }
        if (!in_blk) continue;
        int la = is_lead_all(lead, q, qn);
        lead = 0;
        if (line_is(q, qn, BLK_END)) { in_blk = 0; continue; }
        if (is_rate_line(q, e)) {
            if (hn + (size_t)(e - p) < sizeof have) { memcpy(have + hn, p, e - p); hn += e - p; have[hn] = 0; }
        } else if (!la && q < e && *q != '\r' && *q != '\n') others = 1;
    }
    if (!strcmp(want, have)) return NULL;
    if (want[0] && !blocks) {
        static char blk[512];
        snprintf(blk, sizeof blk, "%s%s%s%s%s", BLK_BEGIN, eol, want, BLK_END, eol);
        char *out = place_block(text, blk, eol);
        if (!out) *err = 2;
        return out;
    }
    char *out = malloc(strlen(text) + strlen(want) + 8), *o = out;
    if (!out) { *err = 2; return NULL; }
    int put = 0;                                  /* in the first block only */
    in_blk = lead = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        int drop = 0;
        if (line_is(q, qn, BLK_BEGIN)) {
            in_blk = lead = 1;
            drop = !want[0] && !others;           /* none, and nothing else in it: the block goes */
        } else if (in_blk) {
            int end = line_is(q, qn, BLK_END), la = is_lead_all(lead, q, qn);
            lead = 0;
            if (want[0] && !put && !la && (end || *q == '[')) {
                o += sprintf(o, "%s", want);
                put = 1;
            }
            drop = is_rate_line(q, e) || (!want[0] && !others);
            if (end) {
                in_blk = 0;
                /* as setup_config_block's 1.: a block that ended the file
                   without a line ending gave the line before it one */
                if (drop && !has_eol(p, e)) o = unend(o, out);
            }
        }
        if (!drop) { memcpy(o, p, e - p); o += e - p; }
    }
    *o = 0;
    return out;
}

/* Both passes: the kernel lines, then the overlay block. NULL when nothing at
   all changes.

   Install points BOTH boards' sections at their AGA kernels, not only the
   board it runs on. The overlay block is global, and its emmc overlay stops
   Emu68 1.0.x right after the splash: a section left on its stock 1.0.x
   kernel would hang the card on the other board - where the installer could
   then never run to fix it. With both lines, one install serves a card that
   moves between a classic PiStorm and a PiStorm16; on a card that never
   does, the other section simply never matches. *err is 1 when the running
   board's own section is missing (touch nothing); the other board's is set
   when it is there. Uninstall restores both, whichever board runs it.
   *err 2: out of memory. *err 3: a block start without its end line (see
   block_open) - touch nothing, install or uninstall. */
static char *setup_config_sections(const char *text, int install, unsigned opts, int *err);

/* AmiKit's own card, the awkward bastard, has no [gpio17=0] section: its
   classic kernel sits under [gpio24=1], the PiStorm16's section here, and a
   classic install stopped at "no [gpio17=0] section". With the installer's
   AmiKit choice (OPT_AMIKIT) the install adds a section of mine at the end
   of the file, between these two lines - after everything else, so it wins
   on a classic PiStorm - and the normal path then puts my kernel under its
   header. [all] first: the file may end inside a filter (AmiKit's ends in
   [gpio24=1]); [all] after, so nothing added later is filtered by it. Added
   on either board: a PiStorm16 install points [gpio24=1] at the PiStorm16
   kernel, and a classic PiStorm (whose GPIO24 reads high too) needs my
   section after it. Uninstall takes the whole tail out, kernel line and all,
   whichever board runs it, and gives the file back byte for byte.
   MultibootOS's config_amikit.txt has a [gpio17=0] section and never gets
   one, thank hell. */
#define OPT_AMIKIT 64
#define TAIL_BEGIN "# AGA-PISTORM last { a [gpio17=0] section for a card without one (AmiKit) - its uninstaller removes this block"
#define TAIL_END   "# AGA-PISTORM last }"

/* where the tail starts and where it ends (past its end line), or 0 */
static int tail_find(const char *text, const char **b, const char **e)
{
    *b = *e = NULL;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (!*b && line_is(q, qn, TAIL_BEGIN)) *b = p;
        else if (*b && line_is(q, qn, TAIL_END)) { *e = line_end(p); return 1; }
    }
    return 0;
}

/* the file with the tail at its end. A last line without a line ending gets
   one, and the tail's end line goes without - which tells tail_remove to
   take that one off again */
static char *tail_add(const char *text)
{
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    size_t n = strlen(text);
    int ended = !n || text[n - 1] == '\n';
    char *out = malloc(n + strlen(TAIL_BEGIN) + strlen(TAIL_END) + 64);
    if (!out) return NULL;
    memcpy(out, text, n + 1);
    if (!ended) strcat(out, eol);
    sprintf(out + strlen(out), "%s%s[all]%s%s%s[all]%s%s%s", TAIL_BEGIN, eol, eol, board_sect[0], eol, eol,
            TAIL_END, ended ? eol : "");
    return out;
}

static char *tail_remove(const char *text)
{
    const char *b, *e;
    if (!tail_find(text, &b, &e)) return NULL;
    char *out = malloc(strlen(text) + 1);
    if (!out) return NULL;
    size_t head = b - text;
    memcpy(out, text, head);
    out[head] = 0;
    if (!(e > text && e[-1] == '\n')) *unend(out + head, out) = 0;   /* the file ended in the tail */
    strcat(out, e);
    return out;
}

/* The PiStorm32-lite's last block (1.116, RGB2RTG ca01950). config.txt picks
   the kernel by pin level and the last kernel= that matches wins. On
   CaffeineOS the classic PiStorm's [gpio17=0] comes after the
   PiStorm32-lite's [gpio4=0] - and a PiStorm32-lite can read GPIO17 low at
   power-on too (its FPGA gets its firmware only later, from Emu68): now and
   then RGB2RTG's A1200 started the classic kernel and stopped at its logo,
   the useless sod. On a card shared with an A1200 that would be my classic
   AGA kernel. So where a [gpio..] section after the card's [gpio4=0] sets a
   kernel= of its own, the install ends the file with the [gpio4=0] kernel
   again, between these lines - only [gpio4=0]: a classic PiStorm reads GPIO4
   high and keeps its kernel. Rebuilt at every install, so it stays the
   file's last lines; Uninstall takes it out byte for byte. */
#define G4_BEGIN "# AGA-PISTORM gpio4 { the PiStorm32-lite boots its own kernel whatever GPIO17 reads - its uninstaller removes this block"
#define G4_END   "# AGA-PISTORM gpio4 }"

static int g4_find(const char *text, const char **b, const char **e)
{
    *b = *e = NULL;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (!*b && line_is(q, qn, G4_BEGIN)) *b = p;
        else if (*b && line_is(q, qn, G4_END)) { *e = line_end(p); return 1; }
    }
    return 0;
}

static char *g4_remove(const char *text)
{
    const char *b, *e;
    if (!g4_find(text, &b, &e)) return NULL;
    char *out = malloc(strlen(text) + 1);
    if (!out) return NULL;
    size_t head = b - text;
    memcpy(out, text, head);
    out[head] = 0;
    if (!(e > text && e[-1] == '\n')) *unend(out + head, out) = 0;   /* the file ended in the block */
    strcat(out, e);
    return out;
}

/* The card's [gpio4=0] kernel= value into val, when a [gpio..] section after
   that one sets a kernel= of its own (my blocks' lines left out, the AmiKit
   tail's counted: it is a [gpio17=0] section after it too); else 0 */
static int g4_needed(const char *text, char *val, int len)
{
    int in_blk = 0, sect4 = 0, gpio_sect = 0, later = 0;
    val[0] = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk) continue;
        if (*q == '[') {
            sect4 = line_is(q, qn, "[gpio4=0]");
            gpio_sect = !strncmp(q, "[gpio", 5);
            continue;
        }
        if (!line_is(q, qn, "kernel=")) continue;
        if (sect4 && !val[0]) {
            const char *v = q, *w = e;
            while (w > v && (w[-1] == '\n' || w[-1] == '\r' || w[-1] == ' ' || w[-1] == '\t')) w--;
            snprintf(val, len, "%.*s", (int)(w - v), v);
        } else if (val[0] && gpio_sect && !sect4) later = 1;
    }
    return val[0] && later;
}

/* text with the block at its end ([all] first: the file may end inside a
   filter; [all] after, so nothing added later is filtered by it). A last
   line without a line ending gets one, and the block's end line goes
   without - which tells g4_remove to take that one off again. */
static char *g4_add(const char *text, const char *kline)
{
    const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
    size_t n = strlen(text);
    int ended = !n || text[n - 1] == '\n';
    char *out = malloc(n + strlen(G4_BEGIN) + strlen(G4_END) + strlen(kline) + 64);
    if (!out) return NULL;
    memcpy(out, text, n + 1);
    if (!ended) strcat(out, eol);
    sprintf(out + strlen(out), "%s%s[all]%s[gpio4=0]%s%s%s[all]%s%s%s", G4_BEGIN, eol, eol, eol, kline, eol, eol,
            G4_END, ended ? eol : "");
    return out;
}

static char *setup_config_tails(const char *text, int install, unsigned opts, int *err);

/* config.txt with the PiStorm32-lite's block taken out first and, on an
   install that needs it, put back as the file's last lines - after the
   AmiKit tail and anything else. NULL when nothing changes. */
char *setup_config(const char *text, int install, unsigned opts, int *err)
{
    *err = 0;
    if (block_open(text)) { *err = 3; return NULL; }
    char *t0 = g4_remove(text);                       /* NULL: there was none */
    const char *src = t0 ? t0 : text;
    char *r = setup_config_tails(src, install, opts, err);
    if (*err) { free(t0); return NULL; }
    const char *now = r ? r : src;
    static char kline[200];                           /* off the Installer's small stack */
    char *out;
    if (install && g4_needed(now, kline, sizeof kline)) out = g4_add(now, kline);
    else if ((out = malloc(strlen(now) + 1)) != NULL) strcpy(out, now);
    free(r);
    free(t0);
    if (!out) { *err = 2; return NULL; }
    if (!strcmp(out, text)) { free(out); return NULL; }
    return out;
}

static char *setup_config_tails(const char *text, int install, unsigned opts, int *err)
{
    const char *b, *e;
    char *t0 = NULL;
    *err = 0;
    if (tail_find(text, &b, &e)) {
        if (!install && !(t0 = tail_remove(text))) { *err = 2; return NULL; }
    } else if (install && (opts & OPT_AMIKIT) && !setup_has_section(text, 0) && setup_has_section(text, 1)) {
        if (!(t0 = tail_add(text))) { *err = 2; return NULL; }
    }
    if (!t0) return setup_config_sections(text, install, opts, err);
    char *r = setup_config_sections(t0, install, opts, err);
    if (*err) { free(t0); return NULL; }
    if (r) { free(t0); return r; }
    return t0;                                    /* the tail alone changed it */
}

static char *setup_config_sections(const char *text, int install, unsigned opts, int *err)
{
    char *a = NULL;
    int found = 0;
    *err = 0;
    if (!setup_has_section(text, 0) && !setup_has_section(text, 1)) {
        /* no board section at all: a card set up by hand (AmigaOS 3.x) */
        a = setup_config_kernel_any(text, install ? board_kernel[cfg_board] : "", install, err);
        if (*err) return NULL;
        char *b = setup_config_block(a ? a : text, opts, install, err);
        if (*err) { free(a); return NULL; }
        if (b) { free(a); return b; }
        return a;
    }
    for (int i = 0; i < 2; i++) {
        /* install: the running board's section first; uninstall: in order */
        int bd = !install ? i : i == 0 ? cfg_board : !cfg_board;
        int e;                            /* 0 done, 1 no such section, 2 out of memory */
        char *k = setup_config_kernel(a ? a : text, board_sect[bd], install ? board_kernel[bd] : "", install, &e);
        if (e == 2) { free(a); *err = 2; return NULL; }
        if (e == 1 && install && i == 0) { free(a); *err = 1; return NULL; }
        if (!e) found++;
        if (k) { free(a); a = k; }
    }
    if (!found) { free(a); *err = 1; return NULL; }   /* neither section: not a config I understand */
    char *b = setup_config_block(a ? a : text, opts, install, err);
    if (*err) { free(a); return NULL; }
    if (b) { free(a); return b; }
    return a;
}

/* ---- files ----------------------------------------------------------------- */

char *setup_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (b && fread(b, 1, n, f) == (size_t)n) b[n] = 0;
    else { free(b); b = NULL; }
    fclose(f);
    return b;
}

int setup_write(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* <path>.pre-aga, the first time only: it is always the untouched original */
int setup_backup(const char *path, const char *text)
{
    char bak[256];
    snprintf(bak, sizeof bak, "%s.pre-aga", path);
    FILE *f = fopen(bak, "rb");
    if (f) { fclose(f); return 1; }
    return setup_write(bak, text);
}

/* ---- the low speed option: the command line (from RGB2RTG 924ba03) ---------- */

/* 1 when my block notes that the cmdline's LOWSPEED_WORD is mine, the
   thieving little word */
int setup_block_slow(const char *text)
{
    int in_blk = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = line_end(p) - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk && line_is(q, qn, BLK_SLOW)) return 1;
    }
    return 0;
}

/* 1 when config.txt has a dtoverlay=emmc line of its own with low_speed: the
   Pi 4's SD card at 25 MHz already */
int setup_card_slow(const char *text)
{
    return has_active(text, "dtoverlay=emmc", "low_speed");
}

/* the cmdline file config.txt names (cmdline=boot/cmdline.txt on
   CaffeineOS): its first active cmdline= line outside my block, else the
   firmware's cmdline.txt - relative to config.txt's partition */
void setup_cmdline_file(const char *text, char *name, int len)
{
    int in_blk = 0;
    snprintf(name, len, "cmdline.txt");
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (line_is(q, qn, BLK_BEGIN)) { in_blk = 1; continue; }
        if (line_is(q, qn, BLK_END))   { in_blk = 0; continue; }
        if (in_blk || !line_is(q, qn, "cmdline=")) continue;
        q += 8;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        const char *v = q;
        while (v < e && *v != ' ' && *v != '\t' && *v != '#' && *v != '\r' && *v != '\n') v++;
        if (v > q) snprintf(name, len, "%.*s", (int)(v - q), q);
        return;
    }
}

/* the command line: the file's first line that is neither empty nor a
   comment; *end where its words end, before trailing blanks and the line
   ending. NULL when there is none. */
static const char *cl_line(const char *text, const char **end)
{
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (q == e || *q == '#' || *q == '\r' || *q == '\n') continue;
        while (e > q && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
        *end = e;
        return p;
    }
    return NULL;
}

/* where `tok` is a whole word of the command line, or NULL */
static const char *cl_find(const char *text, const char *tok)
{
    const char *e = NULL, *p = cl_line(text, &e);
    size_t n = strlen(tok);
    for (const char *r = p; r && r + n <= e; r++)
        if (!strncmp(r, tok, n) && (r == p || r[-1] == ' ' || r[-1] == '\t') &&
            (r + n == e || r[n] == ' ' || r[n] == '\t'))
            return r;
    return NULL;
}

int setup_cmdline_has(const char *text, const char *tok)
{
    return cl_find(text, tok) != NULL;
}

/* `tok` added as the command line's last word, or taken out with the blank
   before it (after it when it is the first word). A file without a command
   line gets it as a line of its own. Taking it out gives back exactly what
   adding it put in. NULL when nothing changes, or out of memory. */
char *setup_cmdline_edit(const char *text, const char *tok, int add)
{
    size_t n = strlen(text), tn = strlen(tok);
    const char *r = cl_find(text, tok), *e = NULL;
    if (add ? r != NULL : r == NULL) return NULL;
    char *out = malloc(n + tn + 4);
    if (!out) return NULL;
    if (add) {
        const char *p = cl_line(text, &e);
        size_t at = p ? (size_t)(e - text) : n;
        /* no command line: a line of its own, after a last one without an ending */
        const char *sep = p ? " " : n && text[n - 1] != '\n' ? (strstr(text, "\r\n") ? "\r\n" : "\n") : "";
        memcpy(out, text, at);
        sprintf(out + at, "%s%s%s", sep, tok, text + at);
        return out;
    }
    const char *p = cl_line(text, &e), *a = r, *b = r + tn;
    if (a > p && (a[-1] == ' ' || a[-1] == '\t')) a--;
    else if (b < e && (*b == ' ' || *b == '\t')) b++;
    memcpy(out, text, a - text);
    strcpy(out + (a - text), b);
    return out;
}

#ifdef HOST_TEST
/* gcc -DHOST_TEST -o setuptest setup.c
   ./setuptest DIR tests/config-*.txt tests/whdload-*.prefs

   DIR holds copies of WHDLoad-Startup, WHDLoad-Cleanup and User-Startup; each
   is installed, re-installed (must not change), and uninstalled (must give
   back the original). What the installs wrote goes to DIR as well
   (<name>.installed), so tests/ stays as it is.

   Each config.txt in tests/ is installed on both boards with every answer to
   the two questions, the whole bloody lot, installed again (must not
   change), and uninstalled (must give back the original byte for byte - or,
   for a card that carries a mark of mine already, what uninstalling it
   gives). Next to a fixture, hand-written results it must give, where there
   are any:
     <name>.expected-install     installed on a classic PiStorm, no Framethrower,
                                 no NTSC Agnus
     <name>.expected-uninstall   uninstalled (the file itself when there is
                                 nothing to take out)
   A fixture with "noend" in its name has a block start without its end line:
   install and uninstall must both leave it alone (err 3).

   ./setuptest --install [--ps16] [--opts=N] IN OUT | --uninstall IN OUT: one
   edit, for package/pc/test-sdsetup.ps1 (N: setup_config_block's opts - 1
   Framethrower, 2 NTSC Agnus, 4 Sound on HDMI, 8 HDMI 1080p50, 16 HDMI
   1080p60). OUT gets the result (IN
   itself when nothing changes); the exit code is setup_config's err. */

/* The firmware's choice of kernel, simulated for a Pi 4 B: the last kernel=
   line whose filters match. [all] resets; a model filter ([pi4], [cm4] ...)
   replaces the model filter, a [gpioN=V] filter the gpio one; other
   bracketed filters are taken as matching. Pins 4, 17 and 24 as given,
   any other pin reads high. */
static const char *boot_kernel(const char *text, int gpio4, int gpio17, int gpio24)
{
    static char k[200];
    int model_ok = 1, gpio_ok = 1;
    k[0] = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        if (*q == '#') continue;
        if (*q == '[') {
            if (line_is(q, qn, "[all]")) model_ok = gpio_ok = 1;
            else if (!strncmp(q, "[gpio", 5)) {
                int pin = atoi(q + 5), val = 1;
                const char *eq = memchr(q, '=', qn);
                if (eq) val = atoi(eq + 1);
                int lvl = pin == 4 ? gpio4 : pin == 17 ? gpio17 : pin == 24 ? gpio24 : 1;
                gpio_ok = lvl == val;
            } else if (line_is(q, qn, "[pi4]")) model_ok = 1;
            else if (!strncmp(q, "[pi", 3) || line_is(q, qn, "[cm4]")) model_ok = 0;
            continue;
        }
        if (model_ok && gpio_ok && line_is(q, qn, "kernel=")) {
            const char *w = e;
            while (w > q && (w[-1] == '\n' || w[-1] == '\r' || w[-1] == ' ' || w[-1] == '\t')) w--;
            snprintf(k, sizeof k, "%.*s", (int)(w - q), q);
        }
    }
    return k;
}

/* equal once trailing whitespace is ignored: a script that ended without a
   newline gets one before my block, and keeps it after uninstall */
static int same_trimmed(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    while (na && (a[na - 1] == '\n' || a[na - 1] == ' ' || a[na - 1] == '\r')) na--;
    while (nb && (b[nb - 1] == '\n' || b[nb - 1] == ' ' || b[nb - 1] == '\r')) nb--;
    return na == nb && !strncmp(a, b, na);
}

/* exact: config.txt must come back byte for byte; a script may differ in its
   final newline */
static int check(const char *name, const char *orig, const char *once, const char *twice, const char *back, int exact)
{
    int same = back && !strcmp(back, orig);
    int ok = once && !twice && back && (same || (!exact && same_trimmed(back, orig)));
    printf("%-18s install %s, reinstall %s, uninstall %s\n", name,
           once ? "changed" : "NO CHANGE", twice ? "CHANGED AGAIN" : "idempotent",
           !back ? "NOTHING" : same ? "identical" : same_trimmed(back, orig) ?
           (exact ? "DIFFERS IN THE FINAL NEWLINE" : "identical but for the final newline") : "DIFFERS");
    return ok;
}

/* 1 when a line of S:WHDLoad.prefs `text` runs my script for key k */
static int whd_on(const char *text, int k)
{
    for (const char *p = text; *p; p = line_end(p)) {
        const char *e = line_end(p), *q = p;
        size_t body = e - p;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        while (q < p + body && (*q == ' ' || *q == '\t')) q++;
        if (whd_sets(q, body - (q - p), k) == 1) return 1;
    }
    return 0;
}

/* where the harness writes: DIR/<the fixture's own name><suffix> */
static void out_path(char *p, size_t n, const char *dir, const char *path, const char *suffix)
{
    const char *b = strrchr(path, '/'), *b2 = strrchr(path, '\\');
    if (b2 && (!b || b2 > b)) b = b2;
    snprintf(p, n, "%s/%s%s", dir, b ? b + 1 : path, suffix);
}

/* On a plain card, outside [gpio..] sections: my kernel= lines, any other
   active kernel= line, and #AGA-PISTORM-WAS: kernel= lines */
static void plain_kernels(const char *text, int *ours, int *others, int *was)
{
    int gpio = 0;
    *ours = *others = *was = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        while (qn && (q[qn - 1] == '\n' || q[qn - 1] == '\r')) qn--;
        if (qn && *q == '[') { gpio = qn >= 5 && !strncmp(q, "[gpio", 5); continue; }
        if (gpio) continue;
        if (is_kernel_line(q, qn)) { if (is_our_kernel(p)) (*ours)++; else (*others)++; }
        else if (is_was_kernel(q, qn)) (*was)++;
    }
}

/* lines that are `s` after their leading blanks, without their line ending */
static int count_lines(const char *text, const char *s)
{
    int n = 0;
    for (const char *p = text; *p; p = line_end(p)) {
        const char *q = p, *e = line_end(p);
        while (*q == ' ' || *q == '\t') q++;
        size_t qn = e - q;
        while (qn && (q[qn - 1] == '\n' || q[qn - 1] == '\r')) qn--;
        n += qn == strlen(s) && !strncmp(q, s, qn);
    }
    return n;
}

/* compare with a hand-written result next to the fixture; nothing to do
   when there is none */
static int expect(const char *dir, const char *path, const char *what, const char *got)
{
    char p[600];
    snprintf(p, sizeof p, "%s.expected-%s", path, what);
    char *want = setup_read(p);
    if (!want) return 1;
    int ok = !strcmp(want, got);
    printf("%-18s %s: %s\n", "", what, ok ? "as expected (hand-written)" : "NOT AS EXPECTED");
    if (!ok) {
        char a[600];
        snprintf(a, sizeof a, ".actual-%s", what);
        out_path(p, sizeof p, dir, path, a);
        setup_write(p, got);
        printf("  what it gave: %s\n", p);
    }
    free(want);
    return ok;
}

int main(int argc, char **argv)
{
    char p[600];
    int all = 1;
    if (argc >= 4 && (!strcmp(argv[1], "--install") || !strcmp(argv[1], "--uninstall"))) {
        int inst = !strcmp(argv[1], "--install"), ps16 = 0, a = 2;
        unsigned opts = 0;
        for (; a < argc - 2; a++) {
            if (!strcmp(argv[a], "--ps16")) ps16 = 1;
            else if (!strncmp(argv[a], "--opts=", 7)) opts = (unsigned)strtoul(argv[a] + 7, NULL, 0);
        }
        char *t = setup_read(argv[a]);
        if (!t) return 20;
        int err;
        setup_board(ps16);
        char *r = setup_config(t, inst, opts, &err);
        if (err) return err;
        return setup_write(argv[a + 1], r ? r : t) ? 0 : 20;
    }
    if (argc < 2) { printf("usage: setuptest DIR files... | --install [--ps16] [--opts=N] IN OUT | --uninstall IN OUT\n"); return 20; }
    const char *dir = argv[1];
    const char *files[3] = { "WHDLoad-Startup", "WHDLoad-Cleanup", "User-Startup" };
    const char *blocks[3] = { setup_whd_on, setup_whd_off, setup_user_startup };
    int where[3] = { 1, 1, 0 };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        char *t = setup_read(p);
        if (!t) { printf("%s: missing\n", p); all = 0; continue; }
        char *a = setup_script_set(t, blocks[i], where[i]);
        char *b = a ? setup_script_set(a, blocks[i], where[i]) : NULL;
        char *c = a ? setup_script_remove(a) : NULL;
        all &= check(files[i], t, a, b, c, 0);
        if (a) { snprintf(p, sizeof p, "%s/%s.installed", dir, files[i]); setup_write(p, a); }
        if (a && strstr(a, "\r")) { printf("  CR IN SCRIPT\n"); all = 0; }
        if (where[i] == 1) {
            /* an install from before 1.107 had the block at the end: installing
               again moves it to the top, once, and uninstall still gives back
               the script without it */
            char *old = setup_script_set(t, blocks[i], 0);
            char *moved = old ? setup_script_set(old, blocks[i], 1) : NULL;
            char *again = moved ? setup_script_set(moved, blocks[i], 1) : NULL;
            char *gone = moved ? setup_script_remove(moved) : NULL;
            int ok = old && moved && a && (!strcmp(moved, a) || same_trimmed(moved, a)) && !again && gone
                     && (!strcmp(gone, t) || same_trimmed(gone, t));
            printf("%-18s old install at the end: %s\n", files[i],
                   ok ? "moved to the top, idempotent, uninstall identical" : "NOT MOVED CORRECTLY");
            all &= ok;
            free(old); free(moved); free(again); free(gone);
        }
        free(a); free(b); free(c); free(t);
    }
    static const char *const kern[2] = { "kernel=kernel/Emu68-classic-aga.gz", "kernel=kernel/Emu68-pistorm16-aga.gz" };
    for (int k = 2; k < argc; k++) {
        char *t = setup_read(argv[k]);
        if (!t) { printf("%s: missing\n", argv[k]); all = 0; continue; }
        const char *base = argv[k];
        for (const char *s = argv[k]; *s; s++) if (*s == '/' || *s == '\\') base = s + 1;
        int err;
        printf("--- %s\n", base);
        if (strstr(base, ".prefs")) {
            /* S:WHDLoad.prefs: both scripts switched on, idempotent, uninstall
               gives the file back; another command is left alone - and only
               a fixture with "other" in its name has one (CaffeineOS's
               "Execute S:WHDLoad-Startup" is mine, found by RGB2RTG) */
            int other, o2, o3;
            char *a = setup_whdprefs(t, 1, &other);
            char *b = a ? setup_whdprefs(a, 1, &o2) : NULL;
            char *c = a ? setup_whdprefs(a, 0, &o3) : NULL;
            const char *now = a ? a : t;
            int on = (other & 1 || whd_on(now, 0)) && (other & 2 || whd_on(now, 1));
            if (!a) printf("%-18s install changes nothing (%s)\n", base, other ? "another command is set" : "already on");
            else all &= check(base, t, a, b, c, 0);
            if (!on) { printf("  NOT BOTH SWITCHED ON\n"); all = 0; }
            if (other) printf("  another command left alone: %s%s\n", other & 1 ? "ExecuteStartup " : "", other & 2 ? "ExecuteCleanup" : "");
            if (!other != !strstr(base, "other")) { printf("  'another command' WRONGLY %s\n", other ? "reported" : "missed"); all = 0; }
            if (a) { out_path(p, sizeof p, dir, argv[k], ".installed"); setup_write(p, a); }
            free(a); free(b); free(c); free(t);
            continue;
        }
        if (strstr(base, "noend")) {
            /* a block start without its end line: nothing is written, on
               either board, install or uninstall */
            int ok = 1;
            for (int ps16 = 0; ps16 < 2; ps16++) {
                setup_board(ps16);
                for (unsigned opts = 0; opts < 64; opts++) {
                    char *a = setup_config(t, 1, opts, &err);
                    if (a || err != 3) ok = 0;
                    free(a);
                }
                char *u = setup_config(t, 0, 0, &err);
                if (u || err != 3) ok = 0;
                free(u);
                for (int on = 0; on < 2; on++) {          /* the Sound switch too */
                    char *h = setup_config_hdmi(t, on, &err);
                    if (h || err != 3) ok = 0;
                    free(h);
                }
                static const int rates[4] = { 0, 50, 60, RATE_ROOM };
                for (int ri = 0; ri < 4; ri++) {          /* and HDMI rate's */
                    char *h = setup_config_rate(t, rates[ri], &err);
                    if (h || err != 3) ok = 0;
                    free(h);
                }
            }
            setup_board(0);
            printf("%-18s block without its end line: %s\n", base, ok ? "left alone, install and uninstall (err 3)" : "WRITTEN");
            all &= ok;
            free(t);
            continue;
        }
        if (block_open(t)) { printf("%s: an open block in a fixture without \"noend\" in its name\n", base); all = 0; }
        int plain = !setup_has_section(t, 0) && !setup_has_section(t, 1);
        /* What uninstall must give back: the card as it was before any
           install. For a card already carrying a mark of mine - installed,
           or given the overlay block by SD-Setup, which uses the same block
           so that Uninstall removes it too - that is uninstall(t), which
           must itself carry no mark; the hand-written .expected-uninstall
           says what it is (comparing uninstall with itself proves nothing,
           the pointless exercise) */
        setup_board(0);
        char *orig = strstr(t, CFG_MARK) || strstr(t, CFG_WAS) || strstr(t, BLK_BEGIN) ? setup_config(t, 0, 0, &err) : NULL;
        const char *ref = orig ? orig : t;
        if (orig && (strstr(orig, CFG_MARK) || strstr(orig, CFG_WAS) || strstr(orig, BLK_BEGIN))) { printf("%s: uninstall left a mark\n", base); all = 0; }
        all &= expect(dir, argv[k], "uninstall", ref);
        /* AmiKit's own card: the installer's AmiKit choice (OPT_AMIKIT) */
        unsigned amik = strstr(base, "amikit") ? OPT_AMIKIT : 0;
        if (!amik && !(!setup_has_section(t, 0) && setup_has_section(t, 1))) {
            /* the AmiKit choice changes nothing for any other card: one with a
               [gpio17=0] section, or with neither board's section */
            int same = 1;
            for (int ps16 = 0; ps16 < 2; ps16++) {
                setup_board(ps16);
                for (unsigned opts = 0; opts < 64; opts++) {
                    int e1, e2;
                    char *x = setup_config(t, 1, opts, &e1), *y = setup_config(t, 1, opts | OPT_AMIKIT, &e2);
                    if (e1 != e2 || !x != !y || (x && strcmp(x, y))) same = 0;
                    free(x); free(y);
                }
            }
            setup_board(0);
            if (!same) { printf("%s: THE AMIKIT CHOICE CHANGES THIS CARD\n", base); all = 0; }
        }
        {
            char *a = setup_config(t, 1, amik, &err);
            all &= expect(dir, argv[k], "install", a ? a : t);
            free(a);
        }
        /* run on each board, with every combination of the two questions */
        for (int ps16 = 0; ps16 < 2; ps16++) {
            setup_board(ps16);
            for (unsigned opts = 0; opts < 64; opts++) {    /* 8|16: 50.2 Hz */
                char *a = setup_config(t, 1, opts | amik, &err);
                if (err) { printf("%s: no %s (err %d)\n", base, setup_section(), err); all = 0; break; }
                char *b = a ? setup_config(a, 1, opts | amik, &err) : NULL;
                char *c = a ? setup_config(a, 0, 0, &err) : NULL;
                char name[600];
                snprintf(name, sizeof name, "%s [%s ft=%u pal=%u sound=%u hz=%s softlimit=%s]", base, ps16 ? "ps16" : "classic",
                         opts & 1, (opts >> 1) & 1, (opts >> 2) & 1, (opts & 24) == 24 ? "50.2" : (opts & 8) ? "50" : (opts & 16) ? "60" : "-",
                         (opts & OPT_NO_SOFT_LIMIT) ? "no" : "yes");
                /* temp_soft_limit=70 in my block on a classic card: once,
                   unless the installer was told no or the card has a line of
                   its own */
                if (!ps16) {
                    const char *now = a ? a : t;
                    int want = !(opts & OPT_NO_SOFT_LIMIT) && !has_active(ref, "temp_soft_limit=", NULL);
                    int got = 0, in_b = 0;
                    for (const char *q = now; *q; q = line_end(q)) {
                        const char *s = q, *e = line_end(q);
                        while (*s == ' ' || *s == '\t') s++;
                        if (line_is(s, e - s, BLK_BEGIN)) { in_b = 1; continue; }
                        if (line_is(s, e - s, BLK_END)) { in_b = 0; continue; }
                        if (in_b && !strncmp(s, "temp_soft_limit=70", 18)) got++;
                    }
                    if (got != want) { printf("  %s: %d temp_soft_limit=70 lines in the block, want %d\n", name, got, want); all = 0; }
                }
                /* Sound on HDMI on a classic card: hdmi_drive=2 once in my
                   block, before its first [filter] so every Pi reads it - but
                   none when the card's own already does that. Switching Sound
                   on after an install without it gives the same file, and
                   switching it off again gives that install back. */
                if (!ps16 && (opts & 4)) {
                    const char *now = a ? a : t;
                    int own = has_active_every(ref, HDMI_DRIVE), n = 0, before = 1, in_b = 0, seen_f = 0, lead = 0;
                    for (const char *q = now; *q; q = line_end(q)) {
                        const char *s = q, *e = line_end(q);
                        while (*s == ' ' || *s == '\t') s++;
                        if (line_is(s, e - s, BLK_BEGIN)) { in_b = lead = 1; continue; }
                        if (line_is(s, e - s, BLK_END)) { in_b = 0; continue; }
                        if (!in_b) continue;
                        int la = is_lead_all(lead, s, e - s);  /* a block at the end starts with [all] */
                        lead = 0;
                        if (*s == '[' && !la) seen_f = 1;
                        if (is_hdmi_line(s, e)) { n++; if (seen_f) before = 0; }
                    }
                    if (own ? n != 0 : n != 1 || !before) {
                        printf("  %s: %d hdmi_drive=2 lines in the block%s\n", name, n, before ? "" : ", after a filter");
                        all = 0;
                    }
                    char *a0 = setup_config(t, 1, (opts & ~4u) | amik, &err);
                    const char *was = a0 ? a0 : t;
                    char *h = setup_config_hdmi(was, 1, &err);
                    int e1 = err;
                    char *h2 = h ? setup_config_hdmi(h, 1, &err) : NULL;
                    char *off = setup_config_hdmi(now, 0, &err);
                    int okh = !e1 && !strcmp(h ? h : was, now) && !h2 && !strcmp(off ? off : now, was);
                    if (!okh) { printf("  %s: SOUND ON/OFF AFTER THE INSTALL IS NOT THE SAME AS INSTALLING WITH IT\n", name); all = 0; }
                    free(a0); free(h); free(h2); free(off);
                }
                /* HDMI rate on a classic card: switching it after an install
                   without it gives what installing with it gives, again
                   changes nothing, and switching it off gives that install
                   back */
                if (!ps16 && (opts & 24)) {
                    int rate = (opts & 24) == 24 ? RATE_ROOM : (opts & 8) ? 50 : 60;
                    const char *now = a ? a : t;
                    char *a0 = setup_config(t, 1, (opts & ~24u) | amik, &err);
                    const char *was = a0 ? a0 : t;
                    char *r1 = setup_config_rate(was, rate, &err);
                    int e1 = err;
                    char *r2 = r1 ? setup_config_rate(r1, rate, &err) : NULL;
                    char *off = setup_config_rate(now, 0, &err);
                    int okr = !e1 && r1 && !strcmp(r1, now) && !r2 && !strcmp(off ? off : now, was)
                              && setup_config_rate_state(now) == rate && setup_config_rate_state(was) == 0;
                    if (!okr) { printf("  %s: HDMI RATE AFTER THE INSTALL IS NOT THE SAME AS INSTALLING WITH IT\n", name); all = 0; }
                    free(a0); free(r1); free(r2); free(off);
                }
                if (!a && strstr(t, CFG_MARK)) {
                    /* a card this installer already set up, with every line it
                       would add: installing again must change nothing at all */
                    printf("%-18s already installed: install changes nothing (correct)\n", name);
                    continue;
                }
                all &= check(name, ref, a, b, c, 1);
                /* whichever board it ran on, every board section the card has
                   now boots that board's AGA kernel */
                for (int j = 0; j < 2; j++)
                    if (a && setup_has_section(t, j) && !strstr(a, kern[j])) { printf("  %s NOT SET\n", kern[j]); all = 0; }
                /* and never keeps one of my own kernel= lines as an old one:
                   uninstall would turn the AGA kernel back on */
                for (int j = 0; j < 2 && a && has_our_mark(t); j++) {
                    char w[128];
                    snprintf(w, sizeof w, "%s%s", CFG_WAS, kern[j]);
                    if (strstr(a, w)) { printf("  OUR KERNEL KEPT AS %s\n", w); all = 0; }
                }
                /* a plain card: mine under every kernel= line it had (one per
                   Pi model, say) - none of its own left active, as many of
                   mine as old lines kept as #AGA-PISTORM-WAS:, and one at the
                   top when it had none */
                if (a && plain) {
                    int ours, others, was;
                    plain_kernels(a, &ours, &others, &was);
                    if (!strstr(a, kern[ps16]) || others || ours != (was ? was : 1)) {
                        printf("  PLAIN CARD: %d of our kernel lines, %d WAS lines, %d others active\n", ours, was, others);
                        all = 0;
                    }
                }
                /* and on a classic PiStorm no gpu_mem below 128 is left where a
                   Pi 4 reads it */
                if (a && !ps16) {
                    int kept, low;
                    char *g = gpu_mem_low_out(a, &kept, &low);
                    if (low) { printf("  A gpu_mem BELOW 128 STILL COUNTS\n"); all = 0; }
                    free(g);
                }
                /* a PiStorm16 gets its kernel= line and nothing else - but the
                   emmc line on a card with none: no gpu_mem touched, no clock,
                   sdhc, pal or unicam line, whatever the two questions said */
                if (a && ps16) {
                    if (strstr(a, CFG_WAS GPU_KEY)) { printf("  gpu_mem CHANGED ON A PISTORM16\n"); all = 0; }
                    const char *bb = strstr(a, BLK_BEGIN), *be = bb ? strstr(bb, BLK_END) : NULL;
                    for (const char *q = bb ? strchr(bb, '\n') + 1 : NULL; q && be && q < be; q = strchr(q, '\n') ? strchr(q, '\n') + 1 : NULL)
                        if (strncmp(q, "[pi4]", 5) && strncmp(q, "dtoverlay=emmc,unit0=rw", 23) && strncmp(q, "[all]", 5)) {
                            printf("  MORE THAN THE emmc LINE IN THE PISTORM16 BLOCK\n"); all = 0; break;
                        }
                    if (bb && has_active(ref, "dtoverlay=emmc", "unit0=rw")) { printf("  A BLOCK ON A PISTORM16 CARD THAT HAS emmc\n"); all = 0; }
                    char *a0 = opts ? setup_config(t, 1, amik, &err) : NULL;
                    if (opts && (!a0 || strcmp(a0, a))) { printf("  THE QUESTIONS CHANGED A PISTORM16 INSTALL\n"); all = 0; }
                    free(a0);
                }
                if (a && opts == 3) { out_path(p, sizeof p, dir, argv[k], ps16 ? ".ps16.installed" : ".classic.installed"); setup_write(p, a); }
                if (a && (strstr(a, "\r\n") != NULL) != (strstr(t, "\r\n") != NULL)) { printf("  LINE ENDINGS CHANGED\n"); all = 0; }
                free(a); free(b); free(c);
            }
        }
        /* the card moves: installed on a classic PiStorm, the installer is run
           again on a PiStorm16. The result must be exactly what installing on
           the PiStorm16 alone gives - the classic run's clock lines, which a
           PiStorm16 does not get, go again - and ONE uninstall, on either
           board, must take everything out. */
        setup_board(0);
        char *x = setup_config(t, 1, 3 | amik, &err);
        setup_board(1);
        char *y = setup_config(x ? x : t, 1, 3 | amik, &err);
        char *direct = setup_config(t, 1, 3 | amik, &err);  /* PiStorm16 from the start */
        const char *both = y ? y : x ? x : t;
        char *z = setup_config(both, 0, 0, &err);
        /* a plain card is one board's: the second run switches it to the
           PiStorm16 kernel instead, and one uninstall still undoes it all */
        int ok = plain ? y && strstr(y, kern[1]) && !strstr(y, kern[0])
                       : strstr(both, kern[0]) && strstr(both, kern[1]) && direct && !strcmp(both, direct);
        free(direct);
        ok = ok && z && !strstr(z, CFG_MARK) && !strcmp(z, ref);
        printf("%-18s moved to the other board: %s\n", base,
               ok ? (plain ? "switched to its kernel, one uninstall undoes it all" : "same as a PiStorm16 install, one uninstall undoes it all")
                  : !plain ? "NOT WHAT A PISTORM16 INSTALL GIVES" : "FAILED");
        all &= ok;
        setup_board(0);
        /* hand edits on the installed card. (a) the block's end line deleted:
           nothing may be written, install or uninstall */
        if (x && strstr(x, BLK_END)) {
            char *cut = malloc(strlen(x) + 1);
            const char *e = strstr(x, BLK_END);
            memcpy(cut, x, e - x);
            strcpy(cut + (e - x), line_end(e));
            char *r1 = setup_config(cut, 0, 0, &err);
            int e1 = err;
            char *r2 = setup_config(cut, 1, 3, &err);
            int okc = !r1 && !r2 && e1 == 3 && err == 3;
            printf("%-18s block end line deleted by hand: %s\n", base, okc ? "left alone (err 3)" : "WRITTEN");
            all &= okc;
            free(r1); free(r2); free(cut);
        }
        /* (b) the old kernel put back by hand: each of my kernel= lines
           replaced with another, the #AGA-PISTORM-WAS: lines deleted, my
           comments left. Uninstall must keep every such line and the line
           after each comment; installing again takes them over as it would
           any kernel= line */
        if (x) {
            const char *hand = "kernel=kernel/Emu68-hand";
            const char *eol = strstr(x, "\r\n") ? "\r\n" : "\n";
            char *h = malloc(strlen(x) * 2 + 64), *o = h;
            int n = 0;
            for (const char *q = x; *q; q = line_end(q)) {
                const char *e = line_end(q);
                if (is_our_kernel(q)) { o += sprintf(o, "%s%s", hand, e > q && e[-1] == '\n' ? eol : ""); n++; continue; }
                if (!strncmp(q, CFG_WAS "kernel=", strlen(CFG_WAS) + 7)) continue;
                memcpy(o, q, e - q); o += e - q;
            }
            *o = 0;
            char *u = setup_config(h, 0, 0, &err);
            char *i2 = setup_config(h, 1, 3, &err);
            char *u2 = i2 ? setup_config(i2, 0, 0, &err) : NULL;
            const char *uu = u ? u : h;
            /* a hand line inside my tail section (AmiKit's card) goes
               with the tail: the section is mine, and kept on its own the
               line would land in whatever section came before it. Every
               other one stays. */
            int in_tail = 0;
            {
                const char *tb, *te;
                if (tail_find(h, &tb, &te))
                    for (const char *q = tb; q < te; q = line_end(q))
                        if (!strncmp(q, hand, strlen(hand))) in_tail++;
            }
            int okh = count_lines(uu, hand) == n - in_tail && !strstr(uu, CFG_MARK) && !strstr(uu, TAIL_BEGIN)
                      && i2 && u2 && !strcmp(u2, uu);
            printf("%-18s old kernel put back by hand (%d): %s\n", base, n,
                   okh ? "uninstall keeps it, install takes it over" : "LOST OR CHANGED");
            all &= okh;
            free(h); free(u); free(i2); free(u2);
        }
        /* the Sound switch on a card AGA-PISTORM is not installed on: nothing
           there is mine to change (err 1) */
        {
            char *h = setup_config_hdmi(ref, 1, &err);
            int okn = !h && err == 1;
            printf("%-18s Sound switched on before any install: %s\n", base, okn ? "left alone (err 1)" : "WRITTEN");
            all &= okn;
            free(h);
        }
        /* the PiStorm32-lite's last block (RGB2RTG ca01950), with the
           firmware's choice simulated (boot_kernel): where the block went in,
           a PiStorm32-lite in stealth mode (GPIO4 low) that reads GPIO17 low
           boots its own [gpio4=0] kernel, and a classic PiStorm (GPIO4 and
           GPIO24 high, GPIO17 low) the kernel it boots without the block */
        {
            int e2;
            char *a = setup_config(t, 1, amik, &e2);
            const char *at = a ? a : t;
            const char *gb, *ge;
            if (!e2 && g4_find(at, &gb, &ge)) {
                char k4[200];
                char *plain = g4_remove(at);
                int need = plain && g4_needed(plain, k4, sizeof k4);
                char ps32[200], cl_with[200], cl_without[200];
                snprintf(ps32, sizeof ps32, "%s", boot_kernel(at, 0, 0, 0));
                snprintf(cl_with, sizeof cl_with, "%s", boot_kernel(at, 1, 0, 1));
                snprintf(cl_without, sizeof cl_without, "%s", plain ? boot_kernel(plain, 1, 0, 1) : "");
                int ok4 = need && !strcmp(ps32, k4) && !strcmp(cl_with, cl_without);
                printf("%-18s PiStorm32-lite last block: %s (PS32-lite with GPIO17 low: %s, classic: %s)\n", base,
                       ok4 ? "in" : "WRONG", ps32, cl_with);
                all &= ok4;
                free(plain);
            } else if (!e2 && strstr(at, "[gpio4=0]")) {
                char k4[200];
                printf("%-18s PiStorm32-lite last block: not needed%s\n", base,
                       g4_needed(at, k4, sizeof k4) ? " - BUT IT IS" : "");
                if (g4_needed(at, k4, sizeof k4)) all = 0;
            }
            free(a);
        }
        /* the low speed option: an install with it notes the cmdline word in
           my block, again changes nothing, one without it takes the note out
           (as a plain install), Uninstall leaves none (as a plain uninstall) */
        {
            char *p0 = setup_config(t, 1, amik, &err);
            char *u0 = setup_config(p0 ? p0 : t, 0, 0, &err);
            char *s1 = setup_config(t, 1, OPT_SLOW | amik, &err);
            char *s2 = s1 ? setup_config(s1, 1, OPT_SLOW | amik, &err) : NULL;
            char *s3 = s1 ? setup_config(s1, 1, amik, &err) : NULL;
            char *s4 = s1 ? setup_config(s1, 0, 0, &err) : NULL;
            const char *plain = p0 ? p0 : t, *gone = u0 ? u0 : (p0 ? p0 : t);
            int oks = s1 && setup_block_slow(s1) && !s2 && s3 && !setup_block_slow(s3) && !strcmp(s3, plain)
                      && s4 && !setup_block_slow(s4) && !strcmp(s4, gone);
            printf("%-18s low speed note: %s\n", base, oks ? "in with the option, out without it and on uninstall" : "WRONG");
            all &= oks;
            free(p0); free(u0); free(s1); free(s2); free(s3); free(s4);
        }
        free(x); free(y); free(z); free(orig); free(t);
    }
    /* the low speed option's command line word (RGB2RTG's cases): added as
       the last word, taken out again exactly; a word that only looks like it
       is not it */
    {
        static const struct { const char *in, *added; int has, back; } ct[] = {
            { "root=x quiet\n", "root=x quiet emmc.low_speed\n", 0, 1 },
            { "a b  \r\n", "a b emmc.low_speed  \r\n", 0, 1 },
            { "", "emmc.low_speed", 0, 1 },
            { "# c\n\nsd.unit0=rw emmc.unit0=rw\n", "# c\n\nsd.unit0=rw emmc.unit0=rw emmc.low_speed\n", 0, 1 },
            { "# c\n", "# c\nemmc.low_speed", 0, 1 },
            { "# c", "# c\nemmc.low_speed", 0, 0 },
            { "xemmc.low_speed\n", "xemmc.low_speed emmc.low_speed\n", 0, 1 },
            { "a emmc.low_speed_x\n", "a emmc.low_speed_x emmc.low_speed\n", 0, 1 },
            { "emmc.low_speed sd.unit0=rw\n", NULL, 1, 0 },
            { "# emmc.low_speed\nsd.unit0=rw\n", "# emmc.low_speed\nsd.unit0=rw emmc.low_speed\n", 0, 1 },
        };
        for (unsigned i = 0; i < sizeof ct / sizeof ct[0]; i++) {
            char *a = setup_cmdline_edit(ct[i].in, LOWSPEED_WORD, 1);
            char *b = a ? setup_cmdline_edit(a, LOWSPEED_WORD, 0) : NULL;
            char *c = a ? setup_cmdline_edit(a, LOWSPEED_WORD, 1) : NULL;
            int ok = setup_cmdline_has(ct[i].in, LOWSPEED_WORD) == ct[i].has &&
                     (ct[i].added ? a && !strcmp(a, ct[i].added) && setup_cmdline_has(a, LOWSPEED_WORD) &&
                                    !c && b && (!ct[i].back || !strcmp(b, ct[i].in))
                                  : !a);
            printf("cmdline %u: %s%s\n", i, ct[i].has ? "has it" : a ? "added, taken out" : "?", ok ? "" : " - WRONG");
            if (!ok) all = 0;
            free(a); free(b); free(c);
        }
        char *d = setup_cmdline_edit("emmc.low_speed sd.unit0=rw\n", LOWSPEED_WORD, 0);
        int ok = d && !strcmp(d, "sd.unit0=rw\n");
        printf("cmdline, the first word taken out: %s\n", ok ? "ok" : "WRONG");
        if (!ok) all = 0;
        free(d);
        static const struct { const char *cfg, *name; } cf[] = {
            { "[all]\r\ncmdline=boot/cmdline.txt\r\n", "boot/cmdline.txt" },
            { "arm_64bit=1\n", "cmdline.txt" },
            { "#cmdline=x.txt\n  cmdline=y.txt # mine\n", "y.txt" },
        };
        for (unsigned i = 0; i < sizeof cf / sizeof cf[0]; i++) {
            char name[64];
            setup_cmdline_file(cf[i].cfg, name, sizeof name);
            ok = !strcmp(name, cf[i].name);
            printf("cmdline file %u: %s%s\n", i, name, ok ? "" : " - WRONG");
            if (!ok) all = 0;
        }
        ok = setup_card_slow("[pi4]\ndtoverlay=emmc,unit0=rw,low_speed\n") && !setup_card_slow("[pi4]\ndtoverlay=emmc,unit0=rw\n")
             && !setup_card_slow("#dtoverlay=emmc,low_speed\n");
        printf("the card's own low_speed overlay: %s\n", ok ? "seen" : "WRONG");
        if (!ok) all = 0;
    }
    /* Emu68 Imager's config.txt (RGB2RTG 0c5af86): each overlay setting on a
       dtparam= line of its own, which belongs to the overlay loaded last - up
       to the next dtoverlay= line or [filter] */
    {
        static const struct { const char *text; int sdrw, slow; } it[] = {
            { "dtoverlay=sdhc\n\ndtparam=unit0=rw\n", 1, 0 },
            { "dtoverlay=sdhc\ndtoverlay=emmc\ndtparam=unit0=rw\n", 0, 0 },
            { "dtoverlay=sdhc\n[pi4]\ndtparam=unit0=rw\n", 0, 0 },
            { "dtparam=unit0=rw\ndtoverlay=sdhc\n", 0, 0 },
            { "dtoverlay=emmc\r\n\r\ndtparam=unit0=rw\r\ndtparam=low_speed\r\n", 0, 1 },
            { "dtoverlay=emmc\ndtoverlay=unicam\ndtparam=low_speed\n", 0, 0 },
            { "#dtoverlay=emmc\ndtparam=low_speed\n", 0, 0 },
            { "dtoverlay=emmc\n# a comment\n\ndtparam=low_speed # 25 MHz\n", 0, 1 },
            { BLK_BEGIN "\ndtoverlay=emmc\n" BLK_END "\ndtparam=low_speed\n", 0, 0 },   /* mine is not the card's */
        };
        for (unsigned i = 0; i < sizeof it / sizeof it[0]; i++) {
            int sdrw = has_active(it[i].text, "dtoverlay=sdhc", "unit0=rw"), slow = setup_card_slow(it[i].text);
            int ok = sdrw == it[i].sdrw && slow == it[i].slow;
            printf("dtparam lines %u: sdhc rw %d, own low speed %d%s\n", i, sdrw, slow, ok ? "" : " - WRONG");
            if (!ok) all = 0;
        }
        static const struct { const char *text; int pal; } ov[] = {
            { "dtoverlay=PAL\n", 1 }, { "dtoverlay=pal,x=1\r\n", 1 }, { "dtoverlay=palx\n", 0 },
            { "#dtoverlay=pal\n", 0 }, { "dtoverlay=pal", 1 },
        };
        for (unsigned i = 0; i < sizeof ov / sizeof ov[0]; i++) {
            int ok = has_overlay(ov[i].text, "pal") == ov[i].pal;
            printf("pal overlay %u: %d%s\n", i, has_overlay(ov[i].text, "pal"), ok ? "" : " - WRONG");
            if (!ok) all = 0;
        }
        /* a small Imager card, 50 Hz, NTSC Agnus, Sound: the block below its
           screen mode, headed [all], so my hdmi_mode is the last one; its
           PAL and its sdhc/emmc write access kept, none of mine added; Sound
           and HDMI rate switched afterwards stay in the block, under its
           [all]; uninstall gives the file back */
        const char *tc = "# Config.txt for PiStorm\n# Generated by Emu68 Imager v2.0\n[all]\narm_64bit=1\n[pi4]\narm_boost=1\n[all]\n"
                         "dtoverlay=PAL\ndtoverlay=sdhc\n\ndtparam=unit0=rw\ndtoverlay=emmc\n\ndtparam=unit0=rw\n"
                         "hdmi_group=1\nhdmi_mode=16\n[all]\n## PiStorm Boot\n[gpio17=0]\nkernel=Emu68-pistorm-classic.gz\n"
                         "initramfs /kick.rom\n";
        setup_board(0);
        int err;
        char *r = setup_config(tc, 1, 2 | 4 | 8, &err);
        char *again = r ? setup_config(r, 1, 2 | 4 | 8, &err) : NULL;
        char *u = r ? setup_config(r, 0, 0, &err) : NULL;
        char *noh = r ? setup_config_hdmi(r, 0, &err) : NULL;          /* Sound off ... */
        char *hon = noh ? setup_config_hdmi(noh, 1, &err) : NULL;      /* ... and on again */
        char *r60 = r ? setup_config_rate(r, 60, &err) : NULL;
        const char *last = NULL, *blk = r ? strstr(r, BLK_BEGIN) : NULL;
        for (const char *s = r; s && (s = strstr(s, "\nhdmi_mode=")); s++) last = s;
        int ok = r && !err && !again && u && !strcmp(u, tc) && blk && blk > strstr(r, "[gpio17=0]")
                 && !strncmp(line_end(blk), "[all]\n", 6) && last && !strncmp(last, "\nhdmi_mode=31", 13)
                 && !strstr(r, "dtoverlay=sdhc,unit0=rw") && !strstr(r, "dtoverlay=emmc,unit0=rw")
                 && !strstr(r, "\ndtoverlay=pal\n") && noh && !strstr(noh, HDMI_DRIVE) && hon && !strcmp(hon, r)
                 && r60 && strstr(r60, "[all]\n" GPU_KEY "128\n" HDMI_DRIVE "\nhdmi_group=2\nhdmi_mode=82\n");
        printf("Imager card: %s\n", ok ? "block at the end under [all], its overlays kept, Sound and rate in place, uninstall identical"
                                       : "WRONG");
        if (!ok) { all = 0; if (r) printf("%s", r); }
        free(r); free(again); free(u); free(noh); free(hon); free(r60);
    }
    printf(all ? "ALL OK\n" : "FAILURES\n");
    return all ? 0 : 1;
}
#endif
