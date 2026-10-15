#!/usr/bin/env python3
"""Rebuild the 480x270 RGB565/RLE Bare Metal boot splash header.
Usage: python3 tools/build_bare_metal_logo.py
Requires Pillow and numpy.
"""
from pathlib import Path
from PIL import Image
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'assets/EMU68AGA_Bare_Metal_Splash.png'
DST=ROOT/'machine/Emu68/include/logo/logo_aga_rgb.h'
im=Image.open(SRC).convert('RGB')
small=im.resize((480,270),Image.Resampling.LANCZOS)
p=small.quantize(colors=192,method=Image.Quantize.MEDIANCUT,dither=Image.Dither.NONE)
idx=np.asarray(p,dtype=np.uint8).ravel()
pal=p.getpalette()[:192*3]
rgb565=[]
for i in range(192):
    rv,gv,bv=pal[3*i:3*i+3]
    rgb565.append(((rv>>3)<<11)|((gv>>2)<<5)|(bv>>3))
out=bytearray()
i=0
while i<len(idx):
    v=int(idx[i]);j=i+1
    while j<len(idx) and idx[j]==v and j-i<255:j+=1
    out.extend([j-i,v]);i=j
lines=['/* EMU68AGA BARE METAL: 480x270, 192 colours, 565 palette, RLE; generated from assets/EMU68AGA_Bare_Metal_Splash.png. */',
'#define LOGO_AGA_RGB_WIDTH 480','#define LOGO_AGA_RGB_HEIGHT 270','#define LOGO_AGA_RGB_COLORS 192',
'static const unsigned short logo_aga_rgb_pal[192]={']
for i in range(0,192,12):lines.append(' '+','.join('0x%x'%n for n in rgb565[i:i+12])+',')
lines.extend(['};','static const unsigned char logo_aga_rgb[]={'])
for i in range(0,len(out),28):lines.append(' '+','.join(str(n) for n in out[i:i+28])+',')
lines.extend(['};',f'#define LOGO_AGA_RGB_LEN {len(out)}'])
assert sum(out[::2])==480*270
DST.write_text('\n'.join(lines)+'\n')
print(f'Wrote {DST} ({len(out)} RLE bytes)')
