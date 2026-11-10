/**
 * @file beagle_main.c
 * @brief Kernel entry and serial shell for BeagleBone Black / BeagleV-Ahead
 *
 * The counterpart of rpi_main.c for the two BeagleBoard.org targets that
 * aren't AArch64:
 *
 *   - PLATFORM_BBB:           TI AM335x, ARM Cortex-A8 (32-bit ARMv7-A)
 *   - PLATFORM_BEAGLEV_AHEAD: T-Head TH1520, 4x XuanTie C910 (RV64GC)
 *
 * plus PLATFORM_QEMU_RISCV, the BeagleV-Ahead kernel on QEMU's riscv64
 * "virt" machine (make run), whose SD controller sits behind PCI.
 *
 * Entered from bbb_boot.S / beaglev_boot.S with a stack, zeroed .bss and
 * interrupts masked. Brings up the console UART, the heap and the
 * scheduler stub, then runs a small command prompt on the serial console.
 * The MMU, GUI, filesystems and the LX loader are AArch64-only for now.
 *
 * With HAVE_OS2API (both boards - see kernel/CMakeLists.txt) the OS/2
 * API (os2api/) and the VFS under it are linked in too, and CD/DIR/TYPE go
 * through DosSetCurrentDir/DosFindFirst/DosOpen/... exactly like the
 * AArch64 shell's DOS commands.
 *
 * With HAVE_SD the microSD card is brought up at boot (sdhci.c), its
 * partitions are probed (MBR, or GPT), and the FAT (FAT16/FAT32) and ext4
 * partitions found are mounted. The boot partition - the FAT partition
 * U-Boot loaded this kernel from: the MBR "active" one, else the first
 * FAT partition - becomes "/" (drive C:); every other volume goes at
 * "/<partition>", e.g. "/mmc0p2". fat32.c and the ext4 driver each keep
 * one volume's state, so at most one of each is mounted.
 */

#include "types.h"
#include "ns16550.h"

#ifdef HAVE_OS2API
#include "os2_api.h"
#include "vfs.h"
#include "kprintf.h"
#include "string.h"
#endif

#ifdef HAVE_SD
#include "sdhci.h"
#include "mbr.h"
#include "gpt.h"
#include "fat32.h"
#include "ext4.h"
#ifdef PLATFORM_QEMU_RISCV
#include "qemu_pci.h"
#endif
#endif

extern void mem_init(uintptr_t start, size_t size);
extern void mem_get_stats(uintptr_t *used, uintptr_t *total);
extern void scheduler_init(void);
extern void stack_protector_reseed(void);

/* From bbb_linker.ld / beaglev_linker.ld */
extern char _start[];
extern char _bss_end[];
extern char _heap_start[];

#define HEAP_START  ((uintptr_t)_heap_start)
#define HEAP_SIZE   (32u * 1024u * 1024u)   /* 32 MB */

/* ── Board description ───────────────────────────────────────────────────── */

#if defined(PLATFORM_BBB)
#define BOARD_NAME     "BeagleBone Black"
#define BOARD_EDITION  "ARMv7 Edition"
#define BOARD_SOC      "TI AM335x (Sitara)"
#define BOARD_CPU      "ARM Cortex-A8 @ 1 GHz (32-bit ARMv7-A)"
#define BOARD_RAM      "512 MB DDR3L"
#define BOARD_UART     "UART0 (16550)"
#elif defined(PLATFORM_BEAGLEV_AHEAD)
#define BOARD_NAME     "BeagleV-Ahead"
#define BOARD_EDITION  "RISC-V Edition"
#define BOARD_SOC      "T-Head TH1520"
#define BOARD_CPU      "4x XuanTie C910 @ 1.85 GHz (RV64GC)"
#define BOARD_RAM      "4 GB LPDDR4"
#define BOARD_UART     "UART0 (DW APB 16550)"
#elif defined(PLATFORM_QEMU_RISCV)
#define BOARD_NAME     "QEMU RISC-V virt"
#define BOARD_EDITION  "RISC-V Edition"
#define BOARD_SOC      "QEMU virt machine"
#define BOARD_CPU      "RV64GC (emulated)"
#define BOARD_RAM      "as set by QEMU -m"
#define BOARD_UART     "ns16550a"
#else
#error "beagle_main.c needs PLATFORM_BBB, PLATFORM_BEAGLEV_AHEAD or PLATFORM_QEMU_RISCV"
#endif

