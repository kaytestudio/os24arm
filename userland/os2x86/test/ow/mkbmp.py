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
