/* fs/hpfs/hpfs.h - HPFS (OS/2 High Performance File System) */
#ifndef FS_HPFS_H
#define FS_HPFS_H

#include "types.h"
#include "vfs.h"
#include "blkdev.h"

#define HPFS_DNODE_SIZE 2048

/* One mounted volume: vfs_mount() it with ->dev set (like fat32_mount_t). */
typedef struct {
    blkdev_t *dev;
    uint32_t  root_fnode;
    uint32_t  n_sectors;
    char      label[33];
    uint8_t   dnode[HPFS_DNODE_SIZE];   /* one-dnode cache */
    uint32_t  dnode_lsn;                /* which dnode is cached, 0 = none */
    uint32_t  bad_dnode;                /* last damage reported (once each) */
    uint8_t   upcase[128];              /* code page: upper case of 80h..FFh */
    uint16_t  codepage;                 /* its number, 0 = none (ASCII only) */
    uint32_t  n_hotfix;                 /* bad sectors remapped to spares */
    uint32_t  hotfix_from[256], hotfix_to[256];

    /* Writing (only when mounted read-write, see hpfs.c) */
    int       rw;                       /* writes allowed */
    int       marked_dirty;             /* spareblock dirty bit set by us */
    uint32_t  nbands;                   /* free-space bitmap bands (8 MB each) */
    uint32_t  bitmaps[512];             /* each band's bitmap LSN */
    uint32_t  band;                     /* band in bandbuf, ~0 = none */
    uint8_t   bandbuf[2048];            /* bit set = sector free */
    uint32_t  dband_start, dband_end;   /* directory band (dnodes) */
    uint32_t  dband_bitmap, dband_n;    /* its bitmap LSN, dnodes in it */
    uint8_t   dband_bm[2048];           /* bit set = dnode free */
    uint32_t  alloc_hint;
    uint32_t  dir_gen;                  /* bumped when any directory changes */
    uint32_t  wcount;                   /* sectors written (failure tracking) */
    int       inconsistent;             /* an operation failed mid-way */
} hpfs_mount_t;

extern vfs_fs_t vfs_hpfs_fs;

/* 1 if mounted read-write (writable device, clean volume, bitmaps OK). */
static inline int hpfs_is_rw(const hpfs_mount_t *m) { return m->rw; }

/* 1 if dev holds an HPFS volume (superblock signature at LSN 16). */
int hpfs_probe(blkdev_t *dev);

/* IFS init (IFS=HPFS.IFS): mount every HPFS volume found on the block
 * devices at /HPFS, /HPFS2, ... Not fatal if there is none. */
int hpfs_init(const char *params);

#endif /* FS_HPFS_H */