static unsigned long boot_hart_id;
static unsigned long boot_dtb_addr;

/* ── String utilities ────────────────────────────────────────────────────── */

static int str_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int str_tokenize(char *str, char **tokens, int max_tokens)
{
    int count = 0;
    char *p = str;

    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p == '\0')
            break;

        tokens[count++] = p;

        while (*p && *p != ' ' && *p != '\t')
            p++;

        if (*p)
            *p++ = '\0';
    }

    return count;
}

static void str_toupper(char *s)
{
    while (*s) {
        if (*s >= 'a' && *s <= 'z')
            *s = *s - 'a' + 'A';
        s++;
    }
}

/* ── Commands ────────────────────────────────────────────────────────────── */

static void cmd_ver(void)
{
    uart_puts("\n");
    uart_puts("OS/2 Warp Version 4.52 (" BOARD_EDITION ")\n");
    uart_puts("Platform: " BOARD_NAME "\n");
    uart_puts("SoC:      " BOARD_SOC "\n");
    uart_puts("CPU:      " BOARD_CPU "\n");
    uart_puts("Kernel:   Bare Metal Build (serial console)\n");
    uart_puts("\n");
}

static void cmd_help(void)
{
    uart_puts("\n");
    uart_puts("Available commands:\n");
    uart_puts("  VER       - Display OS version\n");
    uart_puts("  HELP      - Display this help\n");
    uart_puts("  MEM       - Display memory information\n");
    uart_puts("  ECHO      - Display message\n");
#ifdef HAVE_OS2API
    uart_puts("  CD        - Show or change directory (DosSetCurrentDir)\n");
    uart_puts("  DIR       - List files (DosFindFirst/DosFindNext)\n");
    uart_puts("  TYPE      - Display a text file (DosOpen/DosRead)\n");
#endif
    uart_puts("  CLS       - Clear the screen\n");
    uart_puts("  EXIT      - Exit to idle loop\n");
    uart_puts("\n");
}

static void cmd_mem(void)
{
    uintptr_t used, total;
    mem_get_stats(&used, &total);

    uart_puts("\n");
    uart_puts("Memory Information:\n");
    uart_puts("  Kernel:     "); uart_puthex((unsigned long)(uintptr_t)_start);
    uart_puts(" - ");            uart_puthex((unsigned long)(uintptr_t)_bss_end); uart_puts("\n");
    uart_puts("  Heap start: "); uart_puthex((unsigned long)HEAP_START); uart_puts("\n");
    uart_puts("  Heap size:  "); uart_puthex((unsigned long)total);      uart_puts(" (32 MB)\n");
    uart_puts("  Heap used:  "); uart_puthex((unsigned long)used);       uart_puts("\n");
    uart_puts("  Total RAM:  " BOARD_RAM " (" BOARD_NAME ")\n");
    uart_puts("\n");
}

static void cmd_echo(int argc, char **argv)
{
    uart_puts("\n");
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            uart_putc(' ');
        uart_puts(argv[i]);
    }
    uart_puts("\n");
}

#ifdef HAVE_OS2API
/* Print an OS/2 API failure the way CMD.EXE does: SYSnnnn plus a hint. */
static void print_os2_error(APIRET rc)
{
    kprintf("SYS%04u: ", (unsigned)rc);
    switch (rc) {
        case ERROR_FILE_NOT_FOUND:    uart_puts("The system cannot find the file specified.\n"); break;
        case ERROR_PATH_NOT_FOUND:    uart_puts("The system cannot find the path specified.\n"); break;
        case ERROR_INVALID_DRIVE:     uart_puts("The system cannot find the drive specified (nothing mounted yet).\n"); break;
        case ERROR_NO_MORE_FILES:     uart_puts("No more files.\n"); break;
        default:                      uart_puts("OS/2 API call failed.\n"); break;
    }
}

static void cmd_cd(int argc, char **argv)
{
    uart_puts("\n");
    if (argc > 1) {
        APIRET rc = DosSetCurrentDir(argv[1]);
        if (rc != NO_ERROR)
            print_os2_error(rc);
        return;
    }

    char cwd[VFS_PATH_MAX];
    APIRET rc = DosQueryCurrentDir(cwd, sizeof(cwd));
    if (rc != NO_ERROR) {
        print_os2_error(rc);
        return;
    }
    uart_puts(cwd);
    uart_puts("\n");
}

