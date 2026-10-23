/* wbmenu.c - the Chipset menu in Workbench's own menu bar, for a plain
 * AmigaOS 3.x.
 *
 * CaffeineOS gets its Chipset title from ToolsDaemon and the DOpus5 user
 * menu, files that agaboot MENU rewrites at boot. A plain 3.x has neither,
 * so there the same items go into Workbench's menu strip through
 * workbench.library's AppMenu items:
 *   V45 and up (3.1.4, 3.2)  a "Chipset" title of its own (WBAPPMENUA_GetTitleKey),
 *                            Output and Expert sub-menus in it (WBAPPMENUA_GetKey
 *                            on the item, its sub-items WBAPPMENUA_UseKey)
 *   V44 (3.5, 3.9)           a "Chipset" sub-menu in Tools (WBAPPMENUA_GetKey)
 *   older                    the items straight in Tools, named "Chipset: ..."
 * Each step is checked rather than assumed from the version: a Workbench that
 * ignores the tag leaves the key at 0, and the next one down is tried. That is
 * the only way I trust it. An item that gets no key of its own stays a plain
 * item and opens its chooser (menu.h) - as it always does under 3.5/3.9's
 * Tools, which has no room for a third level.
 *
 * AppMenu items last only as long as the program that added them, so one
 * small agaboot process stays in the background to own them. Picking an item
 * runs its command in a shell of its own, the way ToolsDaemon does, so the
 * requester it opens never holds up the menu.
 *
 *   agaboot WBMENU        start that process once (agaboot MENU does at boot)
 *   agaboot WBMENU OFF    stop it, and the menu goes with it (UNINSTALL MENU too)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/wb.h>
#include <stdio.h>
#include "menu.h"

struct Library *WorkbenchBase;

/* public, so there is only ever one, and so OFF can find the bastard */
#define WBMENU_PORT "AGA-PISTORM Chipset menu"
#define WBMENU_CMD  "C:agaboot WBMENU SERVE"
#define MAX_ITEMS   16
#define CMD_STACK   16384                      /* what the Chipset drawer's scripts get */

/* both handles NIL:, closed by the shell when the command ends */
static int run_async(const char *cmd)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (in && out) {
        struct TagItem t[] = { { SYS_Input, (ULONG)in }, { SYS_Output, (ULONG)out },
                               { SYS_Asynch, TRUE }, { SYS_UserShell, TRUE }, { TAG_DONE, 0 } };
        if (SystemTagList((STRPTR)cmd, t) != -1) return 1;
    }
    if (in) Close(in);
    if (out) Close(out);
    return 0;
}

int wbmenu_start(void)
{
    Forbid();
    struct MsgPort *p = FindPort((STRPTR)WBMENU_PORT);
    Permit();
    return p ? 1 : run_async(WBMENU_CMD);
}

int wbmenu_stop(void)
{
    Forbid();
    struct MsgPort *p = FindPort((STRPTR)WBMENU_PORT);
    if (p) Signal((struct Task *)p->mp_SigTask, SIGBREAKF_CTRL_C);
    Permit();
    return p != NULL;
}

/* Puts the menu up; 0 while Workbench is not open yet, because
   AddAppMenuItemA fails until LoadWB has opened its screen. A title or
   sub-menu shows only once its first item hangs from it. ami[] gets the title
   (if any) first, then each item followed by its sub-items, in the order
   they must be taken down in reverse. An item's ID is its index in m. */
