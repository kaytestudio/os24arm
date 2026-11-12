#!/usr/bin/env python3
"""
hpfsck.py - check an HPFS volume image for consistency (read-only).

    scripts/hpfsck.py IMAGE [--part N] [--ls] [--cat PATH] [-q]

IMAGE may be a bare HPFS volume, or a disk with an MBR (primary or
logical partitions; --part picks the N-th HPFS partition, default the
first) or GPT. Exit status is the number of errors (0 = consistent).

Checks, independently of the kernel driver:
  - superblock / spareblock signatures, dirty flag, hotfix list
  - every sector owned by at most one structure: boot area, superblock,
    spareblock, bitmap list and bitmaps, bad-block list, directory band
    and its bitmap, user ID table, hotfix list and spares, code page
    directory / data, fnodes, anodes, dnodes, file data
  - the free-space bitmaps agree with that: a used sector marked free is
    an error, a free-marked-used one is reported as lost space
  - the directory band bitmap agrees with the dnodes inside the band
  - directory B-trees: signatures, self / up / root-flag pointers, entry
    bounds, '..' first, sorted by the volume's code page collation, end
    marker in every dnode
  - fnodes: signature, parent pointer, name prefix, directory flag
  - files: extents in order from sector 0, cover the size; directory
    entry size == fnode size
"""
import argparse, struct, sys

OUT = sys.stdout                        # reports (stderr when --cat writes data)


def say(*a):
    print(*a, file=OUT)

SECTOR = 512
SB_MAGIC = (0xF995E849, 0xFA53E9C5)
SP_MAGIC = (0xF9911849, 0xFA5229C5)
FNODE, ANODE, DNODE = 0xF7E40AAE, 0x37E40AAE, 0x77E40AAE
DE_FIRST, DE_DOWN, DE_LAST = 0x01, 0x04, 0x08


