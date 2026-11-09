/* agaboot - the Chipset menu's items as one table, which every menu that
 * carries the buggers is written from: S:ToolsDaemon.menu, Directory Opus 5's
 * user menu (dopusmenu.c) and Workbench's AppMenu (wbmenu.c).
 *
 * An item followed by sub-items (sub = 1) heads a sub-menu: Output > RTG
 * Image..., RGB port...; Expert > System wide..., Run program... (1.115 -
 * one click less than a chooser window, asked for by a player). Its own
 * command opens that chooser, and is what a menu that cannot hang a
 * sub-menu there runs instead: Workbench 3.5/3.9, where Chipset itself is
 * already a sub-menu of Tools, and the SYS:Chipset drawer.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_MENU_H
#define AGABOOT_MENU_H

struct menu_entry {
    const char *label, *cmd;
    int stack;                  /* bytes, for the shell the sod runs in */
    int sub;                    /* 1: sub-item of the header above it, sod */
};

/* 1 when entry i heads the sub-items after it, and no mistake */
static inline int menu_is_header(const struct menu_entry *m, int n, int i)
{
    return !m[i].sub && i + 1 < n && m[i + 1].sub;
}

#endif
