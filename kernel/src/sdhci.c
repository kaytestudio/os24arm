/**
 * @file sdhci.c
 * @brief SD card driver over an SDHCI-compatible host (BBB, BeagleV-Ahead)
 *
 * Both Beagle SoCs use a host controller with the SD Host Controller
 * Standard register layout:
 *
 *   - TI AM335x MMCHS0 @ 0x48060000 (BeagleBone Black microSD): the
 *     standard registers start at +0x200, with TI-only registers below
 *     (SYSCONFIG, CON). It must be accessed 32 bits at a time, uses its
 *     own 10-bit clock divider (SYSCTL.CLKD) off a 96 MHz functional
 *     clock, needs an "init stream" after power-up, and returns 136-bit
 *     responses with the CRC byte still in place.
 *   - T-Head TH1520 SDIO0 @ 0xFFE7090000 (BeagleV-Ahead microSD): a
 *     Synopsys DesignWare MSHC, i.e. a standard SDHCI v4 host clocked at
 *     198 MHz.
 *
 * The driver uses 32-bit accesses and PIO throughout (no interrupts, no
 * DMA), identifies the card at 400 kHz, then runs it 4-bit at ~25 MHz
 * (default speed). U-Boot already brought up pinmux, clocks and card
 * power before handing over. On the TH1520 the controller is therefore
 * only CMD/DAT-reset, not fully reset, so U-Boot's vendor PHY settings
 * and I/O voltage survive. The card itself is always re-identified from
 * CMD0.
 */

#include "sdhci.h"
#include "string.h"
#include "kprintf.h"

#ifndef SD_BASE
#error "SD_BASE must be defined by the platform (see CMakeLists.txt)"
#endif
#ifndef SD_BASE_CLK_HZ
#error "SD_BASE_CLK_HZ must be defined by the platform (see CMakeLists.txt)"
#endif

/* ── Registers ───────────────────────────────────────────────────────────── */

#ifdef SD_HOST_AM335X_MMCHS
#define SDHCI_OFFSET        0x200u
#define MMCHS_SYSCONFIG     0x110u
#define MMCHS_SYSSTATUS     0x114u
#define MMCHS_CON           0x12Cu
#define CM_PER_MMC0_CLKCTRL 0x44E0003CUL
#else
#define SDHCI_OFFSET        0x000u
#endif

#define REG_BLK       0x04u   /* block size [11:0], block count [31:16]      */
#define REG_ARG       0x08u
#define REG_CMD       0x0Cu   /* transfer mode [15:0], command [31:16]       */
#define REG_RSP0      0x10u
#define REG_DATA      0x20u
#define REG_PSTATE    0x24u
#define REG_HCTL      0x28u   /* host ctl, power ctl, block gap, wakeup      */
#define REG_SYSCTL    0x2Cu   /* clock ctl, timeout ctl, software reset      */
#define REG_STAT      0x30u   /* normal [15:0] + error [31:16] int status    */
#define REG_IE        0x34u
#define REG_ISE       0x38u
#define REG_CAPA      0x40u
#define REG_VERSION   0xFCu   /* spec version in [23:16]                     */

/* REG_CMD */
#define CMD_DDIR_READ   (1u << 4)
#define CMD_RSP_NONE    (0u << 16)
#define CMD_RSP_136     (1u << 16)
#define CMD_RSP_48      (2u << 16)
#define CMD_RSP_48B     (3u << 16)
#define CMD_CRC_CHECK   (1u << 19)
#define CMD_IDX_CHECK   (1u << 20)
#define CMD_DATA        (1u << 21)
#define CMD_INDEX(n)    ((uint32_t)(n) << 24)

/* Response shapes */
#define R_NONE   CMD_RSP_NONE
#define R1       (CMD_RSP_48  | CMD_CRC_CHECK | CMD_IDX_CHECK)
#define R1B      (CMD_RSP_48B | CMD_CRC_CHECK | CMD_IDX_CHECK)
#define R2       (CMD_RSP_136 | CMD_CRC_CHECK)
#define R3       (CMD_RSP_48)
#define R6       R1
#define R7       R1

