/* ============================================================================
 * kernel/src/blkdev.c  —  Block Device Management
 * ========================================================================== */

#include "blkdev.h"
#include "kio.h"
#include "uart.h"
#include "gpt.h"
#include "mbr.h"

/* External functions (should be in your headers) */
/* kmemcpy, kstrcmp, kstrcpy should be in kio.h */

/* Constants */
#define MAX_BLKDEVS     16
#define SECTOR_SIZE     512

/* Block device table */
static blkdev_t *blkdevs[MAX_BLKDEVS];
static int blkdev_count = 0;

int blkdev_register(blkdev_t *dev)
{
    if (blkdev_count >= MAX_BLKDEVS) return -1;
    blkdevs[blkdev_count++] = dev;
    kprintf("[BLKDEV] Registered: %s (%u sectors)\n",
            dev->name, (unsigned)dev->num_sectors);
    return 0;
}

/* Remove a device and every partition registered on it (removable media
 * gone). The structures stay allocated - a filesystem still mounted on
 * one keeps a valid pointer whose reads simply fail. */
static blkdev_remove_fn remove_hook;

void blkdev_set_remove_hook(blkdev_remove_fn fn) { remove_hook = fn; }

int blkdev_unregister(blkdev_t *dev)
{
    int n = 0, removed = 0;
    for (int i = 0; i < blkdev_count; i++) {
        blkdev_t *d = blkdevs[i];
        if (d == dev || d->parent == dev) {
            removed++;
            if (remove_hook) remove_hook(d);
            continue;
        }
        blkdevs[n++] = d;
    }
    for (int i = n; i < blkdev_count; i++) blkdevs[i] = NULL;
    blkdev_count = n;
    return removed ? 0 : -1;
}

blkdev_t *blkdev_get_by_index(int index)
{
    if (index < 0 || index >= blkdev_count) return NULL;
    return blkdevs[index];
}

blkdev_t *blkdev_get_by_name(const char *name)
{
    for (int i = 0; i < blkdev_count; i++) {
        if (kstrcmp(blkdevs[i]->name, name) == 0) {
            return blkdevs[i];
        }
    }
    return NULL;
}

void blkdev_list(void)
{
    kprintf("\n=== Block Devices ===\n");
    for (int i = 0; i < blkdev_count; i++) {
        blkdev_t *d = blkdevs[i];
        int64_t mb = ((int64_t)d->num_sectors * (int64_t)d->sector_size) / (1024 * 1024);
        kprintf("  %s: %lu MB (%u sectors x %u bytes)\n",
                d->name, (unsigned long)mb, d->num_sectors, d->sector_size);
    }
}

/* ── Ramdisk Implementation ────────────────────────────────────────────── */

typedef struct {
    uint8_t     *base;
    uint32_t    num_sectors;
} ramdisk_priv_t;

static ramdisk_priv_t ramdisk_priv;
static blkdev_t       ramdisk_dev;

static int ramdisk_read(blkdev_t *dev, int64_t lba, uint32_t count, void *buf)
{
    ramdisk_priv_t *p = (ramdisk_priv_t *)dev->driver_data;
    if (!p || !p->base) return -1;
    if (lba + count > p->num_sectors) return -1;

    kmemcpy(buf, p->base + lba * SECTOR_SIZE, count * SECTOR_SIZE);
    return 0;
}

static int ramdisk_write(blkdev_t *dev, int64_t lba, uint32_t count, const void *buf)
{
    ramdisk_priv_t *p = (ramdisk_priv_t *)dev->driver_data;
    if (!p || !p->base) return -1;
    if (lba + count > p->num_sectors) return -1;

    kmemcpy(p->base + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE);
    return 0;
}

