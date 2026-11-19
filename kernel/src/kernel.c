/* ============================================================================
 * kernel/kernel.c  —  OS/2 Warp 4.52 ARM64 — kernel entry
 *
 * Call order:
 *   _start (start.S)  →  kernel_main()  →  shell_run()
 * ========================================================================== */

#include "types.h"
#include "uart.h"
#include "kio.h"
#include "memory.h"
#include "shell.h"
#include "fs/vfs.h"      /* VFS interface from fs/vfs.c */
#include "fs/blkdev.h"   /* Block device interface */
#include "string.h"      /* For strlen */

/* Filesystem type constants - should match what's in your fs drivers */
#ifndef FS_EXT4
#define FS_EXT4   1
#define FS_FAT32  2
#define FS_FAT16  3
#define FS_EXFAT  4
#endif

/* Default CONFIG.SYS if not found on disk */
static const char default_config[] =
    "REM OS/2 Warp 4.52 ARM64 - Default Configuration\r\n"
    "PROTSHELL=C:\\OS2\\PMSHELL.EXE\r\n"
    "SET PATH=C:\\OS2;C:\\OS2\\SYSTEM\r\n"
    "SET LIBPATH=C:\\OS2\\DLL\r\n"
    "BUFFERS=90\r\n"
    "FILES=256\r\n"
    "THREADS=4096\r\n";

/* Heap lives right after the kernel image.
 * Linker exports _bss_end; we place the heap 1 MB past it. */
extern uint8_t _bss_end;
#define HEAP_START  ((uintptr_t)&_bss_end + 0x1000u)
#define HEAP_SIZE   (8u * 1024u * 1024u)   /* 8 MB */
extern void rexx_run_script(const char* path);

/* Block device initialization - implement this in your fs/ folder */
extern void blk_init(void);
extern blkdev_t *blk_get(const char *name);

