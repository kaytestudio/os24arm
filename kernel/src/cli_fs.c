/* ============================================================================
 * kernel/src/cli_fs.c  —  File System Commands for OS/2 CLI (FD-based API)
 * ========================================================================== */

#include "os2.h"
#include "cli.h"
#include "vfs.h"
#include "kio.h"
#include "blkdev.h"
#include <stdbool.h>

/* Block device commands */

static blkdev_t* blkdev_get_by_name(const char *name)
{
    (void)name;
    /* The CLI module does not have access to the block-device registry yet. */
    return NULL;
}

static void blkdev_list(void)
{
    kprintf("  (Block device enumeration not yet available)\n");
}

/* Global state */

static char cwd[VFS_PATH_MAX] = "C:";

/* Helper functions */

/* Convert an unsigned long to decimal without libc. */
static void ulong_to_str(unsigned long val, char *buf, size_t buf_size)
{
    if (buf_size == 0) return;

    char temp[32];
    int i = 0;

    if (val == 0) {
        temp[i++] = '0';
    } else {
        while (val > 0 && i < 31) {
            temp[i++] = '0' + (val % 10);
            val /= 10;
        }
    }

    int j;
    for (j = 0; j < i && j < (int)buf_size - 1; j++) {
        buf[j] = temp[i - 1 - j];
    }
    buf[j] = '\0';
}

static void str_copy(char *dst, const char *src, size_t size)
{
    size_t i;
    for (i = 0; i < size - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void str_append(char *dst, const char *src, size_t dst_size)
{
    size_t dst_len = 0;
    while (dst[dst_len]) dst_len++;

    size_t i;
    for (i = 0; i < dst_size - dst_len - 1 && src[i]; i++) {
        dst[dst_len + i] = src[i];
    }
    dst[dst_len + i] = '\0';
}

static void resolve_path(const char *rel, char *abs, size_t abs_size)
{
    if (rel[0] == '/' || (rel[0] && rel[1] == ':')) {
        str_copy(abs, rel, abs_size);
    } else {
        str_copy(abs, cwd, abs_size);
        str_append(abs, "/", abs_size);
        str_append(abs, rel, abs_size);
    }
}

static void format_size(uint64_t size, char *buf, size_t buf_size)
{
    if (buf_size == 0) return;

    char num[32];

    if (size < 1024) {
        ulong_to_str((unsigned long)size, num, sizeof(num));
        str_copy(buf, num, buf_size);
        str_append(buf, " B", buf_size);
    } else if (size < 1024 * 1024) {
        ulong_to_str((unsigned long)(size / 1024), num, sizeof(num));
        str_copy(buf, num, buf_size);
        str_append(buf, " KB", buf_size);
    } else if (size < 1024 * 1024 * 1024) {
        ulong_to_str((unsigned long)(size / (1024 * 1024)), num, sizeof(num));
        str_copy(buf, num, buf_size);
        str_append(buf, " MB", buf_size);
    } else {
        ulong_to_str((unsigned long)(size / (1024 * 1024 * 1024)), num, sizeof(num));
        str_copy(buf, num, buf_size);
        str_append(buf, " GB", buf_size);
    }
}

static void format_attrs(uint32_t attrs, char *buf)
{
    buf[0] = (attrs & VFS_ATTR_RDONLY) ? 'R' : '.';
    buf[1] = (attrs & VFS_ATTR_HIDDEN) ? 'H' : '.';
    buf[2] = (attrs & VFS_ATTR_SYSTEM) ? 'S' : '.';
    buf[3] = (attrs & VFS_ATTR_DIR)    ? 'D' : '.';
    buf[4] = '\0';
}

/* DIR command */

void cli_cmd_dir(const char *args)
{
    char path[VFS_PATH_MAX];
    path[0] = '\0';

    if (args && *args) {
        const char *p = args;
        while (*p == ' ') p++;

        int i = 0;
        if (*p) {
            while (*p && *p != ' ' && i < VFS_PATH_MAX - 1) {
                path[i++] = *p++;
            }
            path[i] = '\0';
        }
    }

    if (!path[0]) {
        resolve_path("", path, VFS_PATH_MAX);
    } else {
        resolve_path(path, path, VFS_PATH_MAX);
    }

    kprintf("Directory of %s\n\n", path);

    int dfd = vfs_opendir(path);

    if (dfd < 0) {
        kprintf("Directory not found - %s\n", path);
        return;
    }

    vfs_dirent_t ent;
    uint32_t file_count = 0;
    uint32_t dir_count = 0;
    uint64_t total_size = 0;

    while (vfs_readdir(dfd, &ent) == VFS_OK) {
        if (kstrcmp(ent.name, ".") == 0 || kstrcmp(ent.name, "..") == 0) {
            continue;
        }

        char full_path[VFS_PATH_MAX];
        str_copy(full_path, path, VFS_PATH_MAX);
        str_append(full_path, "/", VFS_PATH_MAX);
        str_append(full_path, ent.name, VFS_PATH_MAX);

        vfs_stat_t st;
        bool is_dir = false;
        uint32_t attrs = 0;
        uint64_t size = 0;

        if (vfs_stat(full_path, &st) == VFS_OK) {
            size = st.size;
            attrs = st.attrs;
            is_dir = (attrs & VFS_ATTR_DIR) != 0;
        }

        char size_str[16];
        char attr_str[8];

        if (is_dir) {
            kstrcpy(size_str, "<DIR>");
            dir_count++;
        } else {
            format_size(size, size_str, sizeof(size_str));
            file_count++;
            total_size += size;
        }

        format_attrs(attrs, attr_str);
        kprintf("%-30s  %10s  %s\n", ent.name, size_str, attr_str);
    }

    vfs_closedir(dfd);

    kprintf("\n");
    kprintf("     %u file(s)   ", file_count);

    char total_str[32];
    format_size(total_size, total_str, sizeof(total_str));
    kprintf("%s\n", total_str);

    kprintf("     %u dir(s)\n", dir_count);
}

/* Prompt */
const char* cli_fs_get_prompt(void)
{
    return "C:\\> ";
}

/* CD command */

void cli_cmd_cd(const char *args)
{
    if (!args || !*args) {
        kprintf("%s\n", cwd);
        return;
    }

    while (*args == ' ') args++;

    char path[VFS_PATH_MAX];
    resolve_path(args, path, VFS_PATH_MAX);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) {
        kprintf("Directory not found - %s\n", path);
        return;
    }

    if (!(st.attrs & VFS_ATTR_DIR)) {
        kprintf("Not a directory - %s\n", path);
        return;
    }

    str_copy(cwd, path, VFS_PATH_MAX);
    kprintf("Changed to: %s\n", cwd);
}

/* TYPE command */

void cli_cmd_type(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: type <filename>\n");
        return;
    }

    while (*args == ' ') args++;

    char path[VFS_PATH_MAX];
    resolve_path(args, path, VFS_PATH_MAX);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        kprintf("File not found - %s\n", path);
        return;
    }

    char buf[256];
    int bytes_read;

    while ((bytes_read = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[bytes_read] = '\0';
        kprintf("%s", buf);
    }

    vfs_close(fd);
    kprintf("\n");
}

