#!/usr/bin/env python3
"""mkchipicon.py - the start menu's AGA-PISTORM icons (1.118): a chip as pixel art - a square
package with pins on all four sides, as the AGA chips (Alice, Lisa) are - 32x32 with "AGA" on it
(the AGA-PISTORM heading) and 20x22 without (its entries), written as OS 3.5 colour icons into
chip_icon.h for agaboot MENU (startmenu.c), which puts them in DOpus5:Icons.

    python tools/agaboot/mkchipicon.py [PREVIEW_DIR]

PREVIEW_DIR: also writes both as PNGs, eight times enlarged, to look at on a PC.

The colour icon is RGB2RTG's (tools/rgb2rtg/mkhdmiicon.py, 0.76-dev84): a classic DiskObject with
a 2-plane image, then FORM ICON - FACE and two IMAG (normal; selected, the palette lighter), RLE as
icon.library packs it: ByteRun1 control bytes, the values `depth` bits each in one continuous bit
stream (its decoder checked on CaffeineOS's own Settings.info, 2026-10-20).
SPDX-License-Identifier: GPL-2.0-only
"""
import os, struct, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))

# 0 transparent, 1 the outline, 2-4 the package's shades, 5 the lettering, 6-8 the pins' metal
PAL = {0: None, 1: (10, 10, 12), 2: (30, 31, 36), 3: (48, 50, 58), 4: (84, 88, 98),
       5: (255, 255, 255), 6: (120, 124, 132), 7: (178, 182, 190), 8: (228, 230, 236)}
F57 = {'A': [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
       'G': [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."]}


def canvas(w, h):
    return [[0] * w for _ in range(h)]


def rect(px, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= y < len(px) and 0 <= x < len(px[0]):
                px[y][x] = c


def outline(px):
    h, w = len(px), len(px[0])
    out = [r[:] for r in px]
    for y in range(h):
        for x in range(w):
            if px[y][x] == 0 and any(0 <= y + dy < h and 0 <= x + dx < w and px[y + dy][x + dx] not in (0, 1)
                                     for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1))):
                out[y][x] = 1
    return out


def chip(w, h, b0x, b0y, b1x, b1y, pin_len, text):
    """a package from (b0x,b0y) to (b1x,b1y), pins pin_len long on every side, one every other
    pixel; its pin 1 marked by a dot in the top left corner; text in the middle"""
    px = canvas(w, h)
    for x in range(b0x + 2, b1x - 1, 2):                       # top and bottom rows of pins
        rect(px, x, b0y - pin_len, x, b0y - 1, 7)
        rect(px, x, b1y + 1, x, b1y + pin_len, 7)
        px[b0y - pin_len][x] = 8
        px[b1y + pin_len][x] = 6
    for y in range(b0y + 2, b1y - 1, 2):                       # left and right
        rect(px, b0x - pin_len, y, b0x - 1, y, 7)
        rect(px, b1x + 1, y, b1x + pin_len, y, 7)
        px[y][b0x - pin_len] = 8
        px[y][b1x + pin_len] = 6
    rect(px, b0x, b0y, b1x, b1y, 3)                            # the package, bevelled
    rect(px, b0x, b0y, b1x, b0y, 4)
    rect(px, b0x, b0y, b0x, b1y, 4)
    rect(px, b0x + 1, b1y, b1x, b1y, 2)
    rect(px, b1x, b0y + 1, b1x, b1y, 2)
    px[b0y + 2][b0x + 2] = 2                                   # pin 1's dot
    if b1x - b0x > 14:
        px[b0y + 2][b0x + 3] = 2
        px[b0y + 3][b0x + 2] = 2
        px[b0y + 3][b0x + 3] = 2
    if text:
        tw = sum(len(F57[c][0]) for c in text) + len(text) - 1
        x = b0x + (b1x - b0x + 1 - tw + 1) // 2
        y = b0y + (b1y - b0y + 1 - 7 + 1) // 2
        for ch in text:
            g = F57[ch]
            for r, row in enumerate(g):
                for c, bit in enumerate(row):
                    if bit == '#':
                        px[y + r][x + c] = 5
            x += len(g[0]) + 1
    return outline(px)


NCOL = max(PAL) + 1
DEPTH = (NCOL - 1).bit_length()


class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, v, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc); self.acc = 0; self.n = 0

    def done(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n)); self.acc = 0; self.n = 0
        return bytes(self.out)


def rle_pack(vals, depth):
    b = Bits(); i, n = 0, len(vals)
    while i < n:
        run = 1
        while i + run < n and vals[i + run] == vals[i] and run < 128:
            run += 1
        if run >= 3:
            b.put((257 - run) & 0xff, 8); b.put(vals[i], depth); i += run; continue
        j, lit = i, []
        while j < n and len(lit) < 128:
            r = 1
            while j + r < n and vals[j + r] == vals[j] and r < 3:
                r += 1
            if r >= 3:
                break
            lit.append(vals[j]); j += 1
        b.put(len(lit) - 1, 8)
        for v in lit:
            b.put(v, depth)
        i = j
    return b.done()


