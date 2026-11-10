# FRF EMU68AGA Bare Metal v1.121 RC12 — DF1 drive detection

Developer test candidate; physical Pi 3 verification still required.
Builds on RC11; retains its disk-change timing, INDEX, dual-drive
rotation, PAL soft reset, RTG handoff, and independent FRF USB stack.

## Source defect and change

The virtual CIA-B floppy selection handler skipped DF1 while empty.
Kickstart probes external floppy hardware before DF1 has an ADF.
Therefore it could miss the drive during boot and not discover it
later, even after an ADF was inserted.

RC12 advertises both virtual DF0 and DF1 as connected DD drives
from boot, even when empty. Empty still means /CHNG asserted, with
no READY while the motor is running. When the motor is off, /RDY
reports the DD identification bit to Kickstart.

This is conditional on FRF_VIRTUAL_AGA; physical PiStormAGA drive
selection logic remains unchanged.

## Testing

- Verified build for Pi 3 with FRF_RTG_HANDOFF=ON.
- Host test: empty DF1 selectable at boot, drive-ID signal,
  media-change line, later DF1 insertion and READY.
- Existing DF0/DF1 disk swap, INDEX, motor and dirty-media tests pass.
- Physical Kickstart enumeration and Lionheart disk-2 response are
  not yet verified.

IMPORTANT: A game that requests disk 2 in DF0 must have the second
ADF swapped into DF0. Mounting disk 2 into DF1 will not force a
game that only reads DF0 to use the second drive.

No game ADFs or Kickstart ROMs included. WHDLoad .ss Save/Load
is still not implemented. Preserve the working kernel backup.