void ramdisk_create(const char *name, void *base, size_t size)
{
    ramdisk_priv.base        = (uint8_t *)base;
    ramdisk_priv.num_sectors = size / SECTOR_SIZE;

    kstrcpy(ramdisk_dev.name, name);
    ramdisk_dev.num_sectors  = ramdisk_priv.num_sectors;
    ramdisk_dev.sector_size  = SECTOR_SIZE;
    ramdisk_dev.driver_data  = &ramdisk_priv;
    ramdisk_dev.read         = ramdisk_read;
    ramdisk_dev.write        = ramdisk_write;
    ramdisk_dev.flags        = 0;
    ramdisk_dev.parent       = NULL;
    ramdisk_dev.start_sector = 0;

    blkdev_register(&ramdisk_dev);
    kprintf("[RAMDISK] Created: %s (%u KB)\n", name, (unsigned)(size / 1024));
}

/* ── VirtIO Block Device: QEMU's virtio-blk on virtio-mmio ─────────────── */
/* (-drive if=none,file=disk.img,format=raw,id=c -device virtio-blk-device,drive=c)
 * version 2 (modern), polled like the kernel's other virtio drivers
 * (net/virtio_net.c): each request a chain of three - the header (type,
 * sector), the data (through a bounce buffer, 64 KB at a time), the status
 * the device writes. Up to VB_DISKS of them: disk0 (the first -device on
 * QEMU's command line: the GPT and MBR probes and C:), then disk1, ... (an
 * MBR's partitions probed; MOUNT takes them, or the whole disk); the RAM
 * window below is only disk0's fallback. */
