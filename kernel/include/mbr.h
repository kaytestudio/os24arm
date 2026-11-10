#ifndef MBR_H
#define MBR_H

#include "types.h"
#include "blkdev.h"

/**
 * @brief Probe a disk for a classic MBR (DOS) partition table and register
 * each primary partition as a child block device named "<disk>p<N>"
 * (e.g. "mmc0p1"), N = 1..4 as in the table. No-op if LBA 0 has no
 * 0x55AA signature, or if it's a GPT protective MBR (type 0xEE) - call
 * gpt_probe() for those. Extended partitions (types 0x05/0x0F/0x85) are
 * skipped, not walked. The "active" partition (boot flag 0x80) gets
 * BLKDEV_BOOT.
 *
 * @return number of partitions registered
 */
int mbr_probe(blkdev_t *disk);

#endif /* MBR_H */
