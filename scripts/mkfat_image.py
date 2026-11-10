#!/usr/bin/env python3
"""scripts/mkfat_image.py — build a raw FAT16 filesystem image readable by
this project's FAT driver (kernel/src/fat32.c).

The FAT variant is decided by cluster count, as the Microsoft FAT spec
requires (and fat32.c follows), so the image size must give 4085..65524
clusters - e.g. 4 MiB at the default 512-byte cluster. Files go in the
root directory under 8.3 names; no mtools/mkfs.fat is needed, so this
works the same on macOS and Linux.

Usage: mkfat_image.py <output.img> <size_bytes> [--label NAME] [name:hostfile ...]
Example: mkfat_image.py boot.img 4194304 --label BOOT BOOTINFO.TXT:bootinfo.txt
"""
import struct
import sys

SECTOR = 512
RESERVED = 1
FAT_COPIES = 2
ROOT_ENTRIES = 512
ATTR_VOLUME = 0x08
ATTR_ARCHIVE = 0x20
FAT16_EOC = 0xFFFF


def name83(name):
    base, _, ext = name.upper().partition('.')
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError('not an 8.3 name: %r' % name)
    return base.encode().ljust(8) + ext.encode().ljust(3)


def dirent(name11, attr, cluster, size):
    return name11 + struct.pack('<BBBHHHHHHHI', attr, 0, 0, 0, 0, 0,
                                0, 0, 0, cluster, size)


def build_fat16(total_sectors, label, files):
    spc = 1
    while True:
        root_sectors = ROOT_ENTRIES * 32 // SECTOR
        fat_sectors = ((total_sectors // spc + 2) * 2 + SECTOR - 1) // SECTOR
        data_start = RESERVED + FAT_COPIES * fat_sectors + root_sectors
        clusters = (total_sectors - data_start) // spc
        if clusters < 65525:
            break
        spc *= 2
    if clusters < 4085:
        raise ValueError('volume too small for FAT16 (%d clusters)' % clusters)

    cluster_bytes = spc * SECTOR
    image = bytearray(total_sectors * SECTOR)

    boot = bytearray(SECTOR)
    boot[0:3] = b'\xEB\x3C\x90'
    boot[3:11] = b'OS2WARP '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, SECTOR, spc, RESERVED, FAT_COPIES,
                     ROOT_ENTRIES, total_sectors if total_sectors < 0x10000 else 0,
                     0xF8, fat_sectors, 63, 255, 0,
                     total_sectors if total_sectors >= 0x10000 else 0)
    boot[36] = 0x80                                 # drive number
    boot[38] = 0x29                                 # extended boot signature
    struct.pack_into('<I', boot, 39, 0x0524F2A4)    # volume serial
    boot[43:54] = label.upper().encode()[:11].ljust(11)
    boot[54:62] = b'FAT16   '
    boot[510:512] = b'\x55\xAA'
    image[0:SECTOR] = boot

    fat = [0xFFF8, FAT16_EOC]
    root = dirent(boot[43:54], ATTR_VOLUME, 0, 0)
    for name, payload in files:
        count = (len(payload) + cluster_bytes - 1) // cluster_bytes
        first = len(fat) if count else 0
        for i in range(count):
            fat.append(first + i + 1 if i < count - 1 else FAT16_EOC)
            off = (data_start + (first + i - 2) * spc) * SECTOR
            chunk = payload[i * cluster_bytes:(i + 1) * cluster_bytes]
            image[off:off + len(chunk)] = chunk
        root += dirent(name83(name), ATTR_ARCHIVE, first, len(payload))
    if len(fat) - 2 > clusters:
        raise ValueError('files do not fit in the volume')
    if len(root) > ROOT_ENTRIES * 32:
        raise ValueError('too many files for the root directory')

    fat_raw = b''.join(struct.pack('<H', v) for v in fat)
    for copy in range(FAT_COPIES):
        off = (RESERVED + copy * fat_sectors) * SECTOR
        image[off:off + len(fat_raw)] = fat_raw
    off = (RESERVED + FAT_COPIES * fat_sectors) * SECTOR
    image[off:off + len(root)] = root
    return bytes(image)


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 1
    out, size = argv[1], int(argv[2])
    rest = argv[3:]
    label = 'NO NAME'
    if len(rest) >= 2 and rest[0] == '--label':
        label, rest = rest[1], rest[2:]
    files = []
    for spec in rest:
        name, _, host = spec.partition(':')
        with open(host, 'rb') as f:
            files.append((name, f.read()))
    with open(out, 'wb') as f:
        f.write(build_fat16(size // SECTOR, label, files))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