class Fsck:
    def __init__(self, img, base, quiet):
        self.img, self.base, self.quiet = img, base, quiet
        self.errors = self.warnings = 0
        self.owner = {}
        self.files = {}

    def sec(self, lsn, n=1):
        o = (self.base + lsn) * SECTOR
        if o + n * SECTOR > len(self.img):
            raise ValueError(f"sector {lsn} beyond the image")
        return self.img[o:o + n * SECTOR]

    def err(self, msg):
        self.errors += 1
        say("ERROR:", msg)

    def warn(self, msg):
        self.warnings += 1
        if not self.quiet:
            say("warning:", msg)

    def own(self, lsn, n, what):
        for s in range(lsn, lsn + n):
            if s >= self.n:
                self.err(f"{what}: sector {s} beyond the volume")
                return
            if s in self.owner:
                self.err(f"sector {s} used by both {self.owner[s]} and {what}")
            else:
                self.owner[s] = what

    def u32(self, b, o): return struct.unpack_from('<I', b, o)[0]
    def u16(self, b, o): return struct.unpack_from('<H', b, o)[0]

    def key(self, name):
        return bytes(c - 32 if 0x61 <= c <= 0x7A else
                     self.upcase[c - 0x80] if (self.upcase and 0x80 <= c < 0xFF) else c
                     for c in name)

    # ── fixed structures ────────────────────────────────────────────────
    def check_fixed(self):
        sb = self.sec(16)
        if (self.u32(sb, 0), self.u32(sb, 4)) != SB_MAGIC:
            self.err("bad superblock signature")
            return False
        self.n = self.u32(sb, 16)
        self.root = self.u32(sb, 12)
        say(f"HPFS v{sb[8]}.{sb[9]}, {self.n} sectors, root fnode {self.root}")
        self.own(0, 16, "boot area")
        self.own(16, 1, "superblock")
        self.own(17, 1, "spareblock")

        bl = self.u32(sb, 24)
        self.own(bl, 4, "bitmap list")
        lst = self.sec(bl, 4)
        self.bitmaps = []
        nb = (self.n + 16383) // 16384
        for b in range(nb):
            bm = self.u32(lst, 4 * b)
            self.own(bm, 4, f"bitmap {b}")
            self.bitmaps.append(self.sec(bm, 4))
        bad = self.u32(sb, 32)
        if bad:
            self.own(bad, 4, "bad block list")
        self.dband_n = self.u32(sb, 48)
        self.dband = (self.u32(sb, 52), self.u32(sb, 56))
        self.dband_bm = self.u32(sb, 60)
        if self.dband_n:
            self.own(self.dband[0], self.dband_n, "directory band")
            self.own(self.dband_bm, 4, "directory band bitmap")
            self.dbm = self.sec(self.dband_bm, 4)
        uid = self.u32(sb, 96)
        if uid:
            self.own(uid, 8, "user ID table")

        sp = self.sec(17)
        if (self.u32(sp, 0), self.u32(sp, 4)) != SP_MAGIC:
            self.err("bad spareblock signature")
            return False
        if sp[8] & 1:
            self.warn("volume is marked dirty (not cleanly unmounted)")
        hmap, used, nspare = self.u32(sp, 12), self.u32(sp, 16), self.u32(sp, 20)
        if nspare:
            self.own(hmap, 4, "hotfix list")
            hm = self.sec(hmap, 4)
            for i in range(nspare):
                self.own(self.u32(hm, 4 * (nspare + i)), 1, "spare sector")
            if used:
                say(f"{used} hotfixed sector(s)")
        self.upcase = None
        cpd = self.u32(sp, 32)
        if cpd:
            self.own(cpd, 1, "code page directory")
            d = self.sec(cpd)
            if self.u32(d, 0) != 0x494521F7:
                self.err("bad code page directory signature")
            else:
                seen = set()
                for i in range(self.u32(d, 4)):
                    data, idx = self.u32(d, 16 + 16 * i + 8), self.u16(d, 16 + 16 * i + 12)
                    if data not in seen:
                        self.own(data, 1, "code page data")
                        seen.add(data)
                    cp = self.sec(data)
                    off = self.u16(cp, 20 + 2 * idx)
                    if i == 0 and self.u32(cp, 0) == 0x894521F7:
                        self.upcase = cp[off + 6:off + 6 + 128]
                        say(f"code page {self.u16(cp, off + 2)}")
        return True

    # ── files and directories ───────────────────────────────────────────
    def extents(self, lsn, bt, cap_leaf, cap_int, depth, out, what):
        flags, n = bt[0], bt[5]
        if depth > 8:
            self.err(f"{what}: extent tree too deep")
            return
        if flags & 0x80:
            if n > cap_int:
                self.err(f"{what}: {n} internal pointers (max {cap_int})")
                return
            for i in range(n):
                a = self.u32(bt, 8 + 8 * i + 4)
                an = self.sec(a)
                if self.u32(an, 0) != ANODE or self.u32(an, 4) != a:
                    self.err(f"{what}: bad anode {a}")
                    continue
                self.own(a, 1, f"anode of {what}")
                self.extents(a, an[12:], 40, 60, depth + 1, out, what)
        else:
            if n > cap_leaf:
                self.err(f"{what}: {n} extents (max {cap_leaf})")
                return
            for i in range(n):
                out.append(struct.unpack_from('<III', bt, 8 + 12 * i))

    def check_file(self, lsn, path, dsize):
        f = self.sec(lsn)
        ext = []
        self.extents(lsn, f[56:56 + 104], 8, 12, 0, ext, path)
        size = self.u32(f, 160)
        pos = 0
        data = bytearray()
        for fs, ln, disk in ext:
            if fs != pos:
                self.err(f"{path}: extent starts at file sector {fs}, expected {pos}")
            self.own(disk, ln, f"data of {path}")
            data += self.sec(disk, ln)
            pos = fs + ln
        if pos * SECTOR < size:
            self.err(f"{path}: {size} bytes but only {pos} sectors allocated")
        elif pos > (size + SECTOR - 1) // SECTOR:
            self.warn(f"{path}: {pos} sectors allocated for {size} bytes")
        if dsize != size:
            self.err(f"{path}: directory entry says {dsize} bytes, fnode {size}")
        self.files[path.upper()] = bytes(data[:size])

    def check_fnode(self, lsn, parent, name, path):
        f = self.sec(lsn)
        if self.u32(f, 0) != FNODE:
            self.err(f"{path}: bad fnode {lsn}")
            return None
        self.own(lsn, 1, f"fnode of {path}")
        # external EAs (+44 size, +48 sector, +54 bit 1 anode) and ACL (+32/+36/+42)
        for size, secno, anode, what in ((self.u32(f, 44), self.u32(f, 48), f[54] & 2, "EAs"),
                                         (self.u32(f, 32), self.u32(f, 36), f[42], "ACL")):
            if not (size and secno):
                continue
            if anode:
                ext = []
                an = self.sec(secno)
                if self.u32(an, 0) != ANODE or self.u32(an, 4) != secno:
                    self.err(f"{path}: bad {what} anode {secno}")
                    continue
                self.own(secno, 1, f"{what} anode of {path}")
                self.extents(secno, an[12:], 40, 60, 1, ext, f"{what} of {path}")
                for _, ln, disk in ext:
                    self.own(disk, ln, f"{what} of {path}")
            else:
                self.own(secno, (size + SECTOR - 1) // SECTOR, f"{what} of {path}")
        if self.u32(f, 28) != parent:
            self.err(f"{path}: fnode parent {self.u32(f, 28)}, expected {parent}")
        if name is not None:
            if f[12] != len(name) or f[13:13 + min(15, len(name))] != name[:15]:
                self.err(f"{path}: fnode name doesn't match its entry")
        return f

    def check_dir(self, lsn, parent, path):
        f = self.sec(lsn)
        if not f[55] & 1:
            self.err(f"{path}: directory fnode without the directory flag")
            return
        root = self.u32(f, 72)
        names = []
        self.walk(root, lsn, True, path, names, lsn, parent, [0])
        keys = [self.key(n) for n, _, _, _ in names]
        for i in range(1, len(keys)):
            if keys[i - 1] >= keys[i]:
                self.err(f"{path}: entries out of order: {names[i-1][0]!r} >= {names[i][0]!r}")
        for name, attr, fl, size in names:
            child = f"{path.rstrip('/')}/{name.decode('cp850', 'replace')}"
            fn = self.check_fnode(fl, lsn, name, child)
            if fn is None:
                continue
            if attr & 0x10:
                self.check_dir(fl, lsn, child)
            else:
                self.check_file(fl, child, size)

    def walk(self, lsn, up, is_root, path, out, dir_fnode, parent, first_seen):
        if lsn % 4:
            self.err(f"{path}: dnode {lsn} not on a 4-sector boundary")
            return
        d = self.sec(lsn, 4)
        if self.u32(d, 0) != DNODE or self.u32(d, 16) != lsn:
            self.err(f"{path}: bad dnode {lsn}")
            return
        if self.u32(d, 12) != up:
            self.err(f"{path}: dnode {lsn} up={self.u32(d, 12)}, expected {up}")
        if bool(d[8] & 1) != is_root:
            self.err(f"{path}: dnode {lsn} root flag wrong")
        if self.dband_n and self.dband[0] <= lsn <= self.dband[1]:
            i = (lsn - self.dband[0]) // 4
            if self.dbm[i // 8] & (1 << (i % 8)):
                self.err(f"{path}: dnode {lsn} in use but free in the directory band bitmap")
        else:
            self.own(lsn, 4, f"dnode of {path}")
        end, off = self.u32(d, 4), 20
        if end > 2048:
            self.err(f"{path}: dnode {lsn} first_free {end}")
            return
        while True:
            if off + 32 > end:
                self.err(f"{path}: dnode {lsn} has no end marker")
                return
            ln, fl, attr = self.u16(d, off), d[off + 2], d[off + 3]
            nl = d[off + 30]
            if ln < 32 or off + ln > end or 31 + nl > ln:
                self.err(f"{path}: dnode {lsn} bad entry at {off}")
                return
            if fl & DE_DOWN:
                self.walk(self.u32(d, off + ln - 4), lsn, False, path, out, dir_fnode,
                          parent, first_seen)
            if fl & DE_LAST:
                return
            if fl & DE_FIRST:
                first_seen[0] += 1
                if self.u32(d, off + 4) != parent:
                    self.err(f"{path}: '..' points at {self.u32(d, off + 4)}, expected {parent}")
                if out:
                    self.err(f"{path}: '..' is not the first entry")
            else:
                out.append((bytes(d[off + 31:off + 31 + nl]), attr,
                            self.u32(d, off + 4), self.u32(d, off + 12)))
            off += ln

    # ── bitmaps ─────────────────────────────────────────────────────────
    def check_bitmaps(self):
        used_free = lost = 0
        for s in range(self.n):
            bm = self.bitmaps[s // 16384]
            b = s % 16384
            free = (bm[b // 8] >> (b % 8)) & 1
            if s in self.owner and free:
                used_free += 1
                if used_free <= 10:
                    self.err(f"sector {s} ({self.owner[s]}) is marked free")
            elif s not in self.owner and not free:
                lost += 1
        if used_free > 10:
            self.err(f"... {used_free} used sectors marked free in all")
        if lost:
            self.warn(f"{lost} sector(s) marked used but not referenced (lost space)")
        if self.dband_n:
            n = self.dband_n // 4
            free_bits = sum(1 for i in range(n) if self.dbm[i // 8] & (1 << (i % 8)))
            say(f"directory band: {n - free_bits}/{n} dnodes used")


def find_volume(img, part):
    """Sector offset of the HPFS volume: bare, MBR (+EBR chain) or GPT."""
    def is_hpfs(base):
        o = (base + 16) * SECTOR
        return len(img) >= o + 8 and struct.unpack_from('<II', img, o) == SB_MAGIC
    cands = []
    if img[510:512] == b'\x55\xAA':
        ents = [struct.unpack_from('<B3xB3xII', img, 446 + 16 * i) for i in range(4)]
        if any(e[1] == 0xEE for e in ents):              # GPT
            lba = struct.unpack_from('<Q', img, SECTOR + 72)[0]
            n, esz = struct.unpack_from('<II', img, SECTOR + 80)
            for i in range(n):
                e = img[lba * SECTOR + i * esz:lba * SECTOR + (i + 1) * esz]
                first = struct.unpack_from('<Q', e, 32)[0]
                if any(e[:16]) and is_hpfs(first):
                    cands.append(first)
        else:
            for _, t, start, count in ents:
                if t in (0x05, 0x0F):
                    ebr = start
                    for _ in range(24):
                        _, t0, s0, c0 = struct.unpack_from('<B3xB3xII', img, ebr * SECTOR + 446)
                        _, t1, s1, _c1 = struct.unpack_from('<B3xB3xII', img, ebr * SECTOR + 462)
                        if t0 and is_hpfs(ebr + s0):
                            cands.append(ebr + s0)
                        if t1 not in (0x05, 0x0F) or not s1:
                            break
                        ebr = start + s1
                elif t and is_hpfs(start):
                    cands.append(start)
    if is_hpfs(0):
        cands.insert(0, 0)
    if not cands:
        sys.exit("hpfsck: no HPFS volume found")
    if part > len(cands):
        sys.exit(f"hpfsck: only {len(cands)} HPFS volume(s)")
    return cands[part - 1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('image')
    ap.add_argument('--part', type=int, default=1)
    ap.add_argument('--ls', action='store_true', help='list the files checked')
    ap.add_argument('--cat', help='write this file (e.g. /DIR/NAME) to stdout')
    ap.add_argument('-q', action='store_true', help='errors only')
    a = ap.parse_args()
    global OUT
    if a.cat:
        OUT = sys.stderr
    img = open(a.image, 'rb').read()
    base = find_volume(img, a.part)
    if base:
        say(f"HPFS volume at sector {base}")
    f = Fsck(img, base, a.q)
    if f.check_fixed():
        f.check_fnode(f.root, f.root, None, "/")
        f.check_dir(f.root, f.root, "/")
        f.check_bitmaps()
    if a.ls:
        for p in sorted(f.files):
            say(f"  {len(f.files[p]):8d}  {p}")
    if a.cat:
        data = f.files.get(a.cat.upper())
        if data is None:
            sys.exit(f"hpfsck: {a.cat}: not found")
        sys.stdout.buffer.write(data)
        return 0
    say(f"{len(f.files)} file(s), {f.errors} error(s), {f.warnings} warning(s)")
    return min(f.errors, 125)


if __name__ == '__main__':
    sys.exit(main())