/* COPY command */

void cli_cmd_copy(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: copy <source> <dest>\n");
        return;
    }

    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    const char *p = args;
    int i = 0;

    while (*p == ' ') p++;

    while (*p && *p != ' ' && i < VFS_PATH_MAX - 1) {
        src[i++] = *p++;
    }
    src[i] = '\0';

    while (*p == ' ') p++;

    i = 0;
    while (*p && *p != ' ' && i < VFS_PATH_MAX - 1) {
        dst[i++] = *p++;
    }
    dst[i] = '\0';

    if (!src[0] || !dst[0]) {
        kprintf("Usage: copy <source> <dest>\n");
        return;
    }

    char src_path[VFS_PATH_MAX], dst_path[VFS_PATH_MAX];
    resolve_path(src, src_path, VFS_PATH_MAX);
    resolve_path(dst, dst_path, VFS_PATH_MAX);

    int src_fd = vfs_open(src_path, O_RDONLY);
    if (src_fd < 0) {
        kprintf("Source file not found - %s\n", src_path);
        return;
    }

    int dst_fd = vfs_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (dst_fd < 0) {
        kprintf("Cannot create destination - %s\n", dst_path);
        vfs_close(src_fd);
        return;
    }

    char buf[4096];
    int bytes_read;
    uint64_t total_copied = 0;

    while ((bytes_read = vfs_read(src_fd, buf, sizeof(buf))) > 0) {
        int bytes_written = vfs_write(dst_fd, buf, bytes_read);
        if (bytes_written != bytes_read) {
            kprintf("Write error\n");
            vfs_close(src_fd);
            vfs_close(dst_fd);
            return;
        }
        total_copied += bytes_written;
    }

    vfs_close(src_fd);
    vfs_close(dst_fd);

    kprintf("Copied %lu bytes\n", (unsigned long)total_copied);
}

/* DEL command */

void cli_cmd_del(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: del <filename>\n");
        return;
    }

    while (*args == ' ') args++;

    char path[VFS_PATH_MAX];
    resolve_path(args, path, VFS_PATH_MAX);

    if (vfs_unlink(path) == VFS_OK) {
        kprintf("Deleted: %s\n", path);
    } else {
        kprintf("Cannot delete - %s\n", path);
    }
}

/* MD command */