#ifndef uint64_t
typedef unsigned long long vb_u64;
#else
typedef uint64_t vb_u64;
#endif
#define VB_SLOT_BASE   0x0A000000UL
#define VB_SLOT_STRIDE 0x200UL
#define VB_SLOTS       32
#define VB_QN          16
#define VB_CHUNK       128                      /* sectors a request: 64 KB */
#define VB_DISKS       4
#pragma pack(push, 1)
typedef struct { vb_u64 addr; uint32_t len; uint16_t flags, next; } vb_desc_t;
typedef struct { uint16_t flags, idx, ring[VB_QN]; } vb_avail_t;
typedef struct { uint32_t id, len; } vb_used_elem_t;
typedef struct { uint16_t flags, idx; vb_used_elem_t ring[VB_QN]; } vb_used_t;
typedef struct { uint32_t type, reserved; vb_u64 sector; } vb_req_t;
#pragma pack(pop)
typedef struct {
    vb_desc_t desc[VB_QN] __attribute__((aligned(4096)));
    vb_avail_t avail __attribute__((aligned(4096)));
    vb_used_t used __attribute__((aligned(4096)));
    vb_req_t req __attribute__((aligned(64)));
    volatile uint8_t status __attribute__((aligned(64)));
    uint8_t bounce[VB_CHUNK * 512] __attribute__((aligned(4096)));
    uint16_t last_used;
    uintptr_t base;
} vb_t;
static vb_t vbs[VB_DISKS];
static blkdev_t vb_devs[VB_DISKS];
static int vb_count;
static inline uint32_t vb_rd(vb_t *vb, uint32_t o) { return *(volatile uint32_t *)(vb->base + o); }
static inline void vb_wr(vb_t *vb, uint32_t o, uint32_t v) { *(volatile uint32_t *)(vb->base + o) = v; }
static inline void vb_barrier(void)
{
#if defined(__riscv)
    __asm__ volatile("fence iorw, iorw" ::: "memory");
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

/* one request: type 0 IN (read), 1 OUT (write); n sectors through the bounce buffer */
static int vb_request(vb_t *vbp, uint32_t type, int64_t lba, uint32_t n)
{
#define vb (*vbp)
    vb.req.type = type; vb.req.reserved = 0; vb.req.sector = (vb_u64)lba;
    vb.status = 0xFF;
    vb.desc[0] = (vb_desc_t){ (vb_u64)(uintptr_t)&vb.req, sizeof vb.req, 1, 1 };                         /* NEXT */
    vb.desc[1] = (vb_desc_t){ (vb_u64)(uintptr_t)vb.bounce, n * 512, (uint16_t)(1 | (type == 0 ? 2 : 0)), 2 };   /* NEXT, WRITE (the device's) for a read */
    vb.desc[2] = (vb_desc_t){ (vb_u64)(uintptr_t)&vb.status, 1, 2, 0 };                                  /* WRITE */
    vb.avail.ring[vb.avail.idx % VB_QN] = 0;
    vb_barrier();
    vb.avail.idx++;
    vb_barrier();
    vb_wr(vbp, 0x050, 0);                                                /* QUEUE_NOTIFY */
    for (unsigned long spin = 0; vb.last_used == *(volatile uint16_t *)&vb.used.idx; spin++)
        if (spin > 4000000000UL) { kprintf("[VIRTIO-BLK] no answer\n"); return -1; }
    vb_barrier();
    vb.last_used++;
    return vb.status == 0 ? 0 : -1;
#undef vb
}
static int virtio_blk_read(blkdev_t *dev, int64_t lba, uint32_t count, void *buf)
{
    if (lba < 0 || (vb_u64)lba + count > (vb_u64)dev->num_sectors) return -1;
    vb_t *vb = dev->driver_data;
    uint8_t *b = buf;
    while (count) {
        uint32_t n = count < VB_CHUNK ? count : VB_CHUNK;
        if (vb_request(vb, 0, lba, n)) return -1;
        kmemcpy(b, vb->bounce, n * 512);
        b += n * 512; lba += n; count -= n;
    }
    return 0;
}
static int virtio_blk_write(blkdev_t *dev, int64_t lba, uint32_t count, const void *buf)
{
    if (dev->flags & BLKDEV_READONLY) return -1;
    if (lba < 0 || (vb_u64)lba + count > (vb_u64)dev->num_sectors) return -1;
    vb_t *vb = dev->driver_data;
    const uint8_t *b = buf;
    while (count) {
        uint32_t n = count < VB_CHUNK ? count : VB_CHUNK;
        kmemcpy(vb->bounce, b, n * 512);
        if (vb_request(vb, 1, lba, n)) return -1;
        b += n * 512; lba += n; count -= n;
    }
    return 0;
}
/* Every virtio-blk device (QEMU puts the first -device in the highest slot: from there down), set up and
 * registered as disk0, disk1, ...; how many */
static int virtio_blk_probe(void)
{
    for (int sl = VB_SLOTS - 1; sl >= 0 && vb_count < VB_DISKS; sl--) {
        vb_t *v = &vbs[vb_count];
        v->base = VB_SLOT_BASE + (uintptr_t)sl * VB_SLOT_STRIDE;
        if (vb_rd(v, 0x000) != 0x74726976 || vb_rd(v, 0x004) != 2 || vb_rd(v, 0x008) != 2) continue;   /* magic, version 2, a block device */
        vb_wr(v, 0x070, 0);
        vb_wr(v, 0x070, 1);                                              /* ACKNOWLEDGE */
        vb_wr(v, 0x070, 1 | 2);                                          /* DRIVER */
        vb_wr(v, 0x014, 0);
        uint32_t f0 = vb_rd(v, 0x010);
        vb_wr(v, 0x024, 0); vb_wr(v, 0x020, f0 & (1u << 5));            /* VIRTIO_BLK_F_RO, if it says so */
        vb_wr(v, 0x024, 1); vb_wr(v, 0x020, 1);                          /* VIRTIO_F_VERSION_1 */
        vb_wr(v, 0x070, 1 | 2 | 8);                                      /* FEATURES_OK */
        if (!(vb_rd(v, 0x070) & 8)) { kprintf("[VIRTIO-BLK] features refused\n"); continue; }
        vb_wr(v, 0x030, 0);                                              /* queue 0 */
        if (vb_rd(v, 0x034) < VB_QN) { kprintf("[VIRTIO-BLK] queue too small\n"); continue; }
        kmemset(&v->desc, 0, sizeof v->desc); kmemset(&v->avail, 0, sizeof v->avail); kmemset(&v->used, 0, sizeof v->used);
        v->last_used = 0;
        vb_wr(v, 0x038, VB_QN);
        vb_u64 d = (vb_u64)(uintptr_t)v->desc, a = (vb_u64)(uintptr_t)&v->avail, u = (vb_u64)(uintptr_t)&v->used;
        vb_wr(v, 0x080, (uint32_t)d); vb_wr(v, 0x084, (uint32_t)(d >> 32));
        vb_wr(v, 0x090, (uint32_t)a); vb_wr(v, 0x094, (uint32_t)(a >> 32));
        vb_wr(v, 0x0a0, (uint32_t)u); vb_wr(v, 0x0a4, (uint32_t)(u >> 32));
        vb_wr(v, 0x044, 1);                                              /* QUEUE_READY */
        vb_wr(v, 0x070, 1 | 2 | 8 | 4);                                  /* DRIVER_OK */
        vb_u64 cap = (vb_u64)vb_rd(v, 0x100) | (vb_u64)vb_rd(v, 0x104) << 32;   /* (config: capacity, in 512-byte sectors) */
        blkdev_t *bd = &vb_devs[vb_count];
        kstrcpy(bd->name, "disk0");
        bd->name[4] = (char)('0' + vb_count);
        bd->num_sectors = (int64_t)cap;
        bd->sector_size = SECTOR_SIZE;
        bd->driver_data = v;
        bd->read = virtio_blk_read;
        bd->write = virtio_blk_write;
        bd->flags = (f0 & (1u << 5)) ? BLKDEV_READONLY : 0;
        bd->parent = NULL;
        bd->start_sector = 0;
        blkdev_register(bd);
        kprintf("[VIRTIO-BLK] %s: %u MB%s\n", bd->name, (unsigned)(cap / 2048), (f0 & (1u << 5)) ? ", read-only" : "");
        vb_count++;
    }
    return vb_count;
}
void virtio_blk_init(void) { }                          /* (blk_init probes: disk0) */

/* ── Block Device Subsystem Init ───────────────────────────────────────── */

/* disk0: the first virtio-blk disk (above); without one, a block device
 * backed by a fixed physical RAM address: a disk image can be injected
 * there before boot, e.g.
 *   qemu-system-aarch64 ... -device loader,file=test.img,addr=0x44000000
 * With nothing loaded there, RAM reads as zero and filesystem mounts
 * correctly fail (bad superblock magic) rather than misbehaving. */
#define PRELOADED_DISK_ADDR  0x44000000UL
#define PRELOADED_DISK_SIZE  (24u * 1024u * 1024u)

void blk_init(void)
{

    // Arrays are already zeroed by boot.S BSS clearing
    blkdev_count = 0;

    /* a real disk (virtio-blk) when QEMU has one; else the RAM window */
    if (!virtio_blk_probe())
        ramdisk_create("disk0", (void *)PRELOADED_DISK_ADDR, PRELOADED_DISK_SIZE);

    /* If a GPT-partitioned image (BOOT + C:, or anything else) was loaded
     * into disk0, register its partitions as child devices. No-op if disk0
     * doesn't contain a valid GPT (e.g. a flat, unpartitioned test image),
     * leaving disk0 itself directly usable as before. */
    gpt_probe(blkdev_get_by_name("disk0"));
    /* Or an MBR-partitioned image (e.g. an OS/2 disk with HPFS). A GPT
     * disk's protective MBR is ignored by mbr_probe(). */
    mbr_probe(blkdev_get_by_name("disk0"));
    /* the others: their MBR partitions (disk1p1, ...; a GPT's are named by their labels - BOOT, C - so disk0's only) */
    for (int i = 1; i < vb_count; i++) mbr_probe(&vb_devs[i]);

}
void blkdev_init(void)
{
    kprintf("[BLKDEV] Initializing block device subsystem...\n");

    /* Initialize VirtIO block devices */
    virtio_blk_init();

    /* Create a ramdisk if needed (requires mem_alloc) */
    /* Uncomment this if you have mem_alloc() available:
     *
     * void *ramdisk_mem = mem_alloc(1024 * 1024);
     * if (ramdisk_mem) {
     *     ramdisk_create("ramdisk0", ramdisk_mem, 1024 * 1024);
     * }
     */

    /* List all registered devices */
    if (blkdev_count > 0) {
        blkdev_list();
    } else {
        kprintf("[BLKDEV] No block devices registered yet\n");
    }

    kprintf("[BLKDEV] Initialization complete (%d devices)\n", blkdev_count);
}