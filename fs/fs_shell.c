/* ============================================================================
 * kernel/fs/fs_shell.c  —  Shell commands for filesystem operations
 *
 * Adds to the OS/2 shell: FORMAT, MOUNT, UMOUNT, LS, CAT, WRITE,
 * MKDIR, RM, LSBLK, FSINFO
 * ========================================================================== */

#include "fs/fs_shell.h"
#include "fs/vfs.h"
#include "fs/blkdev.h"
#include "kio.h"
#include "types.h"

/* Internal buffer for CAT / WRITE */
static uint8_t g_io_buf[4096];

/* fs_shell_format */
/* FORMAT <device> <fstype> [label]
   e.g.: FORMAT ramdisk0 ext4 MYDATA
         FORMAT ramdisk1 fat16
         FORMAT ramdisk2 fat32 WINSHARE
         FORMAT ramdisk3 exfat PORTABLE                                       */
void fs_cmd_format(int argc, char *argv[])
{
    if (argc < 3) {
        kprintf("Usage: FORMAT <device> <ext4|fat16|fat32|exfat> [label]\n");
        return;
    }

    blkdev_t *dev = blkdev_find(argv[1]);
    if (!dev) {
        kprintf("Device '%s' not found. Use LSBLK to list devices.\n", argv[1]);
        return;
    }

    vfs_mkfs_opts_t opts;
    kmemset(&opts, 0, sizeof(opts));

    const char *fsname = argv[2];
    for (char *p = argv[2]; *p; p++)    /* lowercase for comparison */
        if (*p >= 'A' && *p <= 'Z') *p += 32;

    if (kstrcmp(fsname, "ext4")  == 0) opts.fs_type = FS_TYPE_EXT4;
    else if (kstrcmp(fsname, "fat16") == 0) opts.fs_type = FS_TYPE_FAT16;
    else if (kstrcmp(fsname, "fat32") == 0) opts.fs_type = FS_TYPE_FAT32;
    else if (kstrcmp(fsname, "exfat") == 0) opts.fs_type = FS_TYPE_EXFAT;
    else {
        kprintf("Unknown filesystem: %s\n", argv[2]);
        kprintf("Supported: ext4  fat16  fat32  exfat\n");
        return;
    }

    if (argc >= 4) kstrncpy(opts.label, argv[3], 11);

    kprintf(ANSI_YELLOW
    "WARNING: All data on '%s' will be erased!\n"
    "Press ENTER to continue or Ctrl-C to abort...\n"
    ANSI_RESET, argv[1]);

    /* Simple confirmation: wait for ENTER */
    char c = uart_getc();
    if (c != '\r' && c != '\n') { kprintf("Aborted.\n"); return; }

    int r = vfs_mkfs(dev, &opts);
    if (r == VFS_OK)
        kprintf(ANSI_GREEN "Format complete. Use MOUNT to mount the volume.\n" ANSI_RESET);
    else
        kprintf(ANSI_RED "Format failed: %s\n" ANSI_RESET, vfs_errstr(r));
}

/* fs_cmd_mount */
/* MOUNT <device> <mountpoint> <fstype>
   e.g.: MOUNT ramdisk0 /data ext4                                            */
void fs_cmd_mount(int argc, char *argv[])
{
    if (argc < 4) {
        kprintf("Usage: MOUNT <device> <mountpoint> <ext4|fat16|fat32|exfat>\n");
        kprintf("       MOUNT          (list mounts)\n");
        return;
    }

    blkdev_t *dev = blkdev_find(argv[1]);
    if (!dev) {
        kprintf("Device '%s' not found.\n", argv[1]);
        return;
    }

    const char *fsname = argv[3];
    /* lowercase */
    for (char *p = argv[3]; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;

    int fs_type;
    if      (kstrcmp(fsname, "ext4")  == 0) fs_type = FS_TYPE_EXT4;
    else if (kstrcmp(fsname, "fat16") == 0) fs_type = FS_TYPE_FAT16;
    else if (kstrcmp(fsname, "fat32") == 0) fs_type = FS_TYPE_FAT32;
    else if (kstrcmp(fsname, "exfat") == 0) fs_type = FS_TYPE_EXFAT;
    else { kprintf("Unknown fs type: %s\n", argv[3]); return; }

    int r = vfs_mount(argv[2], dev, fs_type);
    if (r != VFS_OK)
        kprintf(ANSI_RED "Mount failed: %s\n" ANSI_RESET, vfs_errstr(r));
}

/* fs_cmd_umount */
void fs_cmd_umount(int argc, char *argv[])
{
    if (argc < 2) { kprintf("Usage: UMOUNT <mountpoint>\n"); return; }
    int r = vfs_umount(argv[1]);
    if (r != VFS_OK)
        kprintf(ANSI_RED "Unmount failed: %s\n" ANSI_RESET, vfs_errstr(r));
}

/* fs_cmd_ls */
/* LS [path]   — list directory contents */
void fs_cmd_ls(int argc, char *argv[])
{
    const char *path = (argc >= 2) ? argv[1] : "/";

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) {
        kprintf("ls: cannot access '%s': No such file or directory\n", path);
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* Single file */
        kprintf("%10lu  %s\n", st.st_size, path);
        return;
    }

    vfs_file_t dir;
    if (vfs_open(path, &dir, O_RDONLY | O_DIRECTORY) != VFS_OK) {
        kprintf("ls: cannot open '%s'\n", path);
        return;
    }

    kprintf(ANSI_CYAN "Directory: %s\n" ANSI_RESET, path);
    kprintf("%-6s  %-10s  %s\n", "TYPE", "SIZE", "NAME");
    kprintf("%-6s  %-10s  %s\n", "----", "----", "----");

    vfs_dirent_t ent;
    int count = 0;
    while (vfs_readdir(&dir, &ent) == VFS_OK) {
        const char *type_str = (ent.type == DT_DIR) ? ANSI_CYAN "<DIR> " ANSI_RESET
                                                     : "      ";
        kprintf("%s  %10lu  %s\n", type_str, ent.size, ent.name);
        count++;
    }
    kprintf("  %d item(s)\n", count);
    vfs_close(&dir);
}