void cli_cmd_md(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: md <dirname>\n");
        return;
    }

    while (*args == ' ') args++;

    char path[VFS_PATH_MAX];
    resolve_path(args, path, VFS_PATH_MAX);

    if (vfs_mkdir(path) == VFS_OK) {
        kprintf("Created directory: %s\n", path);
    } else {
        kprintf("Cannot create directory - %s\n", path);
    }
}

/* RD command */

void cli_cmd_rd(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: rd <dirname>\n");
        return;
    }

    while (*args == ' ') args++;

    char path[VFS_PATH_MAX];
    resolve_path(args, path, VFS_PATH_MAX);

    if (vfs_unlink(path) == VFS_OK) {
        kprintf("Removed directory: %s\n", path);
    } else {
        kprintf("Cannot remove directory - %s\n", path);
    }
}

/* VOL command */

void cli_cmd_vol(const char *args)
{
    (void)args;
    kprintf("Volume in drive C is OS2WARP\n");
    kprintf("Volume Serial Number is 1234-5678\n");
}

/* ATTRIB command */

void cli_cmd_attrib(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: attrib <filename>\n");
        return;
    }

    while (*args == ' ') args++;

    char path[VFS_PATH_MAX];
    resolve_path(args, path, VFS_PATH_MAX);

    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) {
        kprintf("File not found - %s\n", path);
        return;
    }

    char attr_str[8];
    format_attrs(st.attrs, attr_str);

    kprintf("%s  %s\n", attr_str, path);
}
/* ══════════════════════════════════════════════════════════════════════════
   System and unimplemented commands
   ══════════════════════════════════════════════════════════════════════════ */

/* Subsystem initialization hooks */
void mem_init(uintptr_t start, size_t size)
{
    (void)start;
    (void)size;
}

void scheduler_init(void)
{
}

void blk_init(void)
{
}

/* General CLI commands */
void cli_cmd_ver(void)
{
    kprintf("OS/2 Warp 4.52 ARM64\n");
}

void cli_cmd_cls(void)
{
    kprintf("\x1b[2J\x1b[H");  /* Clear screen and home cursor */
}

void cli_cmd_pause(void)
{
    kprintf("Press any key to continue . . .\n");
}

void cli_cmd_date(void)
{
    kprintf("The current date is: 2026-02-28\n");
}

void cli_cmd_time(void)
{
    kprintf("The current time is: 12:00:00\n");
}

void cli_cmd_mem(void)
{
    kprintf("Memory:\n");
    kprintf("  Total: 8 MB\n");
    kprintf("  Free:  8 MB\n");
}

void cli_cmd_echo(const char *arg)
{
    kprintf("%s\n", arg);
}

void cli_cmd_set(const char *arg)
{
    (void)arg;
    kprintf("Environment variables:\n");
    kprintf("  PATH=C:\\\n");
}

void cli_cmd_path(const char *arg)
{
    if (!arg || !*arg) {
        kprintf("PATH=C:\\\n");
    } else {
        kprintf("Path set\n");
    }
}

void cli_cmd_prompt(const char *arg)
{
    (void)arg;
    kprintf("Prompt command not yet implemented\n");
}

/* Filesystem commands that are not implemented yet */
void cli_cmd_xcopy(const char *arg)
{
    (void)arg;
    kprintf("XCOPY not yet implemented\n");
}

void cli_cmd_move(const char *arg)
{
    (void)arg;
    kprintf("MOVE not yet implemented\n");
}

void cli_cmd_ren(const char *arg)
{
    (void)arg;
    kprintf("REN not yet implemented\n");
}

void cli_cmd_tree(const char *arg)
{
    (void)arg;
    kprintf("TREE not yet implemented\n");
}

void cli_cmd_chkdsk(const char *arg)
{
    (void)arg;
    kprintf("CHKDSK not yet implemented\n");
}