/* REG_PSTATE */
#define PSTATE_CMDI   (1u << 0)
#define PSTATE_DATI   (1u << 1)

/* REG_HCTL */
#define HCTL_DTW_4BIT (1u << 1)
#define HCTL_DTW_8BIT (1u << 5)
#define HCTL_SDBP     (1u << 8)       /* bus power on                  */
#define HCTL_SDVS_MASK (7u << 9)
#define HCTL_SDVS_3V3 (7u << 9)
#define HCTL_SDVS_3V0 (6u << 9)

/* REG_SYSCTL */
#define SYSCTL_ICE    (1u << 0)       /* internal clock enable         */
#define SYSCTL_ICS    (1u << 1)       /* internal clock stable         */
#define SYSCTL_CEN    (1u << 2)       /* SD clock to card enable       */
#define SYSCTL_DIV_MASK (0x3FFu << 6)
#define SYSCTL_DTO_MAX (0xEu << 16)
#define SYSCTL_SRA    (1u << 24)
#define SYSCTL_SRC    (1u << 25)
#define SYSCTL_SRD    (1u << 26)

/* REG_STAT / REG_IE */
#define STAT_CC       (1u << 0)
#define STAT_TC       (1u << 1)
#define STAT_BWR      (1u << 4)
#define STAT_BRR      (1u << 5)
#define STAT_ERRI     (1u << 15)
#define STAT_ERR_MASK 0x03FF0000u     /* CTO..ADMA error bits          */
#define IE_ALL        (STAT_CC | STAT_TC | STAT_BWR | STAT_BRR | STAT_ERR_MASK)

#define SECTOR_BYTES  512u
#define SD_IDENT_HZ   400000u
#define SD_XFER_HZ    25000000u
#define POLL_LIMIT    5000000u        /* register polls before giving up */

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)((uintptr_t)SD_BASE + SDHCI_OFFSET + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)((uintptr_t)SD_BASE + SDHCI_OFFSET + off) = v;
}

/* Crude delay with no timer yet: each iteration is a device-register read
 * (hundreds of ns on both SoCs), so this is roughly n microseconds or
 * more - only ever used where "at least this long" is what matters. */
static void delay_polls(uint32_t n)
{
    while (n--) (void)rd(REG_PSTATE);
}

/* ── Card state ──────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t rca;           /* relative card address (CMD3)              */
    int      high_capacity; /* SDHC/SDXC: block (not byte) addressing    */
    int64_t  sectors;
} sd_card_t;

static sd_card_t g_card;
static blkdev_t  g_sd_dev;

/* ── Host controller ─────────────────────────────────────────────────────── */

static int wait_clear(uint32_t off, uint32_t mask)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++)
        if (!(rd(off) & mask)) return 0;
    return -1;
}

static int wait_set(uint32_t off, uint32_t mask)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++)
        if (rd(off) & mask) return 0;
    return -1;
}

static void reset_lines(uint32_t which)
{
    wr(REG_SYSCTL, rd(REG_SYSCTL) | which);
    (void)wait_clear(REG_SYSCTL, which);
}

/* Program the SD clock to the fastest rate not above hz. Divisors are
 * found by multiplying rather than dividing: Cortex-A8 has no hardware
 * divide. */