def rle_unpack(data, depth, count):
    bits = ''.join('{:08b}'.format(x) for x in data); p, out = 0, []
    while len(out) < count:
        c = int(bits[p:p + 8], 2); p += 8
        if c < 128:
            for _ in range(c + 1):
                out.append(int(bits[p:p + depth], 2)); p += depth
        elif c > 128:
            v = int(bits[p:p + depth], 2); p += depth; out += [v] * (257 - c)
    return out[:count]


def chunk(cid, body):
    return cid + struct.pack('>I', len(body)) + body + (b'\0' if len(body) & 1 else b'')


def make(px):
    H, W = len(px), len(px[0])
    flat = [v for r in px for v in r]
    packed = rle_pack(flat, DEPTH)
    assert rle_unpack(packed, DEPTH, W * H) == flat
    # classic: the Workbench's 4 colours - dark parts black, light ones white, the metal grey
    def classic(c):
        if c == 0: return 0
        lum = sum(PAL[c]) / 3
        return 1 if lum < 90 else 2 if lum > 200 else 3 if c in (6, 7) else 1
    rowbytes = (W + 15) // 16 * 2
    planes = bytearray()
    for plane in range(2):
        for y in range(H):
            row = bytearray(rowbytes)
            for x in range(W):
                if (classic(px[y][x]) >> plane) & 1:
                    row[x >> 3] |= 0x80 >> (x & 7)
            planes += row
    gadget = struct.pack('>IhhhhHHHIIIIIHI', 0, 0, 0, W, H + 1, 0x0004, 0x0003, 0x0001, 1, 0, 0, 0, 0, 0, 1)
    diskobj = struct.pack('>HH', 0xE310, 1) + gadget + struct.pack('>BBIIiiIII', 4, 0, 0, 0, -0x80000000, -0x80000000, 0, 0, 0)
    image = struct.pack('>hhhhhIBBI', 0, 0, W, H, 2, 1, 3, 0, 0)
    assert len(diskobj) == 78 and len(image) == 20
    pal = b''.join(bytes(PAL[c] if PAL[c] else (0, 0, 0)) for c in range(NCOL))
    lite = b''.join(bytes(min(255, v + 36) for v in (PAL[c] if PAL[c] else (0, 0, 0))) for c in range(NCOL))
    face = bytes([W - 1, H - 1, 1, 0x11]) + struct.pack('>H', len(pal) - 1)      # frameless, square pixels
    def imag(p):
        return bytes([0, NCOL - 1, 3, 1, 0, DEPTH]) + struct.pack('>HH', len(packed) - 1, len(p) - 1) + packed + p
    body = b'ICON' + chunk(b'FACE', face) + chunk(b'IMAG', imag(pal)) + chunk(b'IMAG', imag(lite))
    return diskobj + image + bytes(planes) + b'FORM' + struct.pack('>I', len(body)) + body, W, H


def png(path, px, scale=8):
    """the icon on a mid-grey background (where it is transparent), enlarged"""
    H, W = len(px), len(px[0])
    raw = bytearray()
    for y in range(H * scale):
        raw.append(0)
        for x in range(W * scale):
            c = px[y // scale][x // scale]
            raw += bytes(PAL[c] if c else (150, 150, 150))
    hdr = struct.pack('>IIBBBBB', W * scale, H * scale, 8, 2, 0, 0, 0)
    def pchunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + pchunk(b'IHDR', hdr) + pchunk(b'IDAT', zlib.compress(bytes(raw), 9)) + pchunk(b'IEND', b''))


def carr(name, data):
    s = 'static const unsigned char %s[%d] = {\n' % (name, len(data))
    for k in range(0, len(data), 16):
        s += '    ' + ', '.join('0x%02x' % b for b in data[k:k + 16]) + ',\n'
    return s + '};\n'


big = chip(32, 32, 6, 6, 25, 25, 4, "AGA")
small = chip(20, 22, 4, 5, 15, 16, 3, None)
i32, w32, h32 = make(big)
i22, w22, h22 = make(small)
HEADER = ('/* chip_icon.h - the start menu' + chr(39) + 's AGA-PISTORM icons (1.118): a chip with pins on all four\n'
          '   sides, %dx%d with "AGA" on it for the AGA-PISTORM heading, %dx%d without for its entries\n'
          '   (the sizes CaffeineOS' + chr(39) + 's own start menu icons have). OS 3.5 colour icons (FACE, two IMAG -\n'
          '   normal and selected) with a classic 2-plane image; made by mkchipicon.py. agaboot MENU writes\n'
          '   them as DOpus5:Icons/AGA-PISTORM.info and AGA-PISTORM22.info (startmenu.c). */\n')
with open(os.path.join(HERE, 'chip_icon.h'), 'w', newline='\n') as f:
    f.write(HEADER % (w32, h32, w22, h22))
    f.write(carr('chip_icon', i32))
    f.write(carr('chip_icon22', i22))
print('chip_icon.h: %d + %d bytes' % (len(i32), len(i22)))
if len(sys.argv) > 1:
    png(os.path.join(sys.argv[1], 'chip32.png'), big)
    png(os.path.join(sys.argv[1], 'chip22.png'), small)
    print('previews in', sys.argv[1])
