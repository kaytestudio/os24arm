/**
 * @file virtio_gpu.c
 * @brief Minimal virtio-gpu 2D driver - see gui/virtio_gpu.h
 *
 * MMIO register offsets and split-virtqueue layout are the same ones
 * gui/virtio_input.c already validated against QEMU's virtio-mmio.c;
 * reused verbatim here. The virtio-gpu command struct layouts
 * (virtio_gpu_ctrl_hdr etc.) are cross-checked against the stable
 * include/uapi/linux/virtio_gpu.h ABI.
 *
 * Only the control queue (queue index 0) is used - no cursor queue (index
 * 1), since this driver never moves a hardware cursor. Every command is
 * sent as a single synchronous request/response pair on descriptors 0/1:
 * no interrupts, no multiple requests in flight (matching this project's
 * other MMIO drivers - see gui/virtio_input.c's file header). That's
 * safe here specifically because the caller never reuses the cmd/resp
 * buffers until gpu_ctrlq_submit() returns, which only happens after the
 * used ring shows the device is done with them.
 */

#include "gui/virtio_gpu.h"
#include "uart.h"
#include "kprintf.h"
#include "string.h"

#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

/* QEMU "virt" machine: up to 32 virtio-mmio slots, 0x200 bytes apart -
 * same layout gui/virtio_input.c scans. */
#define VIRTIO_MMIO_SLOT_BASE   0x0A000000UL
#define VIRTIO_MMIO_SLOT_STRIDE 0x200UL
#define VIRTIO_MMIO_SLOT_COUNT  32

#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW    0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH   0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW    0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH   0x0a4

#define VIRTIO_ID_GPU  16

#define VIRTIO_CONFIG_S_ACKNOWLEDGE  1
#define VIRTIO_CONFIG_S_DRIVER       2
#define VIRTIO_CONFIG_S_DRIVER_OK    4
#define VIRTIO_CONFIG_S_FEATURES_OK  8

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

/* Two descriptors are in flight (command and response); use a four-entry queue. */
#define QUEUE_SIZE 4

/* Iteration limit for synchronous control-queue requests. This is not a
 * timer, but it prevents a missing or misconfigured GPU from hanging boot. */
#define GPU_CTRLQ_TIMEOUT  100000000UL

/* virtio-gpu control queue command types (include/uapi/linux/virtio_gpu.h) */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106

#define VIRTIO_GPU_RESP_OK_NODATA        0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO  0x1101

/* Memory byte order B,G,R,X (X = ignored padding, not real alpha) -
 * matches FB_COLOR()'s 0xAARRGGBB value stored little-endian, and what
 * gui/ramfb.h already documents this buffer as. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2

#define VIRTIO_GPU_MAX_SCANOUTS  16

/* Fixed single resource: this driver only ever backs one full-screen 2D
 * scanout, never allocates a second resource. */
#define GPU_RESOURCE_ID  1               /* (and 2: the size before, swapped on a resize) */
#define VIRTIO_MMIO_CONFIG 0x100           /* the device's configuration */
#define VIRTIO_GPU_EVENT_DISPLAY 1        /* config events_read (+0), events_clear (+4) */
#define GPU_SCANOUT_ID   0

#pragma pack(push, 1)
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QUEUE_SIZE];
} vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[QUEUE_SIZE];
} vring_used_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} virtio_gpu_ctrl_hdr_t;

typedef struct {
    uint32_t x, y, width, height;
} virtio_gpu_rect_t;

typedef struct {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} virtio_gpu_display_one_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t    hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_mem_entry_t;

/* nr_entries is always 1 here - one contiguous backing buffer, so the
 * single mem_entry is inlined right after the header instead of being a
 * separate descriptor. */
typedef struct {
    virtio_gpu_ctrl_hdr_t   hdr;
    uint32_t                resource_id;
    uint32_t                nr_entries;
    virtio_gpu_mem_entry_t  entry;
} virtio_gpu_resource_attach_backing_1_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint32_t              scanout_id;
    uint32_t              resource_id;
} virtio_gpu_set_scanout_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint64_t              offset;
    uint32_t              resource_id;
    uint32_t              padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t     r;
    uint32_t              resource_id;
    uint32_t              padding;
} virtio_gpu_resource_flush_t;
#pragma pack(pop)

typedef struct {
    uintptr_t base;
    bool      ready;
    uint16_t  last_used_idx;
    vring_desc_t  desc[QUEUE_SIZE] __attribute__((aligned(16)));
    vring_avail_t avail          __attribute__((aligned(2)));
    vring_used_t  used           __attribute__((aligned(4)));
} vgpu_dev_t;

static vgpu_dev_t g_gpu;
static bool     g_available = false;
static uint32_t g_stride    = 0;
static uint32_t g_res       = GPU_RESOURCE_ID;   /* the resource on the scanout now */