#define OS2_FILE_DIRECTORY 0x10   /* FILEFINDBUF/FILESTATUS attribute */

static void cmd_dir(int argc, char **argv)
{
    /* Like CMD.EXE: DIR <directory> lists what's inside it. */
    char pattern[VFS_PATH_MAX];
    FILESTATUS info;
    if (argc < 2) {
        strcpy(pattern, "*");
    } else if (DosQueryPathInfo(argv[1], 1, &info, sizeof(info)) == NO_ERROR &&
               (info.attributes & OS2_FILE_DIRECTORY)) {
        size_t n = strlen(argv[1]);
        if (n > sizeof(pattern) - 3) n = sizeof(pattern) - 3;
        memcpy(pattern, argv[1], n);
        if (n > 0 && pattern[n - 1] != '\\' && pattern[n - 1] != '/')
            pattern[n++] = '\\';
        pattern[n++] = '*';
        pattern[n] = '\0';
    } else {
        strncpy(pattern, argv[1], sizeof(pattern) - 1);
        pattern[sizeof(pattern) - 1] = '\0';
    }

    HDIR hdir = 0;
    FILEFINDBUF buf;
    uint32_t count = 1;

    uart_puts("\n");
    APIRET rc = DosFindFirst(pattern, &hdir, 0, &buf, sizeof(buf), &count);
    if (rc != NO_ERROR) {
        print_os2_error(rc);
        return;
    }

    unsigned files = 0;
    do {
        if (buf.attributes & OS2_FILE_DIRECTORY)
            uart_puts("     <DIR>  ");
        else
            kprintf("%10u  ", (unsigned)buf.file_size);
        uart_puts(buf.name);
        uart_puts("\n");
        files++;
        count = 1;
    } while (DosFindNext(hdir, &buf, sizeof(buf), &count) == NO_ERROR);
    DosFindClose(hdir);

    kprintf("%u file(s)\n", files);
}
#endif /* HAVE_OS2API */

#ifdef HAVE_OS2API
static void cmd_type(int argc, char **argv)
{
    uart_puts("\n");
    if (argc < 2) {
        uart_puts("Usage: TYPE <file>\n");
        return;
    }

    HFILE fh;
    uint32_t action;
    APIRET rc = DosOpen(argv[1], &fh, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        print_os2_error(rc);
        return;
    }

    char buf[128];
    uint32_t got;
    while (DosRead(fh, buf, sizeof(buf), &got) == NO_ERROR && got > 0) {
        for (uint32_t i = 0; i < got; i++) {
            if (buf[i] == '\n') uart_putc('\r');
            uart_putc(buf[i]);
        }
    }
    DosClose(fh);
    uart_puts("\n");
}
#endif /* HAVE_OS2API */

#ifdef HAVE_SD
extern void *mem_alloc(size_t size);

enum { FS_UNKNOWN, FS_FAT, FS_EXT4 };

/* Identify a volume from its own first sectors, so only the right driver
 * is ever asked to mount it (each prints errors on a mismatch). FAT: a
 * boot sector with a jump instruction, 0x55AA signature, 512-byte sectors
 * and a plausible BPB - fat32.c itself then works out FAT16 vs FAT32 (and
 * rejects FAT12) from the cluster count. ext4: superblock magic 0xEF53 at
 * byte 1024 + 0x38. */
static int detect_fs(blkdev_t *dev)
{
    static uint8_t sec[512] __attribute__((aligned(4)));

    if (blk_read(dev, 0, 1, sec) == 0 &&
        (sec[0] == 0xEB || sec[0] == 0xE9) &&  /* x86 jump            */
        sec[510] == 0x55 && sec[511] == 0xAA &&
        sec[11] == 0x00 && sec[12] == 0x02 &&  /* BPB_BytsPerSec 512  */
        sec[13] != 0 &&                        /* BPB_SecPerClus      */
        (sec[14] | sec[15]) != 0 &&            /* BPB_RsvdSecCnt      */
        sec[16] != 0)                          /* BPB_NumFATs         */
        return FS_FAT;

    if (blk_read(dev, 2, 1, sec) == 0 && sec[0x38] == 0x53 && sec[0x39] == 0xEF)
        return FS_EXT4;

    return FS_UNKNOWN;
}