static int set_clock(uint32_t hz)
{
    uint32_t v = rd(REG_SYSCTL) & ~(SYSCTL_CEN | SYSCTL_DIV_MASK);
    wr(REG_SYSCTL, v);

#ifdef SD_HOST_AM335X_MMCHS
    /* MMCHS: SD clock = 96 MHz / CLKD, CLKD a plain 10-bit divider. */
    uint32_t d = 1;
    while (d < 1023 && (uint32_t)SD_BASE_CLK_HZ > hz * d) d++;
    v |= d << 6;
#else
    uint32_t spec = (rd(REG_VERSION) >> 16) & 0xFFu;
    if (spec >= 2) {
        /* SDHCI v3+: 10-bit divided clock mode, SD clock = base / (2N). */
        uint32_t n = 0;
        if ((uint32_t)SD_BASE_CLK_HZ > hz) {
            n = 1;
            while (n < 1023 && (uint32_t)SD_BASE_CLK_HZ > 2u * n * hz) n++;
        }
        v |= ((n & 0xFFu) << 8) | (((n >> 8) & 0x3u) << 6);
    } else {
        /* SDHCI v1/v2: 8-bit field, power-of-two divisor (field = div/2). */
        uint32_t div = 1;
        while (div < 256 && (uint32_t)SD_BASE_CLK_HZ > hz * div) div <<= 1;
        v |= (div >> 1) << 8;
    }
#endif

    v |= SYSCTL_ICE | SYSCTL_DTO_MAX;
    wr(REG_SYSCTL, v);
    if (wait_set(REG_SYSCTL, SYSCTL_ICS) != 0) return -1;
    wr(REG_SYSCTL, rd(REG_SYSCTL) | SYSCTL_CEN);
    return 0;
}

static int host_init(void)
{
    uint32_t hctl;

#ifdef SD_HOST_AM335X_MMCHS
    /* Module clock (U-Boot normally has it on already): MODULEMODE=ENABLE,
     * then wait for IDLEST=functional. */
    *(volatile uint32_t *)CM_PER_MMC0_CLKCTRL = 2;
    for (uint32_t i = 0; i < POLL_LIMIT; i++)
        if (!(*(volatile uint32_t *)CM_PER_MMC0_CLKCTRL & (3u << 16))) break;

    /* Full module reset, then advertise 3.0 V + 1.8 V support (the CAPA
     * voltage bits are writable on MMCHS) and power the bus at 3.0 V -
     * the same setup U-Boot's omap_hsmmc uses for MMC0. */
    *(volatile uint32_t *)((uintptr_t)SD_BASE + MMCHS_SYSCONFIG) |= 1u << 1;
    for (uint32_t i = 0; i < POLL_LIMIT; i++)
        if (*(volatile uint32_t *)((uintptr_t)SD_BASE + MMCHS_SYSSTATUS) & 1u) break;
    reset_lines(SYSCTL_SRA);

    wr(REG_CAPA, rd(REG_CAPA) | (1u << 25) | (1u << 26));
    hctl = (rd(REG_HCTL) & ~(HCTL_SDVS_MASK | HCTL_DTW_4BIT | HCTL_DTW_8BIT)) | HCTL_SDVS_3V0;
    wr(REG_HCTL, hctl);
    wr(REG_HCTL, hctl | HCTL_SDBP);
    if (wait_set(REG_HCTL, HCTL_SDBP) != 0) {
        kprintf("[SD]     MMCHS: bus power did not come up\n");
        return -1;
    }
#else
    /* Keep U-Boot's vendor/PHY and I/O-voltage setup: reset only the CMD
     * and DAT state machines, drop back to a 1-bit bus, and make sure the
     * card has 3.3 V power. */
    reset_lines(SYSCTL_SRC | SYSCTL_SRD);
    hctl = rd(REG_HCTL) & ~(HCTL_DTW_4BIT | HCTL_DTW_8BIT);
    if (!(hctl & HCTL_SDBP))
        hctl = (hctl & ~HCTL_SDVS_MASK) | HCTL_SDVS_3V3 | HCTL_SDBP;
    wr(REG_HCTL, hctl);
#endif

    /* Status bits only latch when enabled in IE; nothing is routed to an
     * interrupt line (ISE = 0) - everything below is polled. */
    wr(REG_IE, IE_ALL);
    wr(REG_ISE, 0);
    wr(REG_STAT, 0xFFFFFFFFu);

    if (set_clock(SD_IDENT_HZ) != 0) {
        kprintf("[SD]     Internal clock did not stabilise\n");
        return -1;
    }

#ifdef SD_HOST_AM335X_MMCHS
    /* MMCHS initialisation stream (80 clocks with CMD high) before the
     * first command, per the AM335x TRM: CON.INIT + a dummy CMD write. */
    *(volatile uint32_t *)((uintptr_t)SD_BASE + MMCHS_CON) |= 1u << 1;
    wr(REG_CMD, 0);
    (void)wait_set(REG_STAT, STAT_CC);
    delay_polls(1000);
    wr(REG_STAT, 0xFFFFFFFFu);
    *(volatile uint32_t *)((uintptr_t)SD_BASE + MMCHS_CON) &= ~(1u << 1);
#else
    delay_polls(1000);  /* >= 74 SD clocks before the first command */
#endif
    return 0;
}

