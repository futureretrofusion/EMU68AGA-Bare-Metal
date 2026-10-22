#!/usr/bin/env python3
"""Read real USB HID descriptors and capture changed bytes of genuine pads.

The working USPi driver reads the device's report descriptor rather than
guessing that all 27-byte packets share the same layout. This utility gathers
the missing ground truth directly from the actual dongle on a Linux host.
Run after physically plugging the dongle into Studio Main.
"""
import argparse
import datetime as dt
import os
import pathlib
import selectors
import sys
import time

DEVICES = {(0x220d,0x330c),(0x7e05,0x0920),(0x2563,0x0576),(0x2563,0x0575)}
def identity(node):
    path=node / "device"
    try:
        lines=(path/"uevent").read_text(errors="replace").splitlines()
        fields=dict(line.split("=",1) for line in lines if "=" in line)
        _,vendor,product=fields["HID_ID"].split(":")
        vid,pid=int(vendor,16),int(product,16)
        if (vid,pid) not in DEVICES:return None
        desc=(path/"report_descriptor").read_bytes()
        return (vid,pid,fields.get("HID_NAME",""),desc)
    except (OSError,ValueError,KeyError):return None
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--watch",action="store_true",help="Capture actual input reports and changed bytes")
    ap.add_argument("--seconds",type=int,default=90)
    ap.add_argument("--output",default=str(pathlib.Path.home()/"Downloads"))
    a=ap.parse_args()
    if a.seconds<1 or a.seconds>600:ap.error("--seconds must be 1..600")
    suffix=dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    folder=pathlib.Path(a.output)/("EMU68AGA_REAL_HID_"+suffix)
    folder.mkdir(parents=True,exist_ok=True)
    handles=[]
    for node in sorted(pathlib.Path("/sys/class/hidraw").glob("hidraw*")):
        found=identity(node)
        if not found:continue
        vid,pid,name,descriptor=found
        label=f"{node.name}_{vid:04x}_{pid:04x}"
        (folder/(label+"_report_descriptor.hex")).write_text(
            " ".join(f"{b:02X}" for b in descriptor)+"\n")
        print(f"{node.name}: {vid:04x}:{pid:04x} {name!r} descriptor_bytes={len(descriptor)}",flush=True)
        if a.watch:
            try:
                fd=os.open("/dev/"+node.name,os.O_RDONLY|os.O_NONBLOCK)
                handles.append((fd,label))
            except OSError as e:print(f"  Cannot read /dev/{node.name}: {e}",flush=True)
    if not handles:
        if a.watch:print("No accessible target hidraw devices; descriptors are saved, raw reports need permissions.",flush=True)
        elif not list(folder.glob("*.hex")):print("None of the target USB dongles is currently attached to Studio Main.",flush=True)
        print("Output:",folder,flush=True)
        return 0 if list(folder.glob("*.hex")) else 1
    selector=selectors.DefaultSelector()
    for fd,label in handles:selector.register(fd,selectors.EVENT_READ,label)
    prior={}
    with (folder/"hid_input_reports.txt").open("w") as log:
        print("Move D-pad alone, then press each fire button alone, then both. Press Ctrl-C to finish.",flush=True)
        until=time.monotonic()+a.seconds
        try:
            while time.monotonic()<until:
                for key,_ in selector.select(timeout=0.3):
                    try:data=os.read(key.fd,128)
                    except BlockingIOError:continue
                    if not data:continue
                    ident=(key.data,data[0],len(data))
                    old=prior.get(ident)
                    if old==data:continue
                    changes=[] if old is None else [
                        f"{i}:{old[i]:02X}>{data[i]:02X}"
                        for i in range(min(len(old),len(data))) if old[i]!=data[i]]
                    line=(f"{time.time():.6f} {key.data} ID={data[0]:02X} "
                          f"len={len(data)} raw={data.hex(' ').upper()} "
                          f"changed={','.join(changes) or 'BASE'}")
                    log.write(line+"\n");log.flush()
                    print(line,flush=True)
                    prior[ident]=data
        except KeyboardInterrupt:pass
    for fd,_ in handles:os.close(fd)
    print("Captured evidence:",folder,flush=True)
    return 0
if __name__=="__main__":sys.exit(main())
