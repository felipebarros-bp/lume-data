#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Generates PNG / JPEG fixtures (python zlib, macOS sips) into a scratch folder and runs the
# decoder checks against them.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/image_fixtures
D=build/image_fixtures
python3 - "$D" <<'PY'
import struct, zlib, sys, os
d = sys.argv[1]
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
def png(path, w, h, depth, ctype, rows, plte=None, trns=None, interlace=0, raw_override=None, level=9):
    ihdr = struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace)
    raw = raw_override if raw_override is not None else b''.join(rows)
    data = b''.join([b'\x89PNG\r\n\x1a\n', chunk(b'IHDR', ihdr)])
    if plte: data += chunk(b'PLTE', plte)
    if trns: data += chunk(b'tRNS', trns)
    data += chunk(b'IDAT', zlib.compress(raw, level)) + chunk(b'IEND', b'')
    open(os.path.join(d, path), 'wb').write(data)
    return data
# rgb 4x3: row0 filter Sub, row1 Up, row2 Paeth; pixel (0,0) red, (3,2) = (10,20,30)
pix = [[(255,0,0),(0,255,0),(0,0,255),(9,9,9)], [(1,2,3),(4,5,6),(7,8,9),(10,11,12)], [(100,100,100),(50,60,70),(0,0,0),(10,20,30)]]
def sub(row, bpp):
    out = bytearray()
    for i, v in enumerate(row): out.append((v - (row[i-bpp] if i >= bpp else 0)) & 255)
    return bytes(out)
def up(row, prev): return bytes((a - b) & 255 for a, b in zip(row, prev))
def paeth_pred(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
def paeth(row, prev, bpp):
    out = bytearray()
    for i, v in enumerate(row):
        a = row[i-bpp] if i >= bpp else 0; b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
        out.append((v - paeth_pred(a, b, c)) & 255)
    return bytes(out)
r0 = bytes(sum(pix[0], ())); r1 = bytes(sum(pix[1], ())); r2 = bytes(sum(pix[2], ()))
png('rgb.png', 4, 3, 8, 2, [b'\x01' + sub(r0, 3), b'\x02' + up(r1, r0), b'\x04' + paeth(r2, r1, 3)])
# rgba 16-bit 2x2, pixel (1,1) = blue with alpha 0x80
row0 = b'\x00' + struct.pack('>8H', 0xffff,0,0,0xffff, 0,0xffff,0,0xffff)
row1 = b'\x00' + struct.pack('>8H', 0,0,0,0xffff, 0,0,0xffff,0x8000)
png('rgba16.png', 2, 2, 16, 6, [row0, row1])
# 2-bit palette 8x1: indices 0..3 repeated; palette: green, yellow, red, blue; tRNS makes index 1 transparent
idx = [0,1,2,3,0,1,2,3]
packed = bytearray()
for i in range(0, 8, 4): packed.append((idx[i]<<6)|(idx[i+1]<<4)|(idx[i+2]<<2)|idx[i+3])
png('pal.png', 8, 1, 2, 3, [b'\x00' + bytes(packed)], plte=bytes([0,255,0, 255,255,0, 255,0,0, 0,0,255]), trns=bytes([255, 0]))
# 1-bit gray 10x1: 1 0 1 0 ...
png('gray1.png', 10, 1, 1, 0, [b'\x00' + bytes([0b10101010, 0b10000000])])
# 300x200 rgb gradient: pixel (x,y) = (x, y, (x+y)%256)
rows = [b'\x00' + bytes(v for x in range(300) for v in (x & 255, y & 255, (x + y) & 255)) for y in range(200)]
png('big.png', 300, 200, 8, 2, rows)
full = open(os.path.join(d, 'big.png'), 'rb').read()
open(os.path.join(d, 'truncated.png'), 'wb').write(full[:len(full)//2])
png('bomb.png', 100000, 100000, 8, 2, [b'\x00' + b'\x00' * 12])
png('lying.png', 2, 2, 8, 0, [], raw_override=b'\x00' * 5000)
open(os.path.join(d, 'garbage.png'), 'wb').write(b'\x89PNG\r\n\x1a\n' + os.urandom(400))
png('interlaced.png', 4, 4, 8, 0, [b'\x00' + b'\x80'*4]*4, interlace=1)
# source for the JPEG: 64x48 with a red left half and a blue right half; 16x16 mid grey
rows = [b'\x00' + b''.join((bytes([200,30,30]) if x < 32 else bytes([30,30,200])) for x in range(64)) for y in range(48)]
png('photo_src.png', 64, 48, 8, 2, rows)
png('gray_src.png', 16, 16, 8, 0, [b'\x00' + bytes([128]*16)]*16)
PY
if command -v sips >/dev/null; then
  sips -s format jpeg -s formatOptions 90 "$D/photo_src.png" --out "$D/photo.jpg" >/dev/null
  sips -s format jpeg "$D/gray_src.png" --out "$D/gray.jpg" >/dev/null
fi
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -o build/image_check tests/image_check.c src/image.c -lm
build/image_check "$D"