static int add_menu(struct MsgPort *port, const char *title, const struct menu_entry *m, int n,
                    struct AppMenuItem **ami, int *count)
{
    static char plain[MAX_ITEMS][40];
    static const Tag parent_tag[2] = { WBAPPMENUA_GetTitleKey, WBAPPMENUA_GetKey };
    struct TagItem t[3] = { { TAG_DONE, 0 }, { TAG_DONE, 0 }, { TAG_DONE, 0 } };
    ULONG key = 0;
    int k = 0, own_title = 0;
    for (int i = WorkbenchBase->lib_Version >= 45 ? 0 : 1; i < 2 && WorkbenchBase->lib_Version >= 44; i++) {
        key = 0;                         /* a Workbench that ignores the tag leaves it at 0 */
        t[0].ti_Tag = parent_tag[i];
        t[0].ti_Data = (ULONG)&key;
        ami[0] = AddAppMenuItemA(~0UL, 0, (STRPTR)title, port, t);
        if (!ami[0]) return 0;
        if (key) { k = 1; own_title = i == 0; break; }
        RemoveAppMenuItem(ami[0]);       /* it went into Tools as a plain item */
    }
    for (int i = 0; i < n && k < MAX_ITEMS; i++) {
        if (m[i].sub) continue;          /* with its header, below */
        const char *label = m[i].label;
        ULONG sub_key = 0;
        int head = own_title && menu_is_header(m, n, i);
        t[1].ti_Tag = TAG_DONE;
        if (key) {
            t[0].ti_Tag = WBAPPMENUA_UseKey;
            t[0].ti_Data = key;
            if (head) {                  /* a sub-menu of its own, if Workbench gives it a key */
                t[1].ti_Tag = WBAPPMENUA_GetKey;
                t[1].ti_Data = (ULONG)&sub_key;
            }
        } else {                         /* loose in Tools: say whose they are */
            snprintf(plain[i], sizeof plain[i], "%s: %s", title, label);
            label = plain[i];
            t[0].ti_Tag = TAG_DONE;
        }
        ami[k] = AddAppMenuItemA((ULONG)i, 0, (STRPTR)label, port, t);
        if (!ami[k]) { if (k == 0) return 0; continue; }
        k++;
        if (!sub_key) continue;          /* no key: a plain item that opens its chooser */
        t[0].ti_Tag = WBAPPMENUA_UseKey;
        t[0].ti_Data = sub_key;
        t[1].ti_Tag = TAG_DONE;
        for (int j = i + 1; j < n && m[j].sub && k < MAX_ITEMS; j++) {
            ami[k] = AddAppMenuItemA((ULONG)j, 0, (STRPTR)m[j].label, port, t);
            if (ami[k]) k++;
        }
    }
    *count = k;
    return 1;
}

int wbmenu_serve(const char *title, const struct menu_entry *m, int n)
{
    struct AppMenuItem *ami[MAX_ITEMS + 1];
    int count = 0, quit = 0;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    struct MsgPort *port = CreateMsgPort();
    if (!port) return 20;
    port->mp_Node.ln_Name = (char *)WBMENU_PORT;
    port->mp_Node.ln_Pri = 0;
    Forbid();
    int running = FindPort((STRPTR)WBMENU_PORT) != NULL;
    if (!running) AddPort(port);
    Permit();
    if (running) { DeleteMsgPort(port); return 0; }
    {   /* a shell started from here copies this default for its command */
        struct CommandLineInterface *cli = Cli();
        if (cli && cli->cli_DefaultStack < CMD_STACK / 4) cli->cli_DefaultStack = CMD_STACK / 4;
    }
    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 37);
    if (!WorkbenchBase) quit = 1;
    /* at boot this starts before LoadWB: try every two seconds until
       Workbench is open, or until WBMENU OFF */
    while (!quit && !add_menu(port, title, m, n, ami, &count)) {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) quit = 1;
        else Delay(100);
    }
    while (!quit) {
        ULONG got = Wait((1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C);
        struct AppMessage *am;
        while ((am = (struct AppMessage *)GetMsg(port))) {
            if (am->am_Type == AMTYPE_APPMENUITEM && am->am_ID < (ULONG)n)
                run_async(m[am->am_ID].cmd);
            ReplyMsg((struct Message *)am);
        }
        if (got & SIGBREAKF_CTRL_C) quit = 1;
    }
    while (count > 0) RemoveAppMenuItem(ami[--count]);   /* the items, then their title */
    Forbid();
    RemPort(port);
    struct Message *left;
    while ((left = GetMsg(port))) ReplyMsg(left);
    Permit();
    DeleteMsgPort(port);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    return WorkbenchBase ? 0 : 20;
}
