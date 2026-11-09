# FRFADF — standalone EMU68AGA virtual-floppy helper

Compile frfadf.c with the AmigaOS m68k gcc toolchain used by agaboot.
Copy FRFADF to C: in Workbench or CaffeineOS.

Examples from AmigaShell:

    FRFADF Games:ADF/IKPlus.adf SLOT1
    FRFADF Games:ADF/Disk2.adf SLOT2
    FRFADF Games:ADF/System.adf DF0
    FRFADF Games:ADF/Extra.adf DF1
    FRFADF EJECT DF1

RC10 OSD: ADF SLOT selects 1–4, then DF0 INSERT or DF1 INSERT
inserts that slot into the corresponding virtual drive. Eject works
for both DF0 and DF1 but refuses to discard dirty in-memory media.
Boot ADF (when provided) is automatically kept as slot 1.

Only 901120-byte DD and 1802240-byte HD ADFs are accepted.
Mounted slots are write-protected from the OSD. Writes to other
unprotected media are in RAM until flushed separately.
This command uses the FRF private control-register protocol and
must not be run on physical Amigas or unrelated Emu68 kernels.

This AmigaDOS tool populates slots; it does not yet browse files
from the on-screen OSD. Preload alternative disks before launching
WHDLoad games. The .ss save-state backend remains unfinished.
