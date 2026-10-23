#!/usr/bin/env python3
from pathlib import Path
import re, subprocess, tempfile

root=Path(__file__).resolve().parents[1]
usbh=(root/"third_party/tinyusb/src/host/usbh.c").read_text()
hid=(root/"third_party/tinyusb/src/class/hid/hid_host.c").read_text()

body=usbh[usbh.index("bool tuh_vid_pid_get"):usbh.index("return true;",usbh.index("bool tuh_vid_pid_get"))]
vid_matches=re.findall(r'\*vid\s*=\s*([^;]+);',body)
pid_matches=re.findall(r'\*pid\s*=\s*([^;]+);',body)
vid_expr=next(x.strip() for x in vid_matches if "desc_device" in x)
pid_expr=next(x.strip() for x in pid_matches if "desc_device" in x)

hbody=hid[hid.index("uint16_t hidh_open"):hid.index("// Per HID Specs",hid.index("uint16_t hidh_open"))]
m=re.search(r'p_hid->report_desc_len\s*=\s*(.+?);',hbody,re.S)
assert m
len_expr=" ".join(m.group(1).split())

c=f"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "common/tusb_common.h"
#include "common/tusb_types.h"
#include "class/hid/hid.h"

struct devwrap {{ tusb_desc_device_t desc_device; }};
struct hidwrap {{ uint16_t report_desc_len; }};

int main(void) {{
  uint8_t raw_dev[18]={{18,1,0,2,0,0,0,64,0x7e,0x05,0x09,0x20,0,1,1,2,3,1}};
  struct devwrap w; memset(&w,0,sizeof w); memcpy(&w.desc_device,raw_dev,sizeof w.desc_device);
  struct devwrap *dev=&w;
  uint16_t vid=0,pid=0;
  vid = {vid_expr};
  pid = {pid_expr};
  printf("ACTUAL tuh_vid_pid_get: %04X:%04X\\n",vid,pid);
  if(vid!=0x057e || pid!=0x2009)return 10;

  uint8_t raw_hid[9]={{9,0x21,0x11,0x01,0,1,0x22,0xbf,0x00}};
  const tusb_hid_descriptor_hid_t *desc_hid=(const tusb_hid_descriptor_hid_t*)raw_hid;
  struct hidwrap hw={{0}}; struct hidwrap *p_hid=&hw;
  p_hid->report_desc_len = {len_expr};
  printf("ACTUAL hidh_open report_desc_len: %u/0x%04X\\n",
         p_hid->report_desc_len,p_hid->report_desc_len);
  if(p_hid->report_desc_len!=191u)return 11;
  puts("R40A ACTUAL SOURCE PASS: raw USB LE -> host-order VID/PID + HID report length");
  return 0;
}}
"""

with tempfile.TemporaryDirectory(prefix="r40a_endian_") as td:
    td=Path(td)
    src=td/"actual.c"
    src.write_text(c)
    inc=["-I",str(root/"third_party/tinyusb/src"),"-I",str(root/"src/frf_usb/tinyusb")]
    subprocess.run(["gcc","-O2","-Wall","-Wextra","-Werror",*inc,str(src),"-o",str(td/"x86")],check=True)
    subprocess.run([str(td/"x86")],check=True)
    be="/home/fff2025/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu/bin/aarch64_be-none-linux-gnu-gcc"
    subprocess.run([be,"-static","-O2","-Wall","-Wextra","-Werror",*inc,str(src),"-o",str(td/"be")],check=True)
    subprocess.run(["qemu-aarch64_be",str(td/"be")],check=True)