static void mount_volume(blkdev_t *dev, int *mounted, int *have_fat, int *have_ext4)
{
    char mp[40];
    if (*mounted == 0) {
        strcpy(mp, "/");
    } else {
        mp[0] = '/';
        strncpy(mp + 1, dev->name, sizeof(mp) - 2);
        mp[sizeof(mp) - 1] = '\0';
    }

    int fs = detect_fs(dev);
    int r = -1;
    if (fs == FS_FAT && !*have_fat) {
        fat32_mount_t *m = (fat32_mount_t *)mem_alloc(sizeof(*m));
        if (!m) return;
        memset(m, 0, sizeof(*m));
        m->dev = dev;
        r = vfs_mount(mp, &vfs_fat32_fs, m);
        if (r == VFS_OK) *have_fat = 1;
    } else if (fs == FS_EXT4 && !*have_ext4) {
        r = vfs_mount(mp, ext4_get_fs(), dev);
        if (r == VFS_OK) *have_ext4 = 1;
    } else {
        kprintf("[SD]     %s: %s, not mounted\n", dev->name,
                fs == FS_UNKNOWN ? "no FAT/ext4 filesystem" : "second volume of the same type");
        return;
    }

    if (r == VFS_OK) {
        kprintf("[SD]     %s mounted at %s%s\n", dev->name, mp, *mounted == 0 ? " (C:)" : "");
        (*mounted)++;
    }
}

static void storage_init(void)
{
#ifdef PLATFORM_QEMU_RISCV
    if (qemu_pci_setup_sdhci() != 0) {
        uart_puts("[SD]     No SD controller (start QEMU with -device sdhci-pci)\n");
        return;
    }
#endif
    blkdev_t *sd = sdhci_sd_init();
    if (!sd) {
        uart_puts("[SD]     No microSD card found - nothing mounted\n");
        return;
    }

    if (mbr_probe(sd) == 0)
        gpt_probe(sd);

    /* The boot partition becomes C: - the MBR "active" FAT partition,
     * else the first FAT partition - then everything else, in order. */
    blkdev_t *parts[8];
    int nparts = 0;
    blkdev_t *dev;
    for (int i = 0; (dev = blkdev_get_by_index(i)) != NULL && nparts < 8; i++)
        if (dev->parent == sd)
            parts[nparts++] = dev;
    if (nparts == 0)                /* unpartitioned "superfloppy" card */
        parts[nparts++] = sd;

    blkdev_t *boot = NULL;
    for (int i = 0; i < nparts && !boot; i++)
        if ((parts[i]->flags & BLKDEV_BOOT) && detect_fs(parts[i]) == FS_FAT)
            boot = parts[i];
    for (int i = 0; i < nparts && !boot; i++)
        if (detect_fs(parts[i]) == FS_FAT)
            boot = parts[i];

    int mounted = 0, have_fat = 0, have_ext4 = 0;
    if (boot)
        mount_volume(boot, &mounted, &have_fat, &have_ext4);
    for (int i = 0; i < nparts; i++)
        if (parts[i] != boot)
            mount_volume(parts[i], &mounted, &have_fat, &have_ext4);

    if (mounted == 0)
        uart_puts("[SD]     No mountable FAT/ext4 volume on the card\n");
}
#endif /* HAVE_SD */

static void execute_command(char *cmdline)
{
    char *argv[16];
    int argc = str_tokenize(cmdline, argv, 16);

    if (argc == 0)
        return;

    str_toupper(argv[0]);

    if (str_equal(argv[0], "VER")) {
        cmd_ver();
    } else if (str_equal(argv[0], "HELP") || str_equal(argv[0], "?")) {
        cmd_help();
    } else if (str_equal(argv[0], "MEM")) {
        cmd_mem();
    } else if (str_equal(argv[0], "ECHO")) {
        cmd_echo(argc, argv);
#ifdef HAVE_OS2API
    } else if (str_equal(argv[0], "CD") || str_equal(argv[0], "CHDIR")) {
        cmd_cd(argc, argv);
    } else if (str_equal(argv[0], "DIR")) {
        cmd_dir(argc, argv);
    } else if (str_equal(argv[0], "TYPE")) {
        cmd_type(argc, argv);
#endif
    } else if (str_equal(argv[0], "CLS")) {
        uart_puts("\x1b[2J\x1b[H");
    } else if (str_equal(argv[0], "EXIT")) {
        uart_puts("\n[KERNEL] Exiting to idle loop...\n\n");
        while (1) {
            __asm__ volatile("wfi");
        }
    } else {
        uart_puts("\nBad command or file name: ");
        uart_puts(argv[0]);
        uart_puts("\nType HELP for list of commands\n");
    }
}