/* ── Commands ────────────────────────────────────────────────────────────── */

/* Issue one command and wait for its response (and, for R1b, for the
 * card to release DAT0). Data phases are handled by the caller. Returns 0
 * or -1; on error both CMD and DAT lines are reset so the next command
 * starts clean. */
static int sd_cmd(uint32_t index, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
    uint32_t inhibit = PSTATE_CMDI;
    if ((flags & CMD_DATA) || (flags & CMD_RSP_48B) == CMD_RSP_48B)
        inhibit |= PSTATE_DATI;
    if (wait_clear(REG_PSTATE, inhibit) != 0) {
        reset_lines(SYSCTL_SRC | SYSCTL_SRD);
        return -1;
    }

    wr(REG_STAT, 0xFFFFFFFFu);
    wr(REG_ARG, arg);
    wr(REG_CMD, CMD_INDEX(index) | flags);

    uint32_t stat = 0;
    uint32_t i;
    for (i = 0; i < POLL_LIMIT; i++) {
        stat = rd(REG_STAT);
        if (stat & (STAT_CC | STAT_ERRI | STAT_ERR_MASK)) break;
    }
    if (i == POLL_LIMIT || (stat & (STAT_ERRI | STAT_ERR_MASK))) {
        wr(REG_STAT, 0xFFFFFFFFu);
        reset_lines(SYSCTL_SRC | SYSCTL_SRD);
        return -1;
    }

    if (resp) {
        resp[0] = rd(REG_RSP0);
        resp[1] = rd(REG_RSP0 + 4);
        resp[2] = rd(REG_RSP0 + 8);
        resp[3] = rd(REG_RSP0 + 12);
    }
    wr(REG_STAT, STAT_CC);

    if ((flags & CMD_RSP_48B) == CMD_RSP_48B && !(flags & CMD_DATA)) {
        for (i = 0; i < POLL_LIMIT; i++) {
            stat = rd(REG_STAT);
            if (stat & (STAT_TC | STAT_ERRI)) break;
        }
        wr(REG_STAT, 0xFFFFFFFFu);
        if (i == POLL_LIMIT || (stat & STAT_ERRI)) {
            reset_lines(SYSCTL_SRC | SYSCTL_SRD);
            return -1;
        }
    }
    return 0;
}

static int sd_app_cmd(uint32_t index, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
    uint32_t r[4];
    if (sd_cmd(55, g_card.rca << 16, R1, r) != 0) return -1;
    return sd_cmd(index, arg, flags, resp);
}

/* Bits [hi:lo] (hi - lo < 32) of a 136-bit R2 response, numbered as in
 * the SD spec (CSD bit 127 = MSB). Standard SDHCI drops the CRC byte, so
 * response bit k lives at register bit k - 8; the MMCHS keeps it, so bit
 * k is at register bit k. */