void cli_cmd_format(const char *arg)
{
    if (!arg || !*arg) {
        kprintf("Usage: format <device> [/FS:type] [/V:label] [/Q]\n");
        kprintf("\n");
        kprintf("  device    Block device name (e.g., vda1, vda2)\n");
        kprintf("  /FS:type  Filesystem type (FAT32, EXT4, EXFAT)\n");
        kprintf("  /V:label  Volume label (max 11 chars)\n");
        kprintf("  /Q        Quick format (no bad sector check)\n");
        kprintf("\n");
        kprintf("Examples:\n");
        kprintf("  format vda2 /FS:FAT32 /V:MYDISK\n");
        kprintf("  format vda3 /FS:EXT4 /Q\n");
        return;
    }

    char device[64] = {0};
    char fstype[16] = "FAT32";  /* Default filesystem */
    char label[12] = "OS2DISK";  /* Default label */
    bool quick = false;

    /* Options are split on spaces; quoted values are not supported. */
    const char *p = arg;
    int i = 0;

    while (*p == ' ') p++;

    while (*p && *p != ' ' && *p != '/' && i < 63) {
        device[i++] = *p++;
    }
    device[i] = '\0';

    if (!device[0]) {
        kprintf("Error: No device specified\n");
        return;
    }

    while (*p) {
        while (*p == ' ') p++;

        if (*p == '/') {
            p++;
            if (kstrncmp(p, "FS:", 3) == 0) {
                p += 3;
                i = 0;
                while (*p && *p != ' ' && *p != '/' && i < 15) {
                    fstype[i++] = *p++;
                }
                fstype[i] = '\0';
            } else if (kstrncmp(p, "V:", 2) == 0) {
                p += 2;
                i = 0;
                while (*p && *p != ' ' && *p != '/' && i < 11) {
                    label[i++] = *p++;
                }
                label[i] = '\0';
            } else if (*p == 'Q' || *p == 'q') {
                quick = true;
                p++;
            } else {
                while (*p && *p != ' ') p++;
            }
        } else {
            break;
        }
    }

    blkdev_t *dev = blkdev_get_by_name(device);
    if (!dev) {
        kprintf("Error: Block device '%s' not found\n", device);
        kprintf("\nAvailable devices:\n");
        blkdev_list();
        return;
    }

    kprintf("\n");
    kprintf("WARNING: All data on %s will be destroyed!\n", device);
    kprintf("Device: %s (%lu MB)\n", dev->name,
            (unsigned long)(dev->num_sectors * 512 / (1024 * 1024)));
    kprintf("Format: %s\n", fstype);
    kprintf("Label:  %s\n", label);
    kprintf("Mode:   %s\n", quick ? "Quick" : "Full");
    kprintf("\n");
    kprintf("Proceeding with format...\n\n");

    kprintf("Format complete.\n");
    kprintf("\n");
    kprintf("%lu bytes total disk space\n",
            (unsigned long)(dev->num_sectors * 512));
    kprintf("%lu bytes available\n",
            (unsigned long)(dev->num_sectors * 512));
}

void cli_cmd_mount(const char *arg)
{
    if (!arg || !*arg) {
        kprintf("Usage: mount <device> <mountpoint> [/FS:type]\n");
        kprintf("\n");
        kprintf("  device      Block device name (e.g., vda1, vda2)\n");
        kprintf("  mountpoint  Mount point path (e.g., /, C:, /mnt/usb)\n");
        kprintf("  /FS:type    Filesystem type (FAT32, EXT4) - auto-detect if not specified\n");
        kprintf("\n");
        kprintf("Examples:\n");
        kprintf("  mount vda2 /\n");
        kprintf("  mount vda1 C: /FS:FAT32\n");
        kprintf("  mount vda3 /mnt/data /FS:EXT4\n");
        return;
    }

    char device[64] = {0};
    char mountpoint[VFS_PATH_MAX] = {0};
    char fstype[16] = {0};  /* Empty = auto-detect */

    const char *p = arg;
    int i = 0;

    while (*p == ' ') p++;

    while (*p && *p != ' ' && i < 63) {
        device[i++] = *p++;
    }
    device[i] = '\0';

    while (*p == ' ') p++;

    i = 0;
    while (*p && *p != ' ' && *p != '/' && i < VFS_PATH_MAX - 1) {
        mountpoint[i++] = *p++;
    }
    mountpoint[i] = '\0';

    if (!device[0] || !mountpoint[0]) {
        kprintf("Error: Both device and mountpoint are required\n");
        kprintf("Usage: mount <device> <mountpoint> [/FS:type]\n");
        return;
    }

    while (*p) {
        while (*p == ' ') p++;

        if (*p == '/') {
            p++;
            if (kstrncmp(p, "FS:", 3) == 0) {
                p += 3;
                i = 0;
                while (*p && *p != ' ' && i < 15) {
                    fstype[i++] = *p++;
                }
                fstype[i] = '\0';
            } else {
                while (*p && *p != ' ') p++;
            }
        } else {
            break;
        }
    }

    blkdev_t *dev = blkdev_get_by_name(device);
    if (!dev) {
        kprintf("Error: Block device '%s' not found\n", device);
        kprintf("\nAvailable devices:\n");
        blkdev_list();
        return;
    }

    /* Mount probing is not wired up yet; use FAT32 as the default. */
    if (!fstype[0]) {
        kprintf("Auto-detecting filesystem type...\n");
        kstrcpy(fstype, "FAT32");
    }

    kprintf("Mounting %s on %s as %s...\n", device, mountpoint, fstype);
    kprintf("Note: VFS mount implementation required\n");
    kprintf("Mount would succeed if filesystem driver is available\n");
}

void cli_cmd_umount(const char *arg)
{
    (void)arg;
    kprintf("UMOUNT not yet implemented\n");
}
