/* EMU68AGA bare-metal: RTG/AGA HDMI HVS ownership policy.
 * SPDX-License-Identifier: GPL-2.0-only
 * Pure state machine; no HVS writes or application-specific behavior.
 */
#ifndef FRF_RTG_HANDOFF_H
#define FRF_RTG_HANDOFF_H
#include <stdint.h>

typedef struct {
    uint32_t boot_list;
    uint32_t external_list;
    unsigned frames;
    unsigned rtg_owns;
    unsigned saw_native_off;
    unsigned prev_native;
    unsigned takeovers;
    unsigned returns;
} frf_rtg_handoff;

static inline void frf_rtg_handoff_init(frf_rtg_handoff *h, uint32_t boot)
{
    *h = (frf_rtg_handoff){0};
    h->boot_list = boot;
}

/* Call once per completed AGA frame.
 * external_valid is a checked VC4/VC6 HVS display-list distinct from ours.
 * A new external GPU display list is evidence P96 or another output owner
 * has taken HDMI. Do not steal it back merely because AGA is still rendering.
 *
 * Reclaim native output only after a native-blank -> native-active edge,
 * which is the normal handover on a WHDLoad/native screen transition.
 */
static inline int frf_rtg_handoff_step(frf_rtg_handoff *h,
                                        uint32_t live,
                                        uint32_t own_a, uint32_t own_b,
                                        int native, int external_valid,
                                        int had_aga_output)
{
    int external = external_valid &&
                   live != 0 && live != own_a && live != own_b &&
                   live != h->boot_list;
    int rising = native && !h->prev_native;
    if (external && (had_aga_output || h->frames >= 16u)) {
        if (!h->rtg_owns) {
            ++h->takeovers;
            h->rtg_owns = 1;
            h->saw_native_off = 0;
        }
        h->external_list = live;
    }
    if (h->rtg_owns) {
        if (!native) h->saw_native_off = 1;
        if (rising && h->saw_native_off) {
            h->rtg_owns = 0;
            h->saw_native_off = 0;
            ++h->returns;
        }
    }
    h->prev_native = !!native;
    if (h->frames < 0xffffu) ++h->frames;
    return h->rtg_owns;
}
#endif
