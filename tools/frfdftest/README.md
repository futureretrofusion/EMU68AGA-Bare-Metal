# FRFDFTEST - AmigaOS trackdisk diagnostic

This is a read-only Amiga 68k executable. Copy FRFDFTEST to C:
on Workbench. Run from AmigaShell:

    FRFDFTEST
    FRFDFTEST 1

The first command queries all four drive units; the second queries DF1.

The disk.resource cached ID is a diagnostic, not proof of current
hardware state. Expected DD ID = $00000000; absent = $FFFFFFFF.
OpenDevice(trackdisk.device,1) = 0 means the OS recognises unit 1.
TD_GETDRIVETYPE and TD_GETNUMTRACKS then reveal its physical
drive emulation; TD_CHANGESTATE actual = 0 means media present,
nonzero means empty. TD_CHANGENUM tracks disk-change notifications.

After mounting a known Workbench-formatted ADF in DF1 and verifying
the above, optionally use:

    FRFDFTEST 1 READ

This performs an actual CMD_READ of sector 0 into CHIP memory and
prints the first eight bytes. DO NOT run READ until OpenDevice and
TD_CHANGESTATE have succeeded; a DMA bug could hang a read.

The Workbench disk icon is separate from virtual-drive identification.
A custom-format game disk may have no AmigaDOS filesystem and hence
may not show a volume icon even when its trackdisk unit functions.
No claim of DF1 hardware success is made until a real Pi3 test passes.
