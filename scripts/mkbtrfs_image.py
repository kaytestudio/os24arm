#!/usr/bin/env python3
"""scripts/mkbtrfs_image.py — build a minimal raw btrfs filesystem image
readable by this project's hand-rolled read-only btrfs driver
(fs/btrfs/btrfs.c / fs/btrfs/btrfs_disk.h).

No mkfs.btrfs exists on macOS, so this constructs the on-disk structures
directly: a superblock, and one single-leaf B-tree each for the chunk
tree, root tree, and fs tree (small enough that everything fits in one
leaf node - no internal nodes needed). The whole image uses one identity
logical==physical chunk mapping, and per-item checksums are left zeroed
since btrfs.c never verifies them (confirmed by reading its mount path).

Usage: mkbtrfs_image.py <output.img> <size_bytes> [name:hostfile ...]
Example: mkbtrfs_image.py c_partition.img 8388608 CMD.EXE:build/CMD.EXE
"""
import os
import struct
import time
import sys

NODESIZE = 4096
SECTORSIZE = 4096
SUPER_OFFSET = 0x10000

CHUNK_LEAF_ADDR = 0x11000
ROOT_LEAF_ADDR = 0x12000
FS_LEAF_ADDR = 0x13000
DATA_START = 0x14000

BTRFS_ROOT_TREE_OBJECTID = 1
BTRFS_CHUNK_TREE_OBJECTID = 3
BTRFS_FS_TREE_OBJECTID = 5
BTRFS_FIRST_CHUNK_TREE_OBJECTID = 256
BTRFS_FIRST_FREE_OBJECTID = 256

INODE_ITEM_KEY = 1
DIR_INDEX_KEY = 96
EXTENT_DATA_KEY = 108
ROOT_ITEM_KEY = 132
CHUNK_ITEM_KEY = 228

S_IFDIR = 0x4000
S_IFREG = 0x8000
FT_REG_FILE = 1

HEADER_FMT = "<32s16sQQ16sQQIB"          # csum,fsid,bytenr,flags,chunk_tree_uuid,generation,owner,nritems,level
assert struct.calcsize(HEADER_FMT) == 101
ITEM_FMT = "<QBQII"                       # key(objectid,type,offset), data offset, data size
assert struct.calcsize(ITEM_FMT) == 25
INODE_FMT = "<QQQQQIIIIQQQ32s12s12s12s12s"  # generation..sequence, reserved[32], 4x timespec[12]
assert struct.calcsize(INODE_FMT) == 160
DIR_ITEM_FMT = "<QBQQHHB"                 # location(key) + transid,data_len,name_len,type
assert struct.calcsize(DIR_ITEM_FMT) == 30
EXTENT_DATA_FMT = "<QQBBHBQQQQ"           # generation,ram_bytes,compression,encryption,other_encoding,type,disk_bytenr,disk_num_bytes,offset,num_bytes
assert struct.calcsize(EXTENT_DATA_FMT) == 53
CHUNK_FIXED_FMT = "<QQQQIIIHH"            # length,owner,stripe_len,type,io_align,io_width,sector_size,num_stripes,sub_stripes
assert struct.calcsize(CHUNK_FIXED_FMT) == 48
STRIPE_FMT = "<QQ16s"                     # devid, offset, dev_uuid
assert struct.calcsize(STRIPE_FMT) == 32
DEV_ITEM_FMT = "<QQQIIIQQQIBB16s16s"
assert struct.calcsize(DEV_ITEM_FMT) == 98
ROOT_ITEM_FMT = "<160sQQQQQQQI17sBB"      # inode,generation,root_dirid,bytenr,byte_limit,bytes_used,last_snapshot,flags,refs,drop_progress(key),drop_level,level
assert struct.calcsize(ROOT_ITEM_FMT) == 239
KEY_FMT = "<QBQ"
assert struct.calcsize(KEY_FMT) == 17


def pack_key(objectid, type_, offset):
    return struct.pack(KEY_FMT, objectid, type_, offset)


# Every inode is stamped with the build time (or SOURCE_DATE_EPOCH, for
# reproducible images), so DIR shows dates.
STAMP = struct.pack("<QI", int(os.environ.get("SOURCE_DATE_EPOCH", time.time())), 0)


