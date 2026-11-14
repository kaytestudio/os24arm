/* virtio_net.c - the network card: QEMU's virtio-net on virtio-mmio
 * (-device virtio-net-device), version 2 (modern), polled like the
 * kernel's other virtio drivers (gui/virtio_input.c). Frames in and out,
 * whole: each buffer starts with the 12-byte virtio_net_hdr_v1. */
#include "net/net.h"
#include "string.h"
#include "kprintf.h"

#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define VIRTIO_MMIO_SLOT_BASE   0x0A000000UL
#define VIRTIO_MMIO_SLOT_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOT_COUNT  32
#define R_MAGIC     0x000
#define R_VERSION   0x004
#define R_DEVICE_ID 0x008
#define R_DEV_FEAT  0x010
#define R_DEV_FEAT_SEL 0x014
#define R_DRV_FEAT  0x020
#define R_DRV_FEAT_SEL 0x024
#define R_QUEUE_SEL 0x030
#define R_QUEUE_NUM_MAX 0x034
#define R_QUEUE_NUM 0x038
#define R_QUEUE_READY 0x044
#define R_QUEUE_NOTIFY 0x050
#define R_STATUS    0x070
#define R_DESC_LO   0x080
#define R_DESC_HI   0x084
#define R_AVAIL_LO  0x090
#define R_AVAIL_HI  0x094
#define R_USED_LO   0x0a0
#define R_USED_HI   0x0a4
#define R_CONFIG    0x100

#define S_ACK 1
#define S_DRIVER 2
#define S_DRIVER_OK 4
#define S_FEATURES_OK 8
#define F_MAC (1u << 5)                    /* word 0 */
#define F_VERSION_1 (1u << 0)              /* word 1: bit 32 */
#define DESC_F_WRITE 2

#define QN    64                           /* descriptors per queue */
#define BUFSZ 2048
#define HDR   12                           /* struct virtio_net_hdr_v1 */

#pragma pack(push, 1)
typedef struct { uint64_t addr; uint32_t len; uint16_t flags, next; } desc_t;
typedef struct { uint16_t flags, idx, ring[QN]; } avail_t;
typedef struct { uint32_t id, len; } used_elem_t;
typedef struct { uint16_t flags, idx; used_elem_t ring[QN]; } used_t;
#pragma pack(pop)

typedef struct {
    desc_t desc[QN] __attribute__((aligned(4096)));
    avail_t avail __attribute__((aligned(4096)));
    used_t used __attribute__((aligned(4096)));
    uint8_t buf[QN][BUFSZ] __attribute__((aligned(64)));
    uint16_t last_used;
    int busy[QN];                          /* tx: still with the device */
} vq_t;

static vq_t rxq, txq;
static uintptr_t base;
static int present;

static inline uint32_t rd(uint32_t o) { return *(volatile uint32_t *)(base + o); }
static inline void wr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(base + o) = v; }
static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static int queue_setup(int q, vq_t *vq)
{
    wr(R_QUEUE_SEL, (uint32_t)q);
    if (rd(R_QUEUE_NUM_MAX) < QN) return -1;
    wr(R_QUEUE_NUM, QN);
    uint64_t d = (uint64_t)(uintptr_t)vq->desc, a = (uint64_t)(uintptr_t)&vq->avail, u = (uint64_t)(uintptr_t)&vq->used;
    wr(R_DESC_LO, (uint32_t)d); wr(R_DESC_HI, (uint32_t)(d >> 32));
    wr(R_AVAIL_LO, (uint32_t)a); wr(R_AVAIL_HI, (uint32_t)(a >> 32));
    wr(R_USED_LO, (uint32_t)u); wr(R_USED_HI, (uint32_t)(u >> 32));
    wr(R_QUEUE_READY, 1);
    return 0;
}