static uint32_t r2_bits(const uint32_t resp[4], unsigned hi, unsigned lo)
{
#ifdef SD_HOST_AM335X_MMCHS
    const unsigned shift = 0;
#else
    const unsigned shift = 8;
#endif
    uint32_t v = 0;
    for (unsigned b = hi + 1; b-- > lo; ) {
        unsigned k = b - shift;
        v = (v << 1) | ((resp[k >> 5] >> (k & 31)) & 1u);
    }
    return v;
}

static int64_t csd_sectors(const uint32_t csd[4])
{
    uint32_t structure = r2_bits(csd, 127, 126);
    if (structure == 1) {
        /* CSD v2 (SDHC/SDXC): capacity = (C_SIZE + 1) * 512 KiB */
        uint32_t c_size = r2_bits(csd, 69, 48);
        return ((int64_t)c_size + 1) << 10;
    }
    /* CSD v1 (SDSC): blocks = (C_SIZE + 1) << (C_SIZE_MULT + 2), each
     * 2^READ_BL_LEN bytes. */
    uint32_t c_size = r2_bits(csd, 73, 62);
    uint32_t mult   = r2_bits(csd, 49, 47);
    uint32_t bl_len = r2_bits(csd, 83, 80);
    int64_t bytes = (((int64_t)c_size + 1) << (mult + 2)) << bl_len;
    return bytes >> 9;
}

static int card_identify(void)
{
    uint32_t r[4];
    int v2 = 0;

    g_card.rca = 0;
    g_card.high_capacity = 0;

    (void)sd_cmd(0, 0, R_NONE, NULL);               /* GO_IDLE_STATE */
    delay_polls(1000);

    /* SEND_IF_COND: 2.7-3.6 V, check pattern 0xAA. No answer = v1 card. */
    if (sd_cmd(8, 0x1AA, R7, r) == 0) {
        if ((r[0] & 0xFFF) != 0x1AA) {
            kprintf("[SD]     CMD8 echo mismatch (0x%x)\n", r[0]);
            return -1;
        }
        v2 = 1;
    }

    /* SD_SEND_OP_COND until the card leaves busy (can take up to ~1 s). */
    uint32_t ocr_arg = 0x00FF8000u | (v2 ? (1u << 30) : 0);
    int ready = 0;
    for (int tries = 0; tries < 2000; tries++) {
        if (sd_app_cmd(41, ocr_arg, R3, r) != 0) break;
        if (r[0] & (1u << 31)) {
            ready = 1;
            g_card.high_capacity = v2 && (r[0] & (1u << 30));
            break;
        }
        delay_polls(1000);
    }
    if (!ready) {
        kprintf("[SD]     No card (ACMD41 got no ready response)\n");
        return -1;
    }

    if (sd_cmd(2, 0, R2, r) != 0) return -1;       /* ALL_SEND_CID     */
    if (sd_cmd(3, 0, R6, r) != 0) return -1;       /* SEND_RELATIVE_ADDR */
    g_card.rca = r[0] >> 16;

    if (sd_cmd(9, g_card.rca << 16, R2, r) != 0) return -1;  /* SEND_CSD */
    g_card.sectors = csd_sectors(r);

    if (sd_cmd(7, g_card.rca << 16, R1B, r) != 0) return -1; /* SELECT */

    /* 4-bit bus: ACMD6 on the card, DTW on the host. */
    if (sd_app_cmd(6, 2, R1, r) == 0)
        wr(REG_HCTL, rd(REG_HCTL) | HCTL_DTW_4BIT);
    else
        kprintf("[SD]     ACMD6 failed, staying on a 1-bit bus\n");

    if (!g_card.high_capacity)
        (void)sd_cmd(16, SECTOR_BYTES, R1, r);    /* SET_BLOCKLEN     */

    if (set_clock(SD_XFER_HZ) != 0) return -1;
    return 0;
}

/* ── Block I/O ───────────────────────────────────────────────────────────── */