/* ── Kernel shell ────────────────────────────────────────────────────────── */

static void print_banner(void)
{
    uart_puts("\n");
    uart_puts("  ___  ____    ______     _  _  __              \n");
    uart_puts(" / _ \\/ ___|  / /___ \\   | || |/ /___ _  __    \n");
    uart_puts("| | | \\___ \\ / /  __) |  | || '_// _ \\ \\/ /   \n");
    uart_puts("| |_| |___) / /  / __/   |__   _|  __/>  <    \n");
    uart_puts(" \\___/|____/_/  |_____|     |_|  \\___/_/\\_\\  \n");
    uart_puts("\n");
    uart_puts("  OS/2 Warp 4.52  -  " BOARD_EDITION " Bare-Metal Kernel\n");
    uart_puts("  Platform : " BOARD_NAME "\n");
    uart_puts("  CPU      : " BOARD_CPU "\n");
    uart_puts("  UART     : " BOARD_UART " @ ");
    uart_puthex((unsigned long)(uintptr_t)UART_BASE);
    uart_puts("\n");
#if defined(PLATFORM_BEAGLEV_AHEAD) || defined(PLATFORM_QEMU_RISCV)
    uart_puts("  Boot hart: ");
    uart_puthex(boot_hart_id);
    uart_puts("\n");
#endif
    uart_puts("  DTB      : ");
    uart_puthex(boot_dtb_addr);
    uart_puts("\n\n");
}

static void kernel_shell(void)
{
    char buffer[256];
    int pos = 0;

    uart_puts("\n");
    uart_puts("OS/2 Command Prompt - " BOARD_NAME " Edition\n");
    uart_puts("Type HELP for available commands\n");
    uart_puts("\n");
    uart_puts("[C:\\]> ");

    while (1) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            uart_puts("\n");

            if (pos > 0) {
                buffer[pos] = '\0';
                execute_command(buffer);
                pos = 0;
            }

            uart_puts("[C:\\]> ");
        } else if (c == 0x7F || c == 0x08) {
            if (pos > 0) {
                pos--;
                uart_putc('\b');
                uart_putc(' ');
                uart_putc('\b');
            }
        } else if (c == 0x03) {
            uart_puts("^C\n[C:\\]> ");
            pos = 0;
        } else if (c >= 32 && c < 127) {
            if (pos < 255) {
                buffer[pos++] = c;
                uart_putc(c);
            }
        }
    }
}

/* ── Kernel entry point ──────────────────────────────────────────────────── */

/**
 * @brief C entry point, called from bbb_boot.S / beaglev_boot.S
 *
 * @param hart_id  RISC-V: hart that won the boot lottery (from booti's a0).
 *                 ARMv7: always 0 (the AM335x is single-core).
 * @param dtb      Device tree address U-Boot passed (unused so far).
 */
void kernel_main(unsigned long hart_id, unsigned long dtb)
{
    boot_hart_id = hart_id;
    boot_dtb_addr = dtb;

    stack_protector_reseed();
    uart_init();

    print_banner();

    uart_puts("[KERNEL] Subsystems initializing...\n");

    mem_init(HEAP_START, HEAP_SIZE);
    uart_puts("[MEM]    Allocator    : initialized\n");

    scheduler_init();
    uart_puts("[SCHED]  Scheduler    : initialized\n");

    uart_puts("[IPC]    Semaphores   : stub\n");
    uart_puts("[MMU]    Not enabled on this platform yet\n");
#ifdef HAVE_OS2API
    vfs_init();
#ifdef HAVE_SD
    uart_puts("[VFS]    Initialized\n");
    storage_init();
#else
    uart_puts("[VFS]    Initialized (nothing mounted - no SD/eMMC driver yet)\n");
#endif
    uart_puts("[OS2API] DOSCALLS API linked (DosOpen, DosFindFirst, KernVMAlloc, ...)\n");
#else
    uart_puts("[VFS]    Disabled (no SD/eMMC driver yet)\n");
#endif

    uart_puts("\n[KERNEL] Boot complete on " BOARD_NAME "\n");

    kernel_shell();
}
