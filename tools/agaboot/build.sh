#!/bin/sh
# Build the Amiga-side tools with the m68k cross compiler, in WSL because that
# is where it lives.
#   wsl.exe -d Ubuntu-24.04 -- sh /mnt/c/LLM/Projects/AGA-PISTORM/tools/agaboot/build.sh
set -e
export PATH=/opt/amiga/bin:$PATH
cd "$(dirname "$0")"
m68k-amigaos-gcc -O2 -noixemul -o agaboot.new agaboot.c dopusmenu.c startmenu.c setup.c pointer.c picture.c rgbport.c sound.c monitor.c eq.c vm.c rec.c front.c wbmenu.c ahi16.c
ls -l agaboot.new
# the WHDLoad stand-in for the installer's AmiKit choice (agaboot WHDWRAP) -
# do not lose it
m68k-amigaos-gcc -O2 -noixemul -o whdwrap.new whdwrap.c
ls -l whdwrap.new
# the Sound monitor as a program of its own (SYS:Utilities/AGA-SoundMonitor) -
# it earns its keep
m68k-amigaos-gcc -O2 -noixemul -o smon.new smon.c
ls -l smon.new