static int wait_stat(uint32_t bit)
{
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        uint32_t stat = rd(REG_STAT);
        if (stat & (STAT_ERRI | STAT_ERR_MASK)) break;
        if (stat & bit) {
            wr(REG_STAT, bit);
            return 0;
        }
    }
    wr(REG_STAT, 0xFFFFFFFFu);
    reset_lines(SYSCTL_SRC | SYSCTL_SRD);
    return -1;
}

/* One 512-byte block via CMD17/CMD24. The buffer may be unaligned (the
 * BBB runs with the MMU off, where unaligned word access faults), so data
 * moves a byte at a time between it and the 32-bit DATA register. */
static int sd_block(uint32_t lba, uint8_t *buf, int write)
{
    uint32_t r[4];
    uint32_t addr = g_card.high_capacity ? lba : lba * SECTOR_BYTES;

    if (wait_clear(REG_PSTATE, PSTATE_CMDI | PSTATE_DATI) != 0) {
        reset_lines(SYSCTL_SRC | SYSCTL_SRD);
        return -1;
    }
    wr(REG_BLK, (1u << 16) | SECTOR_BYTES);

    if (write) {
        if (sd_cmd(24, addr, R1 | CMD_DATA, r) != 0) return -1;
        if (wait_stat(STAT_BWR) != 0) return -1;
        for (uint32_t i = 0; i < SECTOR_BYTES; i += 4) {
            wr(REG_DATA, (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
                         ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24));
        }
    } else {
        if (sd_cmd(17, addr, R1 | CMD_DATA | CMD_DDIR_READ, r) != 0) return -1;
        if (wait_stat(STAT_BRR) != 0) return -1;
        for (uint32_t i = 0; i < SECTOR_BYTES; i += 4) {
            uint32_t w = rd(REG_DATA);
            buf[i]     = (uint8_t)w;
            buf[i + 1] = (uint8_t)(w >> 8);
            buf[i + 2] = (uint8_t)(w >> 16);
            buf[i + 3] = (uint8_t)(w >> 24);
        }
    }
    /* Transfer complete - for writes, only once the card finishes
     * programming and releases DAT0. */
    return wait_stat(STAT_TC);
}

static int sd_read(blkdev_t *dev, int64_t sector, uint32_t count, void *buf)
{
    (void)dev;
    if (sector < 0 || sector + count > g_card.sectors) return -1;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++, p += SECTOR_BYTES)
        if (sd_block((uint32_t)sector + i, p, 0) != 0) return -1;
    return 0;
}

static int sd_write(blkdev_t *dev, int64_t sector, uint32_t count, const void *buf)
{
    (void)dev;
    if (sector < 0 || sector + count > g_card.sectors) return -1;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++, p += SECTOR_BYTES)
        if (sd_block((uint32_t)sector + i, p, 1) != 0) return -1;
    return 0;
}

/* ── Public entry point ──────────────────────────────────────────────────── */

blkdev_t *sdhci_sd_init(void)
{
    if (host_init() != 0) return NULL;
    if (card_identify() != 0) return NULL;

    memset(&g_sd_dev, 0, sizeof(g_sd_dev));
    strcpy(g_sd_dev.name, "mmc0");
    g_sd_dev.start_sector = 0;
    g_sd_dev.num_sectors  = g_card.sectors;
    g_sd_dev.sector_size  = SECTOR_BYTES;
    g_sd_dev.read         = sd_read;
    g_sd_dev.write        = sd_write;
    g_sd_dev.driver_data  = &g_card;
    g_sd_dev.flags        = BLKDEV_REMOVABLE;

    if (blkdev_register(&g_sd_dev) != 0) return NULL;

    kprintf("[SD]     mmc0: %s card, RCA 0x%x, %u MB\n",
            g_card.high_capacity ? "SDHC/SDXC" : "SDSC",
            (unsigned)g_card.rca, (unsigned)(g_card.sectors >> 11));
    return &g_sd_dev;
}