def zero_inode(mode, size):
    return struct.pack(INODE_FMT,
                        1, 1, size, 0, 0,          # generation,transid,size,nbytes,block_group
                        1, 0, 0, mode,               # nlink,uid,gid,mode
                        0, 0, 0,                     # rdev,flags,sequence
                        b"\x00" * 32,
                        STAMP, STAMP, STAMP, STAMP)  # atime, ctime, mtime, otime


def build_leaf(addr, owner, items):
    """items: list of (key_tuple, data_bytes). Must already be key-sorted."""
    item_bytes = b"".join(struct.pack(ITEM_FMT, k[0], k[1], k[2], 0, len(d)) for k, d in items)
    # patch in correct offsets now that we know the item array length
    data_start = 101 + len(items) * 25
    out_items = bytearray()
    off = data_start
    for k, d in items:
        out_items += struct.pack(ITEM_FMT, k[0], k[1], k[2], off, len(d))
        off += len(d)
    data_blob = b"".join(d for _, d in items)

    header = struct.pack(HEADER_FMT,
                          b"\x00" * 32, b"\x00" * 16, addr, 0, b"\x00" * 16,
                          1, owner, len(items), 0)
    node = bytearray(header) + out_items + data_blob
    if len(node) > NODESIZE:
        raise SystemExit(f"leaf at 0x{addr:x} overflowed nodesize: {len(node)} > {NODESIZE}")
    node += b"\x00" * (NODESIZE - len(node))
    return bytes(node)


