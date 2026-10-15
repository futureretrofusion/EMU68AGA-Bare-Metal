/* EMU68AGA Bare Metal: read-only DOSList snapshot.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Does not open devices, issue mount requests or write any DOS structures.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <stdio.h>

#define MAX_ENTRIES 256u
struct Entry {
    char name[34];
    LONG type;
    UBYTE task;
};
static struct Entry entries[MAX_ENTRIES];

static const char *kind(LONG type) {
    switch (type) {
        case DLT_DEVICE: return "DEVICE";
        case DLT_DIRECTORY: return "ASSIGN";
        case DLT_VOLUME: return "VOLUME";
        case DLT_LATE: return "LATE";
        case DLT_NONBINDING: return "NONBINDING";
        default: return "OTHER";
    }
}
static void copy_name(const struct DosList *d, char *name) {
    ULONG n = 0, i;
    const UBYTE *b = (const UBYTE *)BADDR(d->dol_Name);
    if (b) n = b[0];
    if (n > 32) n = 32;
    for (i = 0; i < n; ++i) name[i] = (char)b[i + 1];
    name[n] = 0;
}
int main(void) {
    const ULONG flags = LDF_READ | LDF_ALL;
    struct DosList *list = 0, *entry = 0;
    ULONG i, n = 0;

    /* Bounded retry; don't wait indefinitely for a held DOSList lock. */
    for (i = 0; i < 10 && !list; ++i) {
        list = AttemptLockDosList(flags);
        if (!list) Delay(5);
    }
    if (!list) {
        puts("[DOS] DOSList locked/busy after bounded retries");
        return 2;
    }
    /* Snapshot while locked, release BEFORE any printf() or DOS I/O. */
    while (n < MAX_ENTRIES &&
           (entry = NextDosEntry(entry ? entry : list, LDF_ALL))) {
        copy_name(entry, entries[n].name);
        entries[n].type = entry->dol_Type;
        entries[n].task = entry->dol_Task != 0;
        ++n;
    }
    UnLockDosList(flags);

    puts("[DOS] Read-only DOSList; no devices opened or mounted");
    for (i = 0; i < n; ++i)
        if (entries[i].name[0])
            printf("[DOS] %-10s %-32s task=%s\n",
                kind(entries[i].type), entries[i].name,
                entries[i].task ? "yes" : "no");
    printf("[DOS] Entries: %u%s\n", n,
           n == MAX_ENTRIES ? " (limit reached)" : "");
    return 0;
}
