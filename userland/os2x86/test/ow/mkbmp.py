# userland/os2x86/test/ow/mkbmp.py DIR - pmbmp's three bitmaps: a 24-bit gradient (Windows /
# OS/2 2.x header), a 4-bit checkerboard (palette), an 8-bit one with an OS/2 1.x header
import struct, sys, os
d = sys.argv[1]
def write(name, info, pal, rows):
    data = b''.join(rows)
    off = 14 + len(info) + len(pal)
    open(os.path.join(d, name), 'wb').write(b'BM' + struct.pack('<IHHI', off + len(data), 0, 0, off) + info + pal + data)
def pad(b): return b + b'\0' * (-len(b) % 4)
w, h = 64, 48
write('grad.bmp', struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, 0, 0, 0, 0, 0), b'',
      [pad(b''.join(bytes((128, y * 5, x * 4)) for x in range(w))) for y in range(h)])          # B, G, R
pal = bytes((0, 0, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0)) + b'\0' * 4 * 13                              # 1 red, 2 blue
rows = []
for y in range(32):
    row = [1 if ((x // 8) + (y // 8)) % 2 == 0 else 2 for x in range(32)]
    rows.append(pad(bytes((row[i] << 4) | row[i + 1] for i in range(0, 32, 2))))
write('check.bmp', struct.pack('<IiiHHIIiiII', 40, 32, 32, 1, 4, 0, 0, 0, 0, 16, 0), pal, rows)
pal8 = b''.join(bytes((0, 200, 0)) if i == 5 else bytes((i, i, i)) for i in range(256))          # RGB triples (1.x): 5 green
write('old.bmp', struct.pack('<IHHHH', 12, 16, 16, 1, 8), pal8, [pad(bytes([5] * 16)) for _ in range(16)])
# pmicon's: a 32x32 colour icon (CI, OS/2 1.x headers): a green disc, its corners transparent
# (AND 1, XOR 0), a 4x4 square in the middle inverting the screen (AND 1, XOR 1)
def bits1(rows):                                            # rows of 0/1, bottom first
    return b''.join(pad(bytes(sum(r[x + k] << (7 - k) for k in range(8)) for x in range(0, 32, 8))) for r in rows)
inside = lambda x, y: (2 * x + 1 - 32) ** 2 + (2 * y + 1 - 32) ** 2 <= 4 * 14 * 14
mid = lambda x, y: 14 <= x < 18 and 14 <= y < 18
andm = [[0 if inside(x, y) and not mid(x, y) else 1 for x in range(32)] for y in range(32)]
xorm = [[1 if mid(x, y) else 0 for x in range(32)] for y in range(32)]
mask = bits1(andm) + bits1(xorm)                            # the AND mask first, then the XOR
pal16 = bytes((0, 0, 0, 0, 0, 128, 0, 128, 0)) + bytes((0, 0, 0)) + bytes((0, 255, 0)) + b'\0' * 3 * 11   # (RGB) 4: green
color = b''.join(pad(bytes(0x44 for _ in range(16))) for _ in range(32))
hm = struct.pack('<IHHHH', 12, 32, 64, 1, 1) + bytes((0, 0, 0, 255, 255, 255))
hc = struct.pack('<IHHHH', 12, 32, 32, 1, 4) + pal16
off_m = 14 + len(hm) + 14 + len(hc)
off_c = off_m + len(mask)
fh = lambda off: b'CI' + struct.pack('<IHHI', 26, 0, 0, off)
open(os.path.join(d, 'trans.ico'), 'wb').write(fh(off_m) + hm + fh(off_c) + hc + mask + color)
