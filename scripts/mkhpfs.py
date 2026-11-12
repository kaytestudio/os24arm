#!/usr/bin/env python3
"""
mkhpfs.py - build an HPFS (OS/2 High Performance File System) volume image
from a host directory, for testing fs/hpfs.

    scripts/mkhpfs.py OUT.img [--size 8M] [--from DIR] [--label NAME]
                      [--fragment N]

The image is a bare HPFS volume (no partition table): boot block (BPB),
superblock (LSN 16), spareblock (LSN 17), free-space bitmaps (one 2 KB
bitmap per 8 MB band, listed from the superblock), a directory band with
its own bitmap, then fnodes, dnodes and file data.

Structure layouts follow the Linux HPFS driver (fs/hpfs/hpfs.h):
  fnode  (512 B, magic F7E40AAE): name, parent, B+ tree of extents
         (8 leaf extents or 12 internal pointers), file size
  anode  (512 B, magic 37E40AAE): B+ tree node, 40 extents or 60 pointers
  dnode  (2 KB on a 4-sector boundary, magic 77E40AAE): a directory
         B-tree node; entries sorted by upper-cased name, each may carry
         a down pointer to the dnode holding the names before it; every
         dnode ends with a dummy "last" entry.

--fragment N allocates file data in N-sector runs separated by one free
sector, so a big file needs many extents and therefore anode trees.
Names compare case-insensitively (ASCII upper-case), matching the kernel
driver.
"""
import argparse, os, struct, sys, time

SECTOR = 512
DNODE_SECTORS = 4
BAND = 16384                    # sectors per bitmap band (2 KB bitmap)

FNODE_MAGIC = 0xF7E40AAE
ANODE_MAGIC = 0x37E40AAE
DNODE_MAGIC = 0x77E40AAE
SB_MAGIC = (0xF995E849, 0xFA53E9C5)
SP_MAGIC = (0xF9911849, 0xFA5229C5)

DE_FIRST, DE_DOWN, DE_LAST = 0x01, 0x04, 0x08
ATTR_DIR, ATTR_ARCHIVE, ATTR_NOT83 = 0x10, 0x20, 0x40
BP_INTERNAL, BP_FNODE_PARENT = 0x80, 0x20
DNODE_HDR = 20
DNODE_SIZE = DNODE_SECTORS * SECTOR


def size_arg(s):
    s = s.upper()
    mult = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}.get(s[-1:], 1)
    return int(s[:-1] if s[-1:] in 'KMG' else s) * mult


def is_83(name):
    if name.upper() != name or name.count('.') > 1 or ' ' in name:
        return False
    base, _, ext = name.partition('.')
    return 0 < len(base) <= 8 and len(ext) <= 3


class Volume:
    def __init__(self, nsect, fragment):
        self.n = nsect
        self.img = bytearray(nsect * SECTOR)
        self.used = bytearray(nsect)        # 1 = allocated
        self.next = 0
        self.fragment = fragment
        self.dband, self.dband_slots, self.dband_used = 0, 0, []

    def mark(self, lsn, count=1):
        for s in range(lsn, lsn + count):
            self.used[s] = 1

    def alloc(self, count, align=1):
        s = self.next
        while True:
            s = (s + align - 1) // align * align
            if s + count > self.n:
                sys.exit("mkhpfs: volume full")
            if not any(self.used[s:s + count]):
                break
            s += 1
        self.mark(s, count)
        self.next = s + count
        return s

    def alloc_dnode(self):
        """Dnodes go in the directory band while it has room."""
        for k in range(self.dband_slots):
            if not self.dband_used[k]:
                self.dband_used[k] = 1
                return self.dband + DNODE_SECTORS * k
        return self.alloc(DNODE_SECTORS, DNODE_SECTORS)

    def put(self, lsn, data):
        self.img[lsn * SECTOR:lsn * SECTOR + len(data)] = data


def bplus(flags, nodes, capacity, leaf):
    """B+ tree header + node array (leaf: (file_secno, length, disk) /
    internal: (file_secno, down))."""
    size = 12 if leaf else 8
    hdr = struct.pack('<B3xBBH', flags, capacity - len(nodes), len(nodes),
                      8 + size * len(nodes))
    body = b''.join(struct.pack('<III', *n) if leaf else struct.pack('<II', *n)
                    for n in nodes)
    return hdr + body.ljust(capacity * size, b'\0')


def fnode(name, up, size, btree, is_dir):
    nb = name.encode('latin-1')
    f = bytearray(SECTOR)
    struct.pack_into('<I', f, 0, FNODE_MAGIC)
    f[12] = min(len(nb), 255)
    f[13:13 + min(15, len(nb))] = nb[:15]
    struct.pack_into('<I', f, 28, up)
    f[55] = 1 if is_dir else 0                     # dirflag
    f[56:56 + 104] = btree                         # header + 96 bytes
    struct.pack_into('<I', f, 160, size)
    struct.pack_into('<H', f, 184, 0xC4)           # ea_offs
    return bytes(f)