static void rx_post(uint16_t i)
{
    rxq.desc[i].addr = (uint64_t)(uintptr_t)rxq.buf[i];
    rxq.desc[i].len = BUFSZ;
    rxq.desc[i].flags = DESC_F_WRITE;
    rxq.avail.ring[rxq.avail.idx % QN] = i;
    barrier();
    rxq.avail.idx++;
    barrier();
}

int vnet_init(uint8_t mac[6])
{
    for (int s = 0; s < VIRTIO_MMIO_SLOT_COUNT && !present; s++) {
        base = VIRTIO_MMIO_SLOT_BASE + (uintptr_t)s * VIRTIO_MMIO_SLOT_STRIDE;
        if (rd(R_MAGIC) != 0x74726976 || rd(R_VERSION) != 2 || rd(R_DEVICE_ID) != 1) continue;
        wr(R_STATUS, 0);
        wr(R_STATUS, S_ACK);
        wr(R_STATUS, S_ACK | S_DRIVER);
        wr(R_DEV_FEAT_SEL, 0);
        uint32_t f0 = rd(R_DEV_FEAT);
        wr(R_DRV_FEAT_SEL, 0);
        wr(R_DRV_FEAT, f0 & F_MAC);
        wr(R_DRV_FEAT_SEL, 1);
        wr(R_DRV_FEAT, F_VERSION_1);
        wr(R_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
        if (!(rd(R_STATUS) & S_FEATURES_OK)) { kprintf("[NET] virtio-net: features refused\n"); continue; }
        memset(&rxq, 0, sizeof rxq);
        memset(&txq, 0, sizeof txq);
        if (queue_setup(0, &rxq) || queue_setup(1, &txq)) { kprintf("[NET] virtio-net: queues too small\n"); continue; }
        for (uint16_t i = 0; i < QN; i++) rx_post(i);
        for (int i = 0; i < 6; i++)
            mac[i] = (f0 & F_MAC) ? *(volatile uint8_t *)(base + R_CONFIG + i) : (uint8_t)(i == 0 ? 0x52 : 0x40 + i);
        wr(R_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
        wr(R_QUEUE_NOTIFY, 0);
        present = 1;
    }
    return present ? 0 : -1;
}

/* the transmit buffers the device is finished with */
static void tx_reclaim(void)
{
    while (txq.last_used != *(volatile uint16_t *)&txq.used.idx) {
        uint32_t id = txq.used.ring[txq.last_used % QN].id;
        if (id < QN) txq.busy[id] = 0;
        txq.last_used++;
    }
}

int vnet_send(const void *frame, unsigned len)
{
    if (!present || len + HDR > BUFSZ) return -1;
    tx_reclaim();
    int i = -1;
    for (int k = 0; k < QN; k++) if (!txq.busy[k]) { i = k; break; }
    if (i < 0) return -1;                                  /* all in flight: drop it */
    memset(txq.buf[i], 0, HDR);
    memcpy(txq.buf[i] + HDR, frame, len);
    txq.desc[i].addr = (uint64_t)(uintptr_t)txq.buf[i];
    txq.desc[i].len = len + HDR;
    txq.desc[i].flags = 0;
    txq.busy[i] = 1;
    txq.avail.ring[txq.avail.idx % QN] = (uint16_t)i;
    barrier();
    txq.avail.idx++;
    barrier();
    wr(R_QUEUE_NOTIFY, 1);
    return 0;
}

int vnet_recv(void *buf, unsigned max)
{
    if (!present || rxq.last_used == *(volatile uint16_t *)&rxq.used.idx) return 0;
    barrier();
    used_elem_t e = rxq.used.ring[rxq.last_used % QN];
    rxq.last_used++;
    int n = 0;
    if (e.id < QN && e.len > HDR) {
        n = (int)(e.len - HDR);
        if ((unsigned)n > max) n = (int)max;
        memcpy(buf, rxq.buf[e.id] + HDR, (size_t)n);
    }
    if (e.id < QN) rx_post((uint16_t)e.id);
    wr(R_QUEUE_NOTIFY, 0);
    return n;
}