/* fs_cmd_cat */
/* CAT <file>   — display file contents */
void fs_cmd_cat(int argc, char *argv[])
{
    if (argc < 2) { kprintf("Usage: CAT <file>\n"); return; }

    vfs_file_t file;
    if (vfs_open(argv[1], &file, O_RDONLY) != VFS_OK) {
        kprintf("cat: %s: File not found\n", argv[1]);
        return;
    }

    ssize_t n;
    while ((n = vfs_read(&file, g_io_buf, sizeof(g_io_buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) uart_putc((char)g_io_buf[i]);
    }
    kprintf("\n");
    vfs_close(&file);
}

/* fs_cmd_write */
/* WRITE <file> <text>   — write text to a file */
void fs_cmd_write(int argc, char *argv[])
{
    if (argc < 3) { kprintf("Usage: WRITE <file> <text...>\n"); return; }

    vfs_file_t file;
    if (vfs_open(argv[1], &file, O_WRONLY | O_CREAT | O_TRUNC) != VFS_OK) {
        kprintf("write: cannot create '%s'\n", argv[1]);
        return;
    }

    /* Concatenate remaining args into the file */
    for (int i = 2; i < argc; i++) {
        vfs_write(&file, argv[i], kstrlen(argv[i]));
        if (i < argc-1) vfs_write(&file, " ", 1);
    }
    vfs_write(&file, "\n", 1);
    vfs_close(&file);
    kprintf("Written to %s\n", argv[1]);
}

/* fs_cmd_mkdir */
void fs_cmd_mkdir_vfs(int argc, char *argv[])
{
    if (argc < 2) { kprintf("Usage: MKDIR <path>\n"); return; }
    int r = vfs_mkdir(argv[1]);
    if (r != VFS_OK) kprintf("mkdir: %s: %s\n", argv[1], vfs_errstr(r));
}

/* fs_cmd_rm */
void fs_cmd_rm(int argc, char *argv[])
{
    if (argc < 2) { kprintf("Usage: RM <file>\n"); return; }
    int r = vfs_unlink(argv[1]);
    if (r != VFS_OK) kprintf("rm: %s: %s\n", argv[1], vfs_errstr(r));
    else kprintf("Removed %s\n", argv[1]);
}

/* fs_cmd_lsblk */
void fs_cmd_lsblk(void)
{
    blkdev_list();
}

/* fs_cmd_mounts */
void fs_cmd_mounts(void)
{
    vfs_mounts_list();
}

/* fs_cmd_fsinfo */
/* FSINFO — show filesystem layout reference */
void fs_cmd_fsinfo(void)
{
    kprintf(ANSI_CYAN
    "┌────────────────────────────────────────────────────────────────┐\n"
    "│          OS/2 Warp ARM64 — Filesystem Guide                    │\n"
    "├───────────┬────────────┬─────────────────────────────────────  │\n"
    "│ FS Type   │ Ramdisk    │ Notes                                 │\n"
    "├───────────┼────────────┼────────────────────────────────────── │\n"
    "│ ext4      │ ramdisk0   │ Linux-native; 4 KB blocks, extents    │\n"
    "│ fat16     │ ramdisk1   │ ≤ 32 MB; DOS/Windows compatible       │\n"
    "│ fat32     │ ramdisk2   │ Large files; Windows/macOS/Linux      │\n"
    "│ exfat     │ ramdisk3   │ SD cards; macOS/Windows/Linux         │\n"
    "└───────────┴────────────┴────────────────────────────────────── ┘\n"
    ANSI_RESET);
    kprintf("\nQuick start:\n");
    kprintf("  FORMAT ramdisk0 ext4  MYDATA\n");
    kprintf("  MOUNT  ramdisk0 /data ext4\n");
    kprintf("  WRITE  /data/hello.txt Hello from OS/2 Warp!\n");
    kprintf("  CAT    /data/hello.txt\n");
    kprintf("  LS     /data\n");
    kprintf("  UMOUNT /data\n\n");
    kprintf("  FORMAT ramdisk3 exfat PORTABLE\n");
    kprintf("  MOUNT  ramdisk3 /usb  exfat\n");
}