static inline uint32_t mmio_read(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write(uintptr_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

/* Send one command as a synchronous request/response pair (descriptors
 * 0 and 1) and busy-wait for the device to answer - see file header for
 * why this is safe without a free-list/rotation scheme. Returns false on
 * timeout (device wedged or never actually attached). */
static bool gpu_ctrlq_submit(const void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
    g_gpu.desc[0].addr  = (uint64_t)(uintptr_t)cmd;
    g_gpu.desc[0].len   = cmd_len;
    g_gpu.desc[0].flags = VRING_DESC_F_NEXT;
    g_gpu.desc[0].next  = 1;

    g_gpu.desc[1].addr  = (uint64_t)(uintptr_t)resp;
    g_gpu.desc[1].len   = resp_len;
    g_gpu.desc[1].flags = VRING_DESC_F_WRITE;
    g_gpu.desc[1].next  = 0;

    uint16_t slot = (uint16_t)(g_gpu.avail.idx % QUEUE_SIZE);
    g_gpu.avail.ring[slot] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    g_gpu.avail.idx++;
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write(g_gpu.base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint64_t spins = 0;
    while (g_gpu.used.idx == g_gpu.last_used_idx) {
        if (++spins > GPU_CTRLQ_TIMEOUT) {
            uart_puts("[GPU] virtio-gpu: control queue request timed out\r\n");
            return false;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    g_gpu.last_used_idx++;
    return true;
}

static bool gpu_cmd_nodata(const void *cmd, uint32_t cmd_len, const char *what)
{
    virtio_gpu_ctrl_hdr_t resp;
    if (!gpu_ctrlq_submit(cmd, cmd_len, &resp, sizeof(resp))) return false;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[GPU] %s failed (resp type 0x%x)\r\n", what, resp.type);
        return false;
    }
    return true;
}

void virtio_gpu_flush(int x, int y, int w, int h)
{
    if (!g_available) return;

    virtio_gpu_transfer_to_host_2d_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.r.x         = (uint32_t)x;
    xfer.r.y         = (uint32_t)y;
    xfer.r.width     = (uint32_t)w;
    xfer.r.height    = (uint32_t)h;
    xfer.offset      = (uint64_t)y * g_stride + (uint64_t)x * 4;
    xfer.resource_id = g_res;
    if (!gpu_cmd_nodata(&xfer, sizeof(xfer), "TRANSFER_TO_HOST_2D")) return;

    virtio_gpu_resource_flush_t flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r           = xfer.r;
    flush.resource_id = g_res;
    gpu_cmd_nodata(&flush, sizeof(flush), "RESOURCE_FLUSH");
}

bool virtio_gpu_available(void)
{
    return g_available;
}

bool virtio_gpu_init(void *fb_addr, uint32_t width, uint32_t height, uint32_t stride)
{
    if (stride != width * 4) {
        uart_puts("[GPU] virtio-gpu: only tightly-packed (stride == width*4) buffers supported\r\n");
        return false;
    }
    if (!virtio_gpu_probe()) return false;
    if (!virtio_gpu_set_mode(fb_addr, width, height)) return false;
    uart_puts("[GPU] virtio-gpu 2D scanout ready\r\n");
    return true;
}

/* Finds the device and sets up its control queue (once). */
bool virtio_gpu_probe(void)
{
    if (g_gpu.ready) return true;
    memset(&g_gpu, 0, sizeof(g_gpu));

    uintptr_t base = 0;
    bool found = false;
    for (int i = 0; i < VIRTIO_MMIO_SLOT_COUNT; i++) {
        uintptr_t slot = VIRTIO_MMIO_SLOT_BASE + (uintptr_t)i * VIRTIO_MMIO_SLOT_STRIDE;
        if (mmio_read(slot, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976UL) continue; /* "virt" */
        if (mmio_read(slot, VIRTIO_MMIO_VERSION) != 2) continue;
        if (mmio_read(slot, VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_GPU) continue;
        base = slot;
        found = true;
        break;
    }
    if (!found) {
        uart_puts("[GPU] No virtio-gpu-device found "
                   "(boot with -device virtio-gpu-device; ramfb will be used instead)\r\n");
        return false;
    }
    g_gpu.base = base;

    mmio_write(base, VIRTIO_MMIO_STATUS, 0);
    mmio_write(base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmio_write(base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* Negotiate only VIRTIO_F_VERSION_1 (feature bit 32 = word 1, bit 0) -
     * in particular no VIRTIO_GPU_F_VIRGL (bit 0 of word 0) yet, and no
     * EVENT_IDX/PACKED, keeping the plain split-ring layout above valid. */
    mmio_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 1); /* bit 0 of word 1 */

    mmio_write(base, VIRTIO_MMIO_STATUS,
               VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER | VIRTIO_CONFIG_S_FEATURES_OK);
    if (!(mmio_read(base, VIRTIO_MMIO_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK)) {
        uart_puts("[GPU] virtio-gpu: feature negotiation rejected\r\n");
        return false;
    }

    mmio_write(base, VIRTIO_MMIO_QUEUE_SEL, 0); /* control queue */
    if (mmio_read(base, VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
        uart_puts("[GPU] virtio-gpu: control queue too small\r\n");
        return false;
    }
    mmio_write(base, VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t desc_addr  = (uint64_t)(uintptr_t)g_gpu.desc;
    uint64_t avail_addr = (uint64_t)(uintptr_t)&g_gpu.avail;
    uint64_t used_addr  = (uint64_t)(uintptr_t)&g_gpu.used;

    mmio_write(base, VIRTIO_MMIO_QUEUE_DESC_LOW,   (uint32_t)desc_addr);
    mmio_write(base, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (uint32_t)(desc_addr >> 32));
    mmio_write(base, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)avail_addr);
    mmio_write(base, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,(uint32_t)(avail_addr >> 32));
    mmio_write(base, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)used_addr);
    mmio_write(base, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,(uint32_t)(used_addr >> 32));
    mmio_write(base, VIRTIO_MMIO_QUEUE_READY, 1);

    mmio_write(base, VIRTIO_MMIO_STATUS,
               VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
               VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);

    g_gpu.ready = true;
    kprintf("[GPU] virtio-gpu control queue ready at %p\r\n", (void *)base);
    return true;
}

/* The host's size for the display (its window, under QEMU): 0 if it has
 * none to say. */
bool virtio_gpu_host_size(uint32_t *w, uint32_t *h)
{
    if (!g_gpu.ready) return false;
    virtio_gpu_ctrl_hdr_t get_di;
    memset(&get_di, 0, sizeof(get_di));
    get_di.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    static virtio_gpu_resp_display_info_t di;
    if (!gpu_ctrlq_submit(&get_di, sizeof(get_di), &di, sizeof(di)) ||
        di.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO || !di.pmodes[0].r.width || !di.pmodes[0].r.height)
        return false;
    *w = di.pmodes[0].r.width;
    *h = di.pmodes[0].r.height;
    return true;
}

/* Has the host's display changed size (QEMU: its window was resized)?
 * The event is taken; true with the new size. */
bool virtio_gpu_resized(uint32_t *w, uint32_t *h)
{
    if (!g_available) return false;
    if (!(mmio_read(g_gpu.base, VIRTIO_MMIO_CONFIG + 0) & VIRTIO_GPU_EVENT_DISPLAY)) return false;
    mmio_write(g_gpu.base, VIRTIO_MMIO_CONFIG + 4, VIRTIO_GPU_EVENT_DISPLAY);   /* events_clear */
    return virtio_gpu_host_size(w, h);
}

/* Bind this tightly packed framebuffer to scanout, replacing the prior resource. */
bool virtio_gpu_set_mode(void *fb_addr, uint32_t width, uint32_t height)
{
    uint32_t old = g_available ? g_res : 0, res = old == GPU_RESOURCE_ID ? GPU_RESOURCE_ID + 1 : GPU_RESOURCE_ID;
    uint32_t stride = width * 4;
    virtio_gpu_resource_create_2d_t create;
    memset(&create, 0, sizeof(create));
    create.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = res;
    create.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width       = width;
    create.height      = height;
    if (!gpu_cmd_nodata(&create, sizeof(create), "RESOURCE_CREATE_2D")) return false;

    virtio_gpu_resource_attach_backing_1_t attach;
    memset(&attach, 0, sizeof(attach));
    attach.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id  = res;
    attach.nr_entries   = 1;
    attach.entry.addr   = (uint64_t)(uintptr_t)fb_addr;
    attach.entry.length = stride * height;
    if (!gpu_cmd_nodata(&attach, sizeof(attach), "RESOURCE_ATTACH_BACKING")) return false;

    virtio_gpu_set_scanout_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
    scan.r.width       = width;
    scan.r.height      = height;
    scan.scanout_id    = GPU_SCANOUT_ID;
    scan.resource_id   = res;
    if (!gpu_cmd_nodata(&scan, sizeof(scan), "SET_SCANOUT")) return false;

    if (old) {                                          /* (the size before: gone) */
        virtio_gpu_ctrl_hdr_t unref[2];
        memset(unref, 0, sizeof unref);
        unref[0].type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
        ((uint32_t *)&unref[1])[0] = old;               /* struct { hdr; u32 resource_id; u32 pad } */
        gpu_cmd_nodata(unref, sizeof(virtio_gpu_ctrl_hdr_t) + 8, "RESOURCE_UNREF");
    }
    g_res       = res;
    g_stride    = stride;
    g_available = true;
    virtio_gpu_flush(0, 0, (int)width, (int)height);
    return true;
}

/* Before powering off: the scanout let go of (no resource on it) and the
 * resource freed - the host's display isn't left showing guest memory
 * that's about to disappear (QEMU's cocoa display crashed on exit so). */
void virtio_gpu_shutdown(void)
{
    if (!g_available) return;
    virtio_gpu_set_scanout_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    scan.scanout_id  = GPU_SCANOUT_ID;
    scan.resource_id = 0;                               /* (none: the display off) */
    gpu_cmd_nodata(&scan, sizeof(scan), "SET_SCANOUT (off)");
    virtio_gpu_ctrl_hdr_t unref[2];
    memset(unref, 0, sizeof unref);
    unref[0].type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    ((uint32_t *)&unref[1])[0] = g_res;
    gpu_cmd_nodata(unref, sizeof(virtio_gpu_ctrl_hdr_t) + 8, "RESOURCE_UNREF");
    g_available = false;
}
