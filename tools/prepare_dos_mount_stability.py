#!/usr/bin/env python3
"""Produce a reversible, private bare-metal DOS mounting startup patch.
Does not modify the source AmigaOS volume.
"""
from pathlib import Path
import argparse, hashlib, difflib

def read_iso(path):
    return path.read_bytes().decode("latin-1").replace("\r\n","\n").replace("\r","\n")

def only_once(s,a,b):
    if s.count(a)!=1: raise ValueError("Expected exact unique section: "+a[:100])
    return s.replace(a,b)

def rewrite_startup(s):
    # Custom AGS automatic classic-SCSI recovery is incompatible with standalone.
    a=s.index("; Loads a patched scsi.device with large partition support if WHDLoad: drive")
    b=s.index("; Loads PeterK's icon library",a)
    s=s[:a]+"""; EMU68AGA Bare Metal: AmigaDOS/RDB owns the boot volumes.
; Never launch the old scsi.device hardware picker or reboot loop if a
; volume is temporarily absent. See S:User-Startup for AGS launch guard.

"""+s[b:]
    a=s.index(" ; Installs the pistorm shared drive dos driver")
    b=s.index("IF EXISTS DEVS:Monitors",a)
    s=s[:a]+"""; EMU68AGA Bare Metal: existing RDB partitions are auto-mounted by
; the OS/ROM storage driver, and must not be re-mounted here.
; Never auto-probe CF0 (PCMCIA), MS0 (physical floppy), legacy PCO, or
; the optional 'emu68' FAT shared drive on an Amiga-less Raspberry Pi.
; To use the FAT transfer volume manually, run:
;   C:Mount DEVS:DOSDrivers/emu68
; from a Shell after confirming brcm-sdhc.device is available.
;
; Keep only handler mounts that do not poll missing block hardware.
If EXISTS L:Queue-Handler
 C:Mount >NIL: DEVS:DOSDrivers/PIPE
EndIf
If EXISTS L:AHI-Handler
 C:Mount >NIL: DEVS:DOSDrivers/AUDIO
EndIf

"""+s[b:]
    s=only_once(s, "c:mount workbench:system/ami-share",
      "; EMU68AGA: external ami-share requires a configured device; mount manually only.\n;c:mount workbench:system/ami-share")
    return s

def rewrite_user(s):
    a=""";BEGIN Amiga Game Selector
 Ex S:AGS-Stuff
;END Amiga Game Selector"""
    b=""";BEGIN Amiga Game Selector
; Only auto-launch when AGS resides on an available RDB volume.
; If missing, finish loading Workbench rather than hardware-recovery loops.
If EXISTS WHDLoad:AGS2
 Ex S:AGS-Stuff
Else
 Echo "[EMU68AGA] WHDLoad:AGS2 unavailable; skipping AGS auto-start."
EndIf
;END Amiga Game Selector"""
    return only_once(s,a,b)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("source",type=Path,help="read-only Workbench volume root")
    ap.add_argument("destination",type=Path,help="new output directory")
    args=ap.parse_args()
    if args.destination.exists(): ap.error("destination already exists; will not overwrite")
    src=args.source/"S"
    expected={"Startup-Sequence":rewrite_startup,"User-Startup":rewrite_user}
    # Strictly read and validate all sources before creating anything.
    orig={name:read_iso(src/name) for name in expected}
    patched={name:fn(orig[name]) for name,fn in expected.items()}
    for name in expected:
        assert patched[name]!=orig[name]
    assert "C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)" not in patched["Startup-Sequence"]
    assert "scsi_hw_select" not in patched["Startup-Sequence"]
    assert "C:Mount >NIL: DEVS:DOSDrivers/PIPE" in patched["Startup-Sequence"]
    assert "C:Mount >NIL: DEVS:DOSDrivers/AUDIO" in patched["Startup-Sequence"]
    assert "If EXISTS WHDLoad:AGS2" in patched["User-Startup"]
    assert "Ex S:AGS-Stuff" in patched["User-Startup"]
    assert patched["Startup-Sequence"].count("LoadWB")==orig["Startup-Sequence"].count("LoadWB")
    assert patched["Startup-Sequence"].count("EndCLI")==orig["Startup-Sequence"].count("EndCLI")
    out=args.destination
    (out/"S").mkdir(parents=True)
    (out/"originals").mkdir()
    changes=[]
    for name in expected:
        (out/"originals"/name).write_bytes((src/name).read_bytes())
        (out/"S"/name).write_bytes(patched[name].encode("latin-1"))
        changes.append("".join(difflib.unified_diff(orig[name].splitlines(keepends=True),patched[name].splitlines(keepends=True),
          fromfile="original/S/"+name,tofile="patched/S/"+name)))
    (out/"DIFF.txt").write_text("\n".join(changes))
    (out/"SHA256SUMS.txt").write_text("\n".join(hashlib.sha256(p.read_bytes()).hexdigest()+"  "+str(p.relative_to(out))
           for p in sorted(out.rglob("*")) if p.is_file())+"\n")
    print("PATCHED",out)
    for name in expected:
        print("FILE",name,"old",len(orig[name]),"new",len(patched[name]))
    print("LIVE_AMIGA_OS_UNCHANGED")

if __name__=="__main__": main()
