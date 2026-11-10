#!/usr/bin/env python3
"""Build the QEMU_RISCV test SD card image (make mksdcard / make run).

Layout (MBR, 128 MB - QEMU's SD card model needs a power-of-two size):
  p1  FAT32, active (boot) - becomes C: in the kernel. Holds the repo's
      CONFIG.SYS, a README.TXT and a DOCS\\NOTE.TXT subdirectory file.
  p2  ext4 (only with --mke2fs) - mounted at \\mmc0p2. Built with
      `mke2fs -d`, so it needs e2fsprogs 1.43+ (brew install e2fsprogs).

The FAT32 volume is written directly by this script (8.3 names, no LFN),
so no mtools/mkfs.fat is needed. Usage:

    mksdcard.py [--mke2fs PATH] OUTPUT.img
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
CARD_BYTES = 128 * 1024 * 1024
FIRST_LBA = 2048
FAT_BYTES = 64 * 1024 * 1024        # >= 65525 clusters at 512 B/cluster: FAT32
EXT4_BYTES = 48 * 1024 * 1024

FAT_RESERVED = 32
FAT_COPIES = 2
FAT_EOC = 0x0FFFFFFF
ATTR_DIR = 0x10
ATTR_ARCHIVE = 0x20


def name83(name):
    if name in ('.', '..'):
        return name.encode().ljust(11)
    base, _, ext = name.upper().partition('.')
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError('not an 8.3 name: %r' % name)
    return base.encode().ljust(8) + ext.encode().ljust(3)


def dirent(name, attr, cluster, size):
    return name83(name) + struct.pack('<BBBHHHHHHHI', attr, 0, 0, 0, 0, 0,
                                      cluster >> 16, 0, 0, cluster & 0xFFFF, size)


def build_fat32(tree, total_sectors, sectors_per_cluster=1):
    """tree: {name: bytes | {name: ...}} -> FAT32 volume image bytes."""
    cluster_bytes = sectors_per_cluster * SECTOR
    fat_sectors = ((total_sectors // sectors_per_cluster + 2) * 4 + SECTOR - 1) // SECTOR
    data_start = FAT_RESERVED + FAT_COPIES * fat_sectors
    clusters = (total_sectors - data_start) // sectors_per_cluster
    assert clusters >= 65525, 'volume too small for FAT32'

    fat = [0x0FFFFFF8, FAT_EOC]
    data = {}

    def alloc(payload):
        count = max(1, (len(payload) + cluster_bytes - 1) // cluster_bytes)
        first = len(fat)
        for i in range(count):
            fat.append(first + i + 1 if i < count - 1 else FAT_EOC)
            data[first + i] = payload[i * cluster_bytes:(i + 1) * cluster_bytes]
        return first

    def write_dir(entries, self_cluster, parent_cluster):
        raw = b''
        if parent_cluster is not None:          # not the root
            raw += dirent('.', ATTR_DIR, self_cluster, 0)
            raw += dirent('..', ATTR_DIR, parent_cluster, 0)   # 0 = root
        for name, content in entries.items():
            if isinstance(content, dict):
                sub = alloc(b'')
                write_dir(content, sub, 0 if parent_cluster is None else self_cluster)
                raw += dirent(name, ATTR_DIR, sub, 0)
            else:
                raw += dirent(name, ATTR_ARCHIVE, alloc(content), len(content))
        assert len(raw) <= cluster_bytes, 'directory needs more than one cluster'
        data[self_cluster] = raw

    root = alloc(b'')
    write_dir(tree, root, None)

    image = bytearray(total_sectors * SECTOR)
    boot = bytearray(SECTOR)
    boot[0:3] = b'\xEB\x58\x90'
    boot[3:11] = b'OS2WARP '
    struct.pack_into('<HBHBHHBHHHII', boot, 11, SECTOR, sectors_per_cluster, FAT_RESERVED,
                     FAT_COPIES, 0, 0, 0xF8, 0, 63, 255, FIRST_LBA, total_sectors)
    struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, root, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    boot[71:82] = b'OS2BOOT    '
    boot[82:90] = b'FAT32   '
    boot[510:512] = b'\x55\xAA'
    image[0:SECTOR] = boot

    fat_raw = b''.join(struct.pack('<I', v) for v in fat)
    for copy in range(FAT_COPIES):
        off = (FAT_RESERVED + copy * fat_sectors) * SECTOR
        image[off:off + len(fat_raw)] = fat_raw
    for cluster, blob in data.items():
        off = (data_start + (cluster - 2) * sectors_per_cluster) * SECTOR
        image[off:off + len(blob)] = blob
    return bytes(image)


def build_ext4(mke2fs, files):
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, 'root')
        for rel, content in files.items():
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as f:
                f.write(content)
        img = os.path.join(tmp, 'ext4.img')
        subprocess.run([mke2fs, '-q', '-F', '-t', 'ext4', '-d', root, img,
                        '%dk' % (EXT4_BYTES // 1024)], check=True)
        with open(img, 'rb') as f:
            return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--mke2fs', help='mke2fs binary; adds an ext4 partition 2')
    ap.add_argument('output')
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(repo, 'CONFIG.SYS'), 'rb') as f:
        config_sys = f.read()

    fat = build_fat32({
        'README.TXT': (b'OS/2 Warp 4.52 - QEMU RISC-V test card.\r\n'
                       b'This is drive C:, the FAT32 boot partition (mmc0p1).\r\n'
                       b'Try: DIR, TYPE CONFIG.SYS, CD DOCS, DIR \\mmc0p2\r\n'),
        'CONFIG.SYS': config_sys,
        'DOCS': {'NOTE.TXT': b'A file in a FAT32 subdirectory.\r\n'},
    }, FAT_BYTES // SECTOR)

    parts = [(0x0C, True, fat)]                      # FAT32 (LBA), active
    if args.mke2fs:
        parts.append((0x83, False, build_ext4(args.mke2fs, {
            'HELLO.TXT': b'Hello from the ext4 partition (\\mmc0p2).\n',
            'docs/NOTE.TXT': b'ext4 is case-sensitive: this is docs\\NOTE.TXT.\n',
        })))

    card = bytearray(CARD_BYTES)
    mbr = bytearray(SECTOR)
    lba = FIRST_LBA
    for i, (ptype, active, blob) in enumerate(parts):
        sectors = len(blob) // SECTOR
        struct.pack_into('<B3sB3sII', mbr, 446 + 16 * i, 0x80 if active else 0,
                         b'\0\0\0', ptype, b'\0\0\0', lba, sectors)
        card[lba * SECTOR:lba * SECTOR + len(blob)] = blob
        lba += sectors
    assert lba * SECTOR <= CARD_BYTES
    mbr[510:512] = b'\x55\xAA'
    card[0:SECTOR] = mbr

    with open(args.output, 'wb') as f:
        f.write(card)
    print('%s: %d MB card, FAT32 boot partition%s'
          % (args.output, CARD_BYTES >> 20, ' + ext4' if args.mke2fs else ''))


if __name__ == '__main__':
    sys.exit(main())