void kernel_main(void)
{
    uart_init();

    /* Basic boot banner over serial before shell starts */
    uart_puts("\033[2J\033[H");   /* clear terminal */
    uart_puts("\r\n");
    uart_puts("  *** OS/2 Warp 4.52 ARM64 Boot ***\r\n");
    uart_puts("  Initialising memory subsystem...\r\n");

    mem_init(HEAP_START, HEAP_SIZE);

    uart_puts("  Initializing subsystems...\r\n");

    console_init();
    scheduler_init();

    /* Initialize VFS - this is in fs/vfs.c */
    uart_puts("  [VFS] Initializing VFS and filesystem drivers...\r\n");
    vfs_init();  /* Registers ext4, fat, exfat drivers */

    /* Initialize block devices - you need to implement this */
    uart_puts("  [BLK] Initializing block device subsystem...\r\n");
    blk_init();  /* Should initialize VirtIO and create block devices */

    /* Small delay for device initialization */
    for (volatile int i = 0; i < 1000000; i++);

    /* ─────────────────────────────────────────────────────────────────────
     * Mount root filesystem from VirtIO disk
     * ───────────────────────────────────────────────────────────────────── */
    uart_puts("\r\n  [VFS] Mounting filesystems...\r\n");

    /* Get the block device for partition 2 (root) */
    uart_puts("  [VFS] Looking for block device vda2...\r\n");
    blkdev_t *vda2 = blk_get("vda2");
    
    if (!vda2) {
        uart_puts("        Block device vda2 not found.\r\n");
        uart_puts("        Implement blk_init() and blk_get() in your fs/ folder\r\n");
        uart_puts("        Operating in ramdisk mode\r\n\r\n");
        goto skip_mounts;
    }
    
    uart_puts("        Found block device: ");
    uart_puts(vda2->name);
    uart_puts("\r\n");

    /* Mount /dev/vda2 (EXT4 partition) as root */
    uart_puts("  [VFS] Attempting: vda2 -> / (ext4)\r\n");
    int mount_result = vfs_mount("/", vda2, FS_EXT4);
    
    if (mount_result == VFS_OK) {
        uart_puts("        Root filesystem mounted (C:)\r\n");

        /* Try to read CONFIG.SYS from disk */
        uart_puts("  [CFG] Loading CONFIG.SYS...\r\n");
        
        /* Use vfs_file_t as struct, not pointer! */
        vfs_file_t cfg_file;
        int open_result = vfs_open("/config.sys", &cfg_file, VFS_O_RDONLY);

        if (open_result != VFS_OK) {
            /* Try alternate location */
            open_result = vfs_open("/etc/config.sys", &cfg_file, VFS_O_RDONLY);
        }

        if (open_result == VFS_OK) {
            uart_puts("        CONFIG.SYS found on disk\r\n");

            /* Read CONFIG.SYS into buffer */
            if (cfg_file.size > 0 && cfg_file.size < 65536) {
                char *config_buf = (char *)mem_alloc(cfg_file.size + 1);
                if (config_buf) {
                    ssize_t bytes = vfs_read(&cfg_file, config_buf, cfg_file.size);
                    if (bytes > 0) {
                        config_buf[bytes] = '\0';
                        uart_puts("        CONFIG.SYS loaded (");
                        uart_put_u32((uint32_t)bytes);
                        uart_puts(" bytes)\r\n");

                        /* TODO: Parse CONFIG.SYS here */
                        /* config_parse(config_buf); */
                    }
                    mem_free(config_buf);
                }
            }
            vfs_close(&cfg_file);
        } else {
            /* CONFIG.SYS not found - create default */
            uart_puts("        Warning: CONFIG.SYS not found, creating default...\r\n");

            open_result = vfs_open("/config.sys", &cfg_file, VFS_O_WRONLY | VFS_O_CREAT);
            if (open_result == VFS_OK) {
                ssize_t written = vfs_write(&cfg_file, default_config, strlen(default_config));
                vfs_close(&cfg_file);
                if (written > 0) {
                    uart_puts("        Default CONFIG.SYS created (");
                    uart_put_u32((uint32_t)written);
                    uart_puts(" bytes)\r\n");
                }
            } else {
                uart_puts("        Warning: Could not create CONFIG.SYS: ");
                uart_puts(vfs_errstr(open_result));
                uart_puts("\r\n");
            }
        }

        /* Mount additional partitions */
        uart_puts("  [VFS] Mounting additional partitions...\r\n");

        /* Mount EFI partition (FAT32) */
        blkdev_t *vda1 = blk_get("vda1");
        if (vda1) {
            uart_puts("  [VFS] Attempting: vda1 -> /boot/efi (fat32)\r\n");
            if (vfs_mount("/boot/efi", vda1, FS_FAT32) == VFS_OK) {
                uart_puts("        EFI partition mounted\r\n");
            }
        }

        /* Mount FAT16 data partition */
        blkdev_t *vda3 = blk_get("vda3");
        if (vda3) {
            uart_puts("  [VFS] Attempting: vda3 -> /mnt/fat16 (fat16)\r\n");
            if (vfs_mount("/mnt/fat16", vda3, FS_FAT16) == VFS_OK) {
                uart_puts("        FAT16 partition mounted (D:)\r\n");
            }
        }

        /* Mount exFAT partition */
        blkdev_t *vda4 = blk_get("vda4");
        if (vda4) {
            uart_puts("  [VFS] Attempting: vda4 -> /mnt/exfat (exfat)\r\n");
            if (vfs_mount("/mnt/exfat", vda4, FS_EXFAT) == VFS_OK) {
                uart_puts("        exFAT partition mounted (E:)\r\n");
            }
        }
    } else {
        uart_puts("        FAILED to mount root filesystem\r\n");
        uart_puts("        Error: ");
        uart_puts(vfs_errstr(mount_result));
        uart_puts("\r\n\r\n");
        uart_puts("  [VFS] Troubleshooting:\r\n");
        uart_puts("        - Is EXT4 driver registered? (check vfs_init)\r\n");
        uart_puts("        - Does block device have valid EXT4 filesystem?\r\n");
        uart_puts("        - Run 'make mkdisk' to create disk image\r\n");
        uart_puts("\r\n");
        uart_puts("  [VFS] Operating in ramdisk mode\r\n");
    }

skip_mounts:
    uart_puts("\r\n");

    shell_start();

    rexx_run_script("/boot/init.rex");

    for (;;); /* idle loop */
    /* Hand off to the interactive shell — never returns */
    shell_run();

    /* Unreachable */
    while (1) {
        __asm__ volatile("wfe");
    }
}