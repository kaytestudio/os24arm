/**
 * @file usbmsd.c
 * @brief USBMSD - USB mass storage class driver (class layer of the
 *        OS/2-style USB stack, see usbd.h), Bulk-Only Transport + SCSI
 *
 * Accepts (CHKSERV) the first interface of class 08h (mass storage),
 * subclass 06h (SCSI transparent), protocol 50h (Bulk-Only) with a bulk
 * IN and a bulk OUT endpoint. After SETCONF completes it brings the
 * device up with synchronous BOT commands - INQUIRY, TEST UNIT READY
 * (retried, with REQUEST SENSE), READ CAPACITY(10) - and registers it as
 * block device "usb0" (kernel/include/blkdev.h), then lets gpt_probe()
 * register its GPT partitions.
 *
 * Synchronous I/O pumps the host controllers with usbd_pump() until the
 * request's completion arrives, so block reads work from anywhere,
 * including from inside a completion callback (where enumeration runs).
 * Data goes through a 16 KB bounce buffer aligned so no transfer crosses
 * a 64 KB boundary (an xHCI TRB rule).
 *
 * Reads and writes use READ(10) / WRITE(10); MODE SENSE(6) reports the
 * write-protect switch, and a protected medium stays read-only.
 *
 * Limits: one device, LUN 0, 512-byte sectors, up to 2 TB, no write
 * cache flush (SYNCHRONIZE CACHE) yet. Block devices can't be
 * unregistered, so an unplugged disk stays listed but fails every read;
 * plugging a disk in again reuses "usb0" (partitions aren't re-probed).
 */

#include "usbmsd.h"
#include "usbd.h"
#include "usb_util.h"
#include "kprintf.h"
#include "blkdev.h"
#include "gpt.h"

#define CBW_SIGNATURE   0x43425355u
#define CSW_SIGNATURE   0x53425355u
#define BOUNCE_SIZE     16384
#define SECTOR          512
#define IO_TIMEOUT_MS   2000

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY   0x25
#define SCSI_MODE_SENSE6     0x1A
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

enum { RQ_SETCONF = 1, RQ_SYNC };

/* One request at a time, completed into a static, generation-tagged
 * slot (requestData2 = generation) so a completion that arrives after
 * its request timed out is ignored rather than written anywhere. */
typedef struct {
    volatile int done;
    uint32_t     gen;
    uint8_t      status;
    uint16_t     actual;
} sync_t;
static sync_t sync;

static int      verbose, registered, active, present;
static uint16_t ctrl;
static uint8_t  addr, iface, ep_in, ep_out;
static uint16_t mps_in, mps_out;
static uint32_t tag = 1;
static uint8_t  setup_conf[8] __attribute__((aligned(8)));
static uint8_t  setup_req[8]  __attribute__((aligned(8)));
static uint8_t  cbw[32]       __attribute__((aligned(64)));
static uint8_t  csw[16]       __attribute__((aligned(64)));
static uint8_t  bounce[BOUNCE_SIZE] __attribute__((aligned(BOUNCE_SIZE)));
static blkdev_t disk;
static int      disk_registered;
static int      wp;                 /* medium write-protected */

#define V(...) do { if (verbose) kprintf(__VA_ARGS__); } while (0)

static void usbmsd_idc(RP_GENIOCTL *rp);

/* ── Synchronous requests ─────────────────────────────────────────────── */
static int wait_sync(sync_t *s)
{
    u64 end = usb_ticks() + usb_ms(IO_TIMEOUT_MS);
    while (!s->done) {
        usbd_pump();
        if (!present || usb_ticks() >= end) return -1;
    }
    return 0;
}

static int submit_sync(USBRb *rb, sync_t *s)
{
    s->done = 0;
    s->gen++;
    rb->controllerId  = ctrl;
    rb->deviceAddress = addr;
    rb->usbIDC        = usbmsd_idc;
    rb->category      = USB_IDC_CATEGORY_CLASS;
    rb->requestData1  = RQ_SYNC;
    rb->requestData2  = s->gen;
    if (usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD, USB_IDC_FUNCTION_ACCIO,
                     rb, 0) & STERR)
        return -1;
    if (wait_sync(s) != 0) {
        USBCancel c = { ctrl, addr, rb->endPointId };
        usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD, USB_IDC_FUNCTION_CANCEL, &c, 0);
        s->gen++;                                    /* orphan any late completion */
        return -1;
    }
    return 0;
}

/* Bulk transfer on `ep` (IN if in). Returns bytes moved, -2 on STALL, -1 error. */
static int bulk(int in, void *buf, uint16_t len)
{
    sync_t *s = &sync;
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    rb.endPointId    = in ? ep_in : ep_out;
    rb.flags         = (in ? USRB_FLAGS_TTYPE_IN : USRB_FLAGS_TTYPE_OUT) |
                       USRB_FLAGS_DET_BULK;
    rb.buffer1       = buf;
    rb.buffer1Length = len;
    rb.maxPacketSize = in ? mps_in : mps_out;
    if (submit_sync(&rb, s) != 0) return -1;
    if (s->status & USRB_STATUS_STALLED) return -2;
    return s->status ? -1 : s->actual;
}

static int control_nodata(uint8_t type, uint8_t req, uint16_t value, uint16_t index)
{
    sync_t *s = &sync;
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    usb_setup(setup_req, type, req, value, index, 0);
    rb.flags         = USRB_FLAGS_TTYPE_SETUP;
    rb.buffer1       = setup_req;
    rb.buffer1Length = 8;
    if (submit_sync(&rb, s) != 0) return -1;
    return s->status ? -1 : 0;
}

static void clear_halt(uint8_t ep_addr)
{
    control_nodata(0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, ep_addr);
}

/* Bulk-Only Mass Storage Reset + clear both halts (BOT 5.3.4). */
static void reset_recovery(void)
{
    control_nodata(0x21, 0xFF, 0, iface);
    clear_halt((uint8_t)(0x80 | ep_in));
    clear_halt(ep_out);
}

/* ── BOT command ──────────────────────────────────────────────────────── */
/* Run SCSI command `cb` with an optional data stage through the bounce
 * buffer (IN: device -> bounce, OUT: bounce -> device). Returns bytes
 * moved, or -1 (also on CHECK CONDITION). */
static int scsi_cmd(const uint8_t *cb, uint8_t cblen, uint32_t len, int out)
{
    if (!present || len > BOUNCE_SIZE) return -1;
    uint32_t t = tag++;
    usb_zero(cbw, 31);
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;   /* USBC */
    cbw[4] = (uint8_t)t; cbw[5] = (uint8_t)(t >> 8);
    cbw[6] = (uint8_t)(t >> 16); cbw[7] = (uint8_t)(t >> 24);
    cbw[8] = (uint8_t)len; cbw[9] = (uint8_t)(len >> 8);
    cbw[10] = (uint8_t)(len >> 16); cbw[11] = (uint8_t)(len >> 24);
    cbw[12] = (len && !out) ? 0x80 : 0x00;            /* direction */
    cbw[13] = 0;                                      /* LUN 0 */
    cbw[14] = cblen;
    usb_copy(cbw + 15, cb, cblen);

    if (bulk(0, cbw, 31) != 31) { reset_recovery(); return -1; }

    int got = 0;
    if (len) {
        got = bulk(!out, bounce, (uint16_t)len);
        if (got == -2) {                              /* device ended the stage */
            clear_halt(out ? ep_out : (uint8_t)(0x80 | ep_in));
            got = 0;
        } else if (got < 0) { reset_recovery(); return -1; }
    }

    int n = bulk(1, csw, 13);
    if (n == -2) {                                    /* stalled: retry once */
        clear_halt((uint8_t)(0x80 | ep_in));
        n = bulk(1, csw, 13);
    }
    uint32_t sig = (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) |
                   ((uint32_t)csw[2] << 16) | ((uint32_t)csw[3] << 24);
    uint32_t ctag = (uint32_t)csw[4] | ((uint32_t)csw[5] << 8) |
                    ((uint32_t)csw[6] << 16) | ((uint32_t)csw[7] << 24);
    if (n != 13 || sig != CSW_SIGNATURE || ctag != t || csw[12] == 2) {
        reset_recovery();                             /* phase error / bad CSW */
        return -1;
    }
    return csw[12] == 0 ? got : -1;                   /* 1 = command failed */
}

static int scsi_in(const uint8_t *cb, uint8_t cblen, uint32_t len)
{
    return scsi_cmd(cb, cblen, len, 0);
}

static int read10(uint32_t lba, uint16_t count)
{
    uint8_t cb[10] = { SCSI_READ10, 0,
                       (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                       (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                       (uint8_t)(count >> 8), (uint8_t)count, 0 };
    int got = scsi_in(cb, 10, (uint32_t)count * SECTOR);
    return got == (int)(count * SECTOR) ? 0 : -1;
}

static int write10(uint32_t lba, uint16_t count)
{
    uint8_t cb[10] = { SCSI_WRITE10, 0,
                       (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                       (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                       (uint8_t)(count >> 8), (uint8_t)count, 0 };
    int put = scsi_cmd(cb, 10, (uint32_t)count * SECTOR, 1);
    return put == (int)(count * SECTOR) ? 0 : -1;
}

/* ── Block device ─────────────────────────────────────────────────────── */
static int msd_read(blkdev_t *dev, int64_t sector, uint32_t count, void *buf)
{
    (void)dev;
    if (!present || sector < 0 || sector + count > disk.num_sectors) return -1;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = count > BOUNCE_SIZE / SECTOR ? BOUNCE_SIZE / SECTOR : count;
        if (read10((uint32_t)sector, (uint16_t)n) != 0) return -1;
        usb_copy(out, bounce, n * SECTOR);
        out += n * SECTOR;
        sector += n;
        count -= n;
    }
    return 0;
}

static int msd_write(blkdev_t *dev, int64_t sector, uint32_t count, const void *buf)
{
    (void)dev;
    if (!present || wp || sector < 0 || sector + count > disk.num_sectors) return -1;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = count > BOUNCE_SIZE / SECTOR ? BOUNCE_SIZE / SECTOR : count;
        usb_copy(bounce, in, n * SECTOR);
        if (write10((uint32_t)sector, (uint16_t)n) != 0) return -1;
        in += n * SECTOR;
        sector += n;
        count -= n;
    }
    return 0;
}

static void copy_trim(char *dst, const uint8_t *src, int n)
{
    int len = n;
    while (len > 0 && (src[len - 1] == ' ' || !src[len - 1])) len--;
    for (int i = 0; i < len; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '?';
    dst[len] = 0;
}

/* SETCONF done: bring the unit up and register it. */
static void bring_up(void)
{
    static const uint8_t inquiry[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    static const uint8_t tur[6]     = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    static const uint8_t sense[6]   = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    static const uint8_t cap[10]    = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (scsi_in(inquiry, 6, 36) < 36) { V("[USBMSD] INQUIRY failed\n"); return; }
    char vendor[9], product[17];
    copy_trim(vendor, bounce + 8, 8);
    copy_trim(product, bounce + 16, 16);
    if ((bounce[0] & 0x1F) != 0x00) {                 /* direct-access only */
        V("[USBMSD] %s %s: not a disk (type %u), ignored\n", vendor, product,
          bounce[0] & 0x1F);
        return;
    }

    int ready = 0;
    for (int i = 0; i < 5 && !ready; i++) {
        if (scsi_in(tur, 6, 0) == 0) ready = 1;
        else { scsi_in(sense, 6, 18); usb_delay_ms(100); }
    }
    if (!ready) { V("[USBMSD] %s %s: not ready (no medium?)\n", vendor, product); return; }

    if (scsi_in(cap, 10, 8) < 8) { V("[USBMSD] READ CAPACITY failed\n"); return; }
    uint32_t last = ((uint32_t)bounce[0] << 24) | ((uint32_t)bounce[1] << 16) |
                    ((uint32_t)bounce[2] << 8) | bounce[3];
    uint32_t bsize = ((uint32_t)bounce[4] << 24) | ((uint32_t)bounce[5] << 16) |
                     ((uint32_t)bounce[6] << 8) | bounce[7];
    if (bsize != SECTOR || last == 0xFFFFFFFFu) {
        V("[USBMSD] %s %s: %u-byte blocks / >2TB not supported\n", vendor, product, bsize);
        return;
    }

    /* Write protect: MODE SENSE(6) header byte 2 bit 7. Devices that
     * don't support the command are treated as writable. */
    static const uint8_t msense[6] = { SCSI_MODE_SENSE6, 0, 0x3F, 0, 4, 0 };
    wp = scsi_in(msense, 6, 4) >= 3 && (bounce[2] & 0x80);

    disk.num_sectors = (int64_t)last + 1;
    if (!disk_registered) {
        usb_copy(disk.name, "usb0", 5);
        disk.start_sector = 0;
        disk.sector_size  = SECTOR;
        disk.read         = msd_read;
        disk.write        = msd_write;   /* partitions copy this pointer */
        disk.driver_data  = 0;
    }
    disk.flags = BLKDEV_REMOVABLE | (wp ? BLKDEV_READONLY : 0);
    V("[USBMSD] usb0: %s %s, %u sectors (%u MB), %s\n", vendor, product,
      (unsigned)disk.num_sectors, (unsigned)(disk.num_sectors / 2048),
      wp ? "write-protected" : "read/write");
    if (!disk_registered && blkdev_register(&disk) == 0) {
        disk_registered = 1;
        gpt_probe(&disk);
        /* GPT names partitions after their labels; a USB disk made from
         * the same image as the boot disk would otherwise shadow its
         * "BOOT"/"C". Use usb0p1, usb0p2, ... instead. */
        int part = 0;
        for (int i = 0; ; i++) {
            blkdev_t *b = blkdev_get_by_index(i);
            if (!b) break;
            if (b->parent != &disk) continue;
            char *nm = b->name;
            V("[USBMSD] usb0p%d: partition '%s', %u sectors\n", part + 1, nm,
              (unsigned)b->num_sectors);
            usb_copy(nm, "usb0p", 5);
            nm[5] = (char)('1' + part);
            nm[6] = 0;
            if (++part == 9) break;
        }
    }
}

/* ── CHKSERV ──────────────────────────────────────────────────────────── */
static void chkserv(USBCDServe *s)
{
    DeviceInfo *di = s ? s->pDeviceInfo : 0;
    if (!di || active) return;

    const uint8_t *c = di->configurationData;
    uint32_t total = di->configLength;
    int in_msd = 0, found_if = -1;
    uint8_t in = 0, out = 0;
    for (uint32_t i = 0; i + 2 <= total && c[i] >= 2; i += c[i]) {
        const uint8_t *p = &c[i];
        if (i + p[0] > total) break;
        if (p[1] == 4 && p[0] >= 9) {
            if (found_if >= 0 && in && out) break;
            in_msd = p[5] == 8 && p[6] == 6 && p[7] == 0x50;
            if (in_msd) { found_if = p[2]; in = out = 0; }
        } else if (p[1] == 5 && p[0] >= 7 && in_msd && (p[3] & 3) == 2) {
            uint16_t m = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            if ((p[2] & 0x80) && !in)  { in  = p[2] & 0xF; mps_in  = m; }
            if (!(p[2] & 0x80) && !out) { out = p[2] & 0xF; mps_out = m; }
        }
    }
    if (found_if < 0 || !in || !out) return;

    ctrl   = di->ctrlID;
    addr   = di->deviceAddress;
    iface  = (uint8_t)found_if;
    ep_in  = in;
    ep_out = out;
    di->bConfigurationValue = c[5];
    di->bInterfaceNumber    = iface;
    active = present = 1;

    USBSetConf sc;
    usb_zero(&sc, sizeof sc);
    sc.controllerId       = ctrl;
    sc.deviceAddress      = addr;
    sc.configurationValue = c[5];
    sc.irqSwitchValue     = RQ_SETCONF;
    sc.category           = USB_IDC_CATEGORY_CLASS;
    sc.setConfiguration   = setup_conf;
    if (usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD, USB_IDC_FUNCTION_SETCONF,
                     &sc, 0) & STERR) {
        active = present = 0;
        di->bConfigurationValue = 0;
        return;
    }
    V("[USBMSD] mass storage at address %u: interface %u, bulk IN %u / OUT %u\n",
      addr, iface, in, out);
}

/* ── IDC entry ────────────────────────────────────────────────────────── */
static void usbmsd_idc(RP_GENIOCTL *rp)
{
    uint16_t err = 0;
    if (rp->Category != USB_IDC_CATEGORY_CLASS) {
        err = USB_IDC_WRONGCAT;
    } else {
        switch (rp->Function) {
        case USB_IDC_FUNCTION_CHKSERV:
            chkserv(rp->ParmPacket);
            break;
        case USB_IDC_FUNCTION_PRCIRQ: {
            USBRb *rb = rp->ParmPacket;
            if (rb->requestData1 == RQ_SYNC) {
                sync_t *s = &sync;
                if (rb->requestData2 != s->gen || s->done) break;   /* stale */
                s->status = rb->status;
                s->actual = (rb->flags & USRB_FLAGS_TTYPE_MASK) == USRB_FLAGS_TTYPE_SETUP
                            ? rb->buffer2Length : rb->buffer1Length;
                s->done = 1;
            } else if (rb->requestData1 == RQ_SETCONF && active &&
                       rb->deviceAddress == addr) {
                if (rb->status) active = present = 0;
                else bring_up();
            }
            break;
        }
        case USB_IDC_FUNCTION_DETDEV: {
            USBDetach *d = rp->ParmPacket;
            if (d && active && d->controllerId == ctrl && d->deviceAddress == addr) {
                active = present = 0;
                V("[USBMSD] usb0 detached (reads now fail)\n");
            }
            break;
        }
        case USB_IDC_FUNCTION_IDLE:
            break;
        default:
            err = USB_IDC_WRONGFUNC;
            break;
        }
    }
    rp->Status = err ? (uint16_t)(STATUS_DONE | STERR | err) : STATUS_DONE;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */
static void try_register(void)
{
    USBDClass c = { usbmsd_idc, 0 };
    uint8_t cat = USB_IDC_CATEGORY_CLASS;
    if (!(usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD,
                       USB_IDC_FUNCTION_REGISTER, &c, &cat) & STERR))
        registered = 1;
}

int usbmsd_basedevinit(int v)
{
    verbose = v;
    try_register();
    return 0;
}

void usbmsd_init_complete(void)
{
    if (!registered) try_register();
    if (!registered) V("[USBMSD] USBD not loaded, driver inactive\n");
}
