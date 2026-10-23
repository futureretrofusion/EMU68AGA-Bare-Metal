/* agaboot - the Chipset menu's Sound monitor (monitor.c)
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_MONITOR_H
#define AGABOOT_MONITOR_H

/* Chipset menu - Sound - Monitor..., and agaboot SOUND MONITOR: the window
   until it is closed. 0, or 5 with msg saying why it could not open. A
   second call while one is open brings that one to the front. */
int monitor_window(char *msg, int len);

#endif