def build_chunk_item(logical, length, phys):
    stripe = struct.pack(STRIPE_FMT, 1, phys, b"\x00" * 16)
    fixed = struct.pack(CHUNK_FIXED_FMT, length, 2, 65536, 7, 4096, 4096, 4096, 1, 0)
    return fixed + stripe


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    out_path = sys.argv[1]
    total_size = int(sys.argv[2])
    files = []
    for spec in sys.argv[3:]:
        name, host_path = spec.split(":", 1)
        with open(host_path, "rb") as f:
            files.append((name, f.read()))

    image = bytearray(total_size)

    # ── Place file data, tracking each file's (ino, disk_addr, size) ──────
    entries = []  # (ino, name, data_addr, size)
    addr = DATA_START
    ino = 257
    for name, data in files:
        if addr + len(data) > total_size:
            raise SystemExit(f"btrfs image too small for {name} ({len(data)} bytes at 0x{addr:x})")
        image[addr:addr + len(data)] = data
        entries.append((ino, name, addr, len(data)))
        addr += len(data)
        addr = (addr + NODESIZE - 1) & ~(NODESIZE - 1)  # keep extents node-aligned
        ino += 1

    # ── FS tree leaf: root dir inode, DIR_INDEX per file, INODE_ITEM +
    #    EXTENT_DATA per file. ──────────────────────────────────────────
    fs_items = [((BTRFS_FIRST_FREE_OBJECTID, INODE_ITEM_KEY, 0), zero_inode(S_IFDIR | 0o755, 0))]

    dir_index = 2
    for ino, name, data_addr, size in entries:
        nb = name.encode("ascii")
        dir_item = struct.pack(DIR_ITEM_FMT, ino, INODE_ITEM_KEY, 0, 1, 0, len(nb), FT_REG_FILE) + nb
        fs_items.append(((BTRFS_FIRST_FREE_OBJECTID, DIR_INDEX_KEY, dir_index), dir_item))
        dir_index += 1

    for ino, name, data_addr, size in entries:
        fs_items.append(((ino, INODE_ITEM_KEY, 0), zero_inode(S_IFREG | 0o644, size)))
        extent = struct.pack(EXTENT_DATA_FMT, 1, size, 0, 0, 0, 1, data_addr, size, 0, size)
        fs_items.append(((ino, EXTENT_DATA_KEY, 0), extent))

    fs_items.sort(key=lambda kv: kv[0])
    fs_leaf = build_leaf(FS_LEAF_ADDR, BTRFS_FS_TREE_OBJECTID, fs_items)
    image[FS_LEAF_ADDR:FS_LEAF_ADDR + NODESIZE] = fs_leaf

    # ── Root tree leaf: ROOT_ITEM for the default (fs) subvolume ───────────
    root_item = struct.pack(ROOT_ITEM_FMT,
                             b"\x00" * 160, 1, BTRFS_FIRST_FREE_OBJECTID, FS_LEAF_ADDR,
                             0, 0, 0, 0, 1,
                             pack_key(0, 0, 0), 0, 0)
    root_items = [((BTRFS_FS_TREE_OBJECTID, ROOT_ITEM_KEY, 0), root_item)]
    root_leaf = build_leaf(ROOT_LEAF_ADDR, BTRFS_ROOT_TREE_OBJECTID, root_items)
    image[ROOT_LEAF_ADDR:ROOT_LEAF_ADDR + NODESIZE] = root_leaf

    # ── Chunk tree leaf: single identity chunk covering the whole image ────
    chunk_data = build_chunk_item(0, total_size, 0)
    chunk_items = [((BTRFS_FIRST_CHUNK_TREE_OBJECTID, CHUNK_ITEM_KEY, 0), chunk_data)]
    chunk_leaf = build_leaf(CHUNK_LEAF_ADDR, BTRFS_CHUNK_TREE_OBJECTID, chunk_items)
    image[CHUNK_LEAF_ADDR:CHUNK_LEAF_ADDR + NODESIZE] = chunk_leaf

    # ── Superblock ──────────────────────────────────────────────────────────
    sys_chunk_array = pack_key(BTRFS_FIRST_CHUNK_TREE_OBJECTID, CHUNK_ITEM_KEY, 0) + chunk_data
    dev_item = struct.pack(DEV_ITEM_FMT, 1, total_size, total_size, 4096, 4096, 4096,
                            0, 1, 0, 0, 0, 0, b"\x00" * 16, b"\x00" * 16)

    sb = bytearray(4096)
    off = 0
    def put(fmt, *vals):
        nonlocal off
        packed = struct.pack(fmt, *vals)
        sb[off:off + len(packed)] = packed
        off += len(packed)

    put("<32s16s", b"\x00" * 32, b"\x00" * 16)              # csum, fsid
    put("<QQ", SUPER_OFFSET, 0)                               # bytenr, flags
    put("<8s", b"_BHRfS_M")                                    # magic
    put("<Q", 1)                                                # generation
    put("<QQQQ", ROOT_LEAF_ADDR, CHUNK_LEAF_ADDR, 0, 0)        # root,chunk_root,log_root,reserved0
    put("<QQQQ", total_size, sum(e[3] for e in entries), 0, 1)  # total_bytes,bytes_used,root_dir_objectid,num_devices
    put("<IIII", SECTORSIZE, NODESIZE, NODESIZE, 4096)          # sectorsize,nodesize,reserved1,stripesize
    put("<I", len(sys_chunk_array))                             # sys_chunk_array_size
    put("<Q", 1)                                                # chunk_root_generation
    put("<QQQ", 0, 0, 0)                                        # compat_flags,compat_ro_flags,incompat_flags
    put("<H", 0)                                                # csum_type
    put("<BBB", 0, 0, 0)                                        # root_level,chunk_root_level,log_root_level
    sb[off:off + 98] = dev_item; off += 98
    label = b"C"
    sb[off:off + 256] = label + b"\x00" * (256 - len(label)); off += 256
    put("<QQ", 0, 0)                                            # cache_generation, uuid_tree_generation
    sb[off:off + 16] = b"\x00" * 16; off += 16                  # metadata_uuid
    off += 224                                                  # reserved2
    sca_off = off
    sb[sca_off:sca_off + len(sys_chunk_array)] = sys_chunk_array
    # (sys_chunk_array field is 2048 bytes; rest stays zero and is never read
    # since parse_system_chunk_array() stops at sys_chunk_array_size)

    image[SUPER_OFFSET:SUPER_OFFSET + 4096] = bytes(sb)

    with open(out_path, "wb") as f:
        f.write(image)

    print(f"Wrote {out_path}: {total_size} bytes, {len(entries)} file(s):")
    for ino, name, data_addr, size in entries:
        print(f"  ino {ino}: {name} ({size} bytes @ 0x{data_addr:x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
