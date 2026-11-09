/* agaboot - one window of a kind (front.c): a second open shoves the first
 * one to the front
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_FRONT_H
#define AGABOOT_FRONT_H

#include <exec/ports.h>
#include <intuition/intuition.h>

struct front {
    struct MsgPort mp;                 /* public, by name: never sent to */
    struct Window *win;                /* the window, once open */
    char name[40];
};

/* The window called name: claimed - or, when another process has it open,
   that one brought to the front and NULL (*other 1). NULL with *other 0:
   no memory, open anyway. */
struct front *front_claim(const char *name, int *other);
void front_set(struct front *f, struct Window *win);
void front_release(struct front *f);

/* a command started on its own, so the window it opens lives beside mine
   ("C:agaboot SOUND EQ ASK"): 1 started */
int front_spawn(const char *cmd);

#endif
