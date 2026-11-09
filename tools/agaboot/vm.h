/* agaboot - the Chipset menu's voice mixer (vm.c): Paula's four voices, each
 * where you want it and as loud as you want it
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AGABOOT_VM_H
#define AGABOOT_VM_H

#include <exec/types.h>
#include <intuition/screens.h>

struct vm_set {
    BYTE on;                    /* the mixer: off is Paula's own wiring, 0 and 3 left, 1 and 2 right */
    BYTE pan[4];                /* -8 left .. 8 right */
    BYTE lev[4];                /* 0 silent, 1-16: -30 .. 0 dB in 2 dB steps */
    BYTE preset;                /* vm_preset_name; 0 Custom */
    /* not kept: the Sound monitor's clicks on its voice scopes */
    BYTE mute[4];
    BYTE solo;                  /* the voice heard alone, -1 none */
};

void vm_load(struct vm_set *v);                 /* ENV:AGA_VOICES; off when not set */
void vm_store(const struct vm_set *v, int save);
void vm_send(const struct vm_set *v);           /* to the kernel, mutes and solo included */
const char *vm_preset_name(int i);              /* NULL past the last */
int  vm_parse_preset(const char *w);            /* "Spread", "softstereo": index, -1 */
void vm_preset(struct vm_set *v, int i);        /* pans and levels, on */
void vm_describe(const struct vm_set *v, char *o, int len);
const char *vm_pan_text(int p, char *o);        /* "Left", "L 3", "Centre", "R 8" */
const char *vm_lev_text(int l, char *o);        /* "off", "-30 dB" .. "0 dB" */

/* the window on its own: 0 Cancel (sent back), 1 Use, 2 Save - stored already */
int  vm_window(struct vm_set *v);
/* beside the Sound monitor, served by its loop */
struct vmw;
struct vmw *vmw_open(struct Screen *scr, struct vm_set *v);
ULONG vmw_sigs(struct vmw *w);
int  vmw_handle(struct vmw *w);                 /* 0 open; 1 Cancel, 2 Use, 3 Save: done and stored */
void vmw_close(struct vmw *w);
void vmw_front(struct vmw *w);
void vmw_sync(struct vmw *w);                   /* the settings changed outside it: show them */

#endif