class Builder:
    def __init__(self, vol, stamp):
        self.v = vol
        self.stamp = stamp

    # ── files ──────────────────────────────────────────────────────────
    def write_data(self, data):
        n = (len(data) + SECTOR - 1) // SECTOR
        extents, fsec = [], 0
        run = self.v.fragment or n or 1
        while fsec < n:
            k = min(run, n - fsec)
            d = self.v.alloc(k)
            if self.v.fragment:
                self.v.next += 1                   # leave a gap
            self.v.put(d, data[fsec * SECTOR:(fsec + k) * SECTOR])
            if extents and extents[-1][2] + extents[-1][1] == d:
                e = extents[-1]
                extents[-1] = (e[0], e[1] + k, e[2])
            else:
                extents.append((fsec, k, d))
            fsec += k
        return extents

    def file_btree(self, extents, fnode_lsn):
        if len(extents) <= 8:
            return bplus(0, extents, 8, True)
        leaves = [extents[i:i + 40] for i in range(0, len(extents), 40)]
        if len(leaves) > 12:
            sys.exit("mkhpfs: file too fragmented (max 480 extents)")
        ptrs = []
        for i, chunk in enumerate(leaves):
            a = self.v.alloc(1)
            an = bytearray(SECTOR)
            struct.pack_into('<III', an, 0, ANODE_MAGIC, a, fnode_lsn)
            an[12:12 + 8 + 480] = bplus(BP_FNODE_PARENT, chunk, 40, True)
            self.v.put(a, an)
            bound = leaves[i + 1][0][0] if i + 1 < len(leaves) else 0xFFFFFFFF
            ptrs.append((bound, a))
        return bplus(BP_INTERNAL, ptrs, 12, False)

    def add_file(self, path, name, parent):
        data = open(path, 'rb').read()
        f = self.v.alloc(1)
        ext = self.write_data(data)
        self.v.put(f, fnode(name, parent, len(data), self.file_btree(ext, f), False))
        return f, len(data), ATTR_ARCHIVE

    # ── directories ────────────────────────────────────────────────────
    def dirent(self, name, flags, attr, fnode_lsn, size, down=None):
        nb = name if isinstance(name, bytes) else name.encode('latin-1')
        length = (31 + len(nb) + 3) & ~3
        if down is not None:
            length += 4
            flags |= DE_DOWN
        e = bytearray(length)
        struct.pack_into('<HBBIIIIIIBBB', e, 0, length, flags, attr, fnode_lsn,
                         self.stamp, size, self.stamp, self.stamp, 0, 0, 0, len(nb))
        e[31:31 + len(nb)] = nb
        if down is not None:
            struct.pack_into('<I', e, length - 4, down)
        return bytes(e)

    # A dnode tree node: items = [(entry, child-or-None)], tail = the child
    # hanging off the "last" entry. A child holds the names before its item.
    @staticmethod
    def _esize(e, child):
        nb = e[0] if isinstance(e[0], bytes) else e[0].encode('latin-1')
        return ((31 + len(nb) + 3) & ~3) + (4 if child is not None else 0)

    def _tree(self, items, tail):
        """Bottom-up B-tree of any depth: if the items don't fit one dnode,
        split them into groups, promote one item per boundary, and build
        the level above from the promoted items."""
        cap = DNODE_SIZE - DNODE_HDR - 36             # room for "last" + down
        if sum(self._esize(e, c) for e, c in items) <= cap:
            return (items, tail)
        target = cap * 3 // 4
        groups, promoted, cur, used = [], [], [], 0
        for e, c in items:
            sz = self._esize(e, c)
            if cur and used + sz > target:
                groups.append(cur)
                promoted.append((e, c))               # its child closes the group
                cur, used = [], 0
                continue
            cur.append((e, c))
            used += sz
        if cur:
            groups.append(cur)
            last_tail = tail
        else:                                         # ended on a promotion
            e, c = promoted.pop()
            groups[-1].append((e, c))
            last_tail = tail
        children = []
        for i, g in enumerate(groups):
            gtail = promoted[i][1] if i < len(promoted) else last_tail
            children.append(self._tree(g, gtail))
        parent_items = [(promoted[i][0], children[i]) for i in range(len(promoted))]
        return self._tree(parent_items, children[-1])

    def _write_tree(self, node, up, root):
        items, tail = node
        lsn = self.v.alloc_dnode()
        downs = [self._write_tree(c, lsn, False) if c is not None else None
                 for _, c in items]
        last = self._write_tree(tail, lsn, False) if tail is not None else None
        return self.write_dnode([e for e, _ in items], downs, last, up, root, lsn)

    def build_dnodes(self, entries, up, root):
        """entries: (name, flags, attr, fnode, size), sorted. Returns the
        root dnode's LSN."""
        return self._write_tree(self._tree([(e, None) for e in entries], None), up, root)

    def write_dnode(self, entries, downs, last_down, up, root, lsn=None):
        if lsn is None:
            lsn = self.v.alloc_dnode()
        body = b''.join(self.dirent(e[0], e[1], e[2], e[3], e[4], d)
                        for e, d in zip(entries, downs))
        body += self.dirent(b'\xff', DE_LAST, 0, 0, 0, last_down)
        d = bytearray(DNODE_SIZE)
        struct.pack_into('<II', d, 0, DNODE_MAGIC, DNODE_HDR + len(body))
        d[8] = 1 if root else 0
        struct.pack_into('<II', d, 12, up, lsn)
        d[DNODE_HDR:DNODE_HDR + len(body)] = body
        self.v.put(lsn, d)
        return lsn

    def add_dir(self, path, name, parent, me=None):
        if me is None:
            me = self.v.alloc(1)
        if parent is None:
            parent = me                                # root: its own parent
        entries = [(b'\x01\x01', DE_FIRST, ATTR_DIR, parent, 0)]
        kids = []
        for child in sorted(os.listdir(path), key=lambda s: s.upper()) if path else []:
            full = os.path.join(path, child)
            if os.path.isdir(full):
                lsn, size, attr = self.add_dir(full, child, me)
            else:
                lsn, size, attr = self.add_file(full, child, me)
            if not is_83(child):
                attr |= ATTR_NOT83
            kids.append((child, 0, attr, lsn, size))
        names = [k[0].upper() for k in kids]
        if len(set(names)) != len(names):
            sys.exit("mkhpfs: names differ only in case in " + path)
        entries += kids
        root = self.build_dnodes(entries, me, True)
        self.v.put(me, fnode(name, parent, 0, bplus(0, [(0, DNODE_SECTORS, root)], 8, True), True))
        return me, 0, ATTR_DIR


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out')
    ap.add_argument('--size', type=size_arg, default=8 << 20)
    ap.add_argument('--from', dest='src')
    ap.add_argument('--label', default='OS2HPFS')
    ap.add_argument('--fragment', type=int, default=0)
    a = ap.parse_args()

    nsect = a.size // SECTOR
    if nsect < 2048:
        sys.exit("mkhpfs: volume too small")
    v = Volume(nsect, a.fragment)
    stamp = int(time.mktime((2026, 11, 12, 12, 0, 0, 0, 0, -1)))

    v.mark(0, 18)                                      # boot area, super, spare
    nbands = (nsect + BAND - 1) // BAND
    bitmap_list = v.alloc(4)
    bitmaps = [v.alloc(4) for _ in range(nbands)]
    badblocks = v.alloc(4)
    uid_table = v.alloc(8)
    dir_band = v.alloc(64, DNODE_SECTORS)               # 16 dnodes, reserved
    dir_band_bitmap = v.alloc(4)
    v.dband, v.dband_slots, v.dband_used = dir_band, 16, [0] * 16

    b = Builder(v, stamp)
    root = v.alloc(1)
    b.add_dir(a.src, '', None, root)

    # Boot block: BPB that says "HPFS", for anything that checks.
    boot = bytearray(SECTOR)
    boot[0:3] = b'\xEB\x3C\x90'
    boot[3:11] = b'IBM 20.0'
    struct.pack_into('<HBHBHHBHHHII', boot, 11, SECTOR, 1, 0, 0, 0,
                     0 if nsect > 0xFFFF else nsect, 0xF8, 0, 63, 255, 0,
                     nsect if nsect > 0xFFFF else 0)
    struct.pack_into('<BBBI', boot, 36, 0x80, 0, 0x29, 0x12345678)
    boot[43:54] = a.label.upper().encode()[:11].ljust(11)
    boot[54:62] = b'HPFS    '
    boot[510:512] = b'\x55\xAA'
    v.put(0, boot)

    sb = bytearray(SECTOR)
    struct.pack_into('<IIBBHIIIIIIIIIIIII', sb, 0, SB_MAGIC[0], SB_MAGIC[1],
                     2, 2, 0, root, nsect, 0, bitmap_list, 0, badblocks, 0,
                     0, 0, 64, dir_band, dir_band + 63, dir_band_bitmap)
    sb[64:96] = a.label.encode()[:32].ljust(32, b'\0')
    struct.pack_into('<I', sb, 96, uid_table)
    v.put(16, sb)

    sp = bytearray(SECTOR)
    struct.pack_into('<II', sp, 0, SP_MAGIC[0], SP_MAGIC[1])
    v.put(17, sp)

    # Free-space bitmaps: bit set = free.
    lst = bytearray(4 * SECTOR)
    for i, bm in enumerate(bitmaps):
        struct.pack_into('<I', lst, 4 * i, bm)
        bits = bytearray(4 * SECTOR)
        for s in range(i * BAND, min(nsect, (i + 1) * BAND)):
            if not v.used[s]:
                bits[(s - i * BAND) // 8] |= 1 << (s % 8)
        v.put(bm, bits)
    v.put(bitmap_list, lst)

    dbits = bytearray(4 * SECTOR)                       # bit set = free dnode
    for k in range(16):
        if not v.dband_used[k]:
            dbits[k // 8] |= 1 << (k % 8)
    v.put(dir_band_bitmap, dbits)

    open(a.out, 'wb').write(v.img)
    print(f"mkhpfs: {a.out}: {nsect} sectors, root fnode {root}, "
          f"{sum(v.used)} sectors used")


if __name__ == '__main__':
    main()
