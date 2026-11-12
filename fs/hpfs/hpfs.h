/* fs/hpfs/hpfs.h - HPFS (OS/2 High Performance File System), read-only */
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
} hpfs_mount_t;

extern vfs_fs_t vfs_hpfs_fs;

/* 1 if dev holds an HPFS volume (superblock signature at LSN 16). */
int hpfs_probe(blkdev_t *dev);

/* IFS init (IFS=HPFS.IFS): mount every HPFS volume found on the block
 * devices at /HPFS, /HPFS2, ... Not fatal if there is none. */
int hpfs_init(const char *params);

#endif /* FS_HPFS_H */
