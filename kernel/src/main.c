#include "os2.h"
#include "keyboard.h"
#include "vfs.h"
#include "blkdev.h"
#include "ifs_loader.h"
#include "../../fs/ext4/ext4.h"
#include "../../fs/btrfs/btrfs.h"
#include "fat32.h"
#include "hpfs.h"
#include "linux_abi.h"
#include "net/net.h"
#include "os2warp_version.h"
#include "kio.h"
#include "basic.h"
#include "gui/framebuffer.h"
#include "gui/virtio_gpu.h"
#include "gui/ramfb.h"
#include "gui/wayland.h"
#include "gui/virtio_input.h"
#include "dos.h"
#include "lx_loader.h"
#include "ai.h"
#include "string.h"
#include "unixroot.h"
#include "kprintf.h"
#include "hyp.h"
#include "usb.h"
#include "usbd.h"
#include "usbmouse.h"
#include "ramfs.h"


/* CONFIG.SYS SET RUNWORKPLACE: PMSHELL.EXE/DESKTOP (and the default) start the
 * desktop at boot, CMD.EXE/CMD the console - see desktop_at_boot() */
static int run_desktop_at_boot = 1;

/* Memory heap configuration */
extern uint8_t _bss_end;
#define HEAP_START  ((uintptr_t)&_bss_end + 0x1000u)
/* 8MB used to be enough, but the 1024x768 front+back framebuffers alone
 * now take 6MB, leaving too little for the GUI's window surface buffers
 * (each xdg_toplevel gets its own - see gui/wayland.c) once there's more
 * than one or two windows, e.g. across multiple workplaces. */
#define HEAP_SIZE   (32u * 1024u * 1024u)

extern void mem_init(uintptr_t start, size_t size);
extern void scheduler_init(void);

/* ── UART ────────────────────────────────────────────────────────────────── */

#define UART_BASE    ((volatile ULONG *)0x09000000UL)
#define UART_DR      (UART_BASE + 0x00)
#define UART_FR      (UART_BASE + 0x06)
#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)

extern void (*console_capture)(char c);   /* uart.c */

static void uart_putc(char c)
{
    if (console_capture) { console_capture(c); return; }
    while (*UART_FR & UART_FR_TXFF);
    *UART_DR = (ULONG)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_puthex(ULONG val)
{
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        uart_putc(hex[(val >> shift) & 0xF]);
}

/* ── String utilities ────────────────────────────────────────────────────── */

static int my_strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static int my_strlen(const char *s)
{
    int len = 0;
    while (*s++)
        len++;
    return len;
}

static char* my_strcpy(char *dst, const char *src)
{
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

static int my_atoi(const char *str)
{
    int result = 0;
    int sign = 1;

    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}


/* Live framebuffer test-pattern renderer, defined in gui/gui_demo.c */
extern void draw_framebuffer_content(wl_surface_t *surface, fb_rect_t damage);

/* Wayland-style surfaces start with undefined (zeroed) buffer content
 * until a client paints something - there's no implicit default fill the
 * way the old flat window manager gave every window free of charge. */
static void cmd_gui_flat_background(wl_surface_t *surface, fb_rect_t damage)
{
    (void)damage;
    wl_buf_fillrect(surface, 0, 0, surface->width, surface->height, WPS_WINDOW_BG);
}

/* One distinctly-colored demo window per extra workplace, so switching
 * with Cmd+Escape is obviously visible. */
static void cmd_gui_wp2_background(wl_surface_t *surface, fb_rect_t damage)
{
    (void)damage;
    wl_buf_fillrect(surface, 0, 0, surface->width, surface->height, FB_COLOR(160, 40, 40));
}

static void cmd_gui_wp3_background(wl_surface_t *surface, fb_rect_t damage)
{
    (void)damage;
    wl_buf_fillrect(surface, 0, 0, surface->width, surface->height, FB_COLOR(40, 140, 60));
}

static void cmd_gui_wp4_background(wl_surface_t *surface, fb_rect_t damage)
{
    (void)damage;
    wl_buf_fillrect(surface, 0, 0, surface->width, surface->height, FB_COLOR(60, 60, 180));
}

static void cmd_gui(void)
{
    uart_puts("\r\nStarting OS/2 Workplace Shell...\r\n");
    fb_init();
    wl_compositor_init(fb_get_width(), fb_get_height());

    /* Real key events from the display window (Cmd+Escape) - separate
     * from this UART shell's own input; safely a no-op if the VM wasn't
     * started with -device virtio-keyboard-device. */
    vinput_init();

    /* Workplace 1: the original desktop. */
    wl_surface_t *win1 = xdg_toplevel_create(200, 100, 400, 300, "OS/2 System");
    wl_surface_t *win2 = xdg_toplevel_create(300, 200, 350, 250, "Drive C:");
    wl_surface_t *win_screen = xdg_toplevel_create(150, 420, 320, 220, "Screen");
    wl_surface_set_draw_callback(win1, cmd_gui_flat_background);
    wl_surface_set_draw_callback(win2, cmd_gui_flat_background);
    wl_surface_set_draw_callback(win_screen, draw_framebuffer_content);

    /* Workplaces 2-4: one demo window each. */
    wl_compositor_set_workplace(1);
    wl_surface_t *win_wp2 = xdg_toplevel_create(120, 100, 400, 260, "Workplace 2");
    wl_surface_set_draw_callback(win_wp2, cmd_gui_wp2_background);

    wl_compositor_set_workplace(2);
    wl_surface_t *win_wp3 = xdg_toplevel_create(120, 100, 400, 260, "Workplace 3");
    wl_surface_set_draw_callback(win_wp3, cmd_gui_wp3_background);

    wl_compositor_set_workplace(3);
    wl_surface_t *win_wp4 = xdg_toplevel_create(120, 100, 400, 260, "Workplace 4");
    wl_surface_set_draw_callback(win_wp4, cmd_gui_wp4_background);

    wl_compositor_set_workplace(0); /* clears, redamages, and flushes workplace 1 into view */
    uart_puts("GUI started! Press Cmd+Escape in the display window to cycle workplaces 1-4.\r\n");
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

/* ── BASIC Interpreter ───────────────────────────────────────────────────── */
/* OLD SIMPLE BASIC - REPLACED WITH FULL QUICKBASIC INTERPRETER FROM basic.c

#define MAX_BASIC_LINES 100
#define MAX_BASIC_VARS  26
#define MAX_LINE_LEN    256

typedef struct {
    int line_num;
    char code[MAX_LINE_LEN];
} basic_line_t;

static basic_line_t basic_program[MAX_BASIC_LINES];
static int basic_line_count = 0;
static int basic_vars[MAX_BASIC_VARS];
static int basic_pc = 0;

static void basic_init(void) { ... }
static int basic_find_line(int line_num) { ... }
static void basic_insert_line(int line_num, const char *code) { ... }
static int basic_eval_expr(const char *expr) { ... }
static void basic_exec_line(const char *line) { ... }

END OF OLD BASIC CODE */

static void cmd_basic(void)
{
    /* Call the full QuickBASIC interpreter from basic.c */
    basic_run();
}

/* ── Other Commands (VER, HELP, DIR, etc.) ──────────────────────────────── */

static void cmd_ver(void)
{
    uart_puts("\r\n");
    uart_puts("OS/2 Warp Version 4.52 (ARM64 Edition)\r\n");
    uart_puts("Internal revision: build " OS2WARP_REVISION " (" OS2WARP_BUILD_DATE ")\r\n");
    uart_puts("Kernel: 4.52.001\r\n");
    uart_puts("Platform: ARM64 Cortex-A72\r\n");
    uart_puts("Target: QEMU virt machine\r\n");
    uart_puts("\r\n");
}

static void cmd_help(void)
{
    uart_puts("\r\n");
    uart_puts("Available commands:\r\n");
    uart_puts("  VER       - Display OS version\r\n");
    uart_puts("  DIR       - List directory contents\r\n");
    uart_puts("  TYPE      - Display file contents\r\n");
    uart_puts("  COPY      - Copy files (wildcards; into a directory)\r\n");
    uart_puts("  MOVE      - Move files (rename, or copy across volumes)\r\n");
    uart_puts("  TREE      - Directory tree [path] [/F lists files]\r\n");
    uart_puts("  FIND      - Lines containing text [/I /C /N /V] \"text\" files\r\n");
    uart_puts("  MORE      - Show a file a screen at a time\r\n");
    uart_puts("  DATE      - Show the date (TIME: the time)\r\n");
    uart_puts("  COMP      - Byte-compare two files\r\n");
    uart_puts("  FC        - Line-compare two files\r\n");
    uart_puts("  DEL       - Delete a file (alias: ERASE)\r\n");
    uart_puts("  REN       - Rename a file (alias: RENAME)\r\n");
    uart_puts("  MD        - Create a directory (alias: MKDIR)\r\n");
    uart_puts("  RD        - Remove a directory (alias: RMDIR)\r\n");
    uart_puts("  CD        - Show/change current directory (alias: CHDIR)\r\n");
    uart_puts("  VOL       - Volume label and file system [path]\r\n");
    uart_puts("  ATTRIB    - Display file attributes\r\n");
    uart_puts("  CHKDSK    - Volume size and free space [path]\r\n");
    uart_puts("  FORMAT    - (not available: volumes are made on the host)\r\n");
    uart_puts("  ECHO      - Display message\r\n");
    uart_puts("  CLS       - Clear screen\r\n");
    uart_puts("  DOSKEY    - Enable command history/macros (arrows, F7/F8/F9,\r\n");
    uart_puts("              /HISTORY, /MACROS, name=text)\r\n");
    uart_puts("  MEM       - Display memory information\r\n");
    uart_puts("  USB       - List USB devices (USBD)\r\n");
    uart_puts("  MOUNT     - MOUNT dev \\DIR: an HPFS/FAT volume (e.g. usb0p1) as the next drive\r\n");
    uart_puts("  D:        - Change to drive D: (C: the system, D:, E:, ... mounted volumes)\r\n");
    uart_puts("  DESKTOP   - The graphical desktop [keyboard layout [variant]]\r\n");
    uart_puts("  SHUTDOWN  - Close everything, unmount the drives, power off\r\n");
    uart_puts("  UMOUNT    - UMOUNT \\DIR: unmount it\r\n");
    uart_puts("  IFS       - List loaded filesystem drivers\r\n");
    uart_puts("  RUN       - Load and run an OS/2 LX .EXE (e.g. RUN CMD.EXE)\r\n");
    uart_puts("  PING      - Send echo requests to a host (PING 10.0.2.2, PING example.com)\r\n");
    uart_puts("  IFCONFIG  - The network interface: address, gateway, name servers\r\n");
    uart_puts("  HOST      - Look up a host name (DNS)\r\n");
#ifdef LX_HAS_TEST_PAYLOAD
    uart_puts("  RUNTEST   - Run the embedded HELLO.EXE loader self-test\r\n");
#endif
    uart_puts("  BASIC     - Start BASIC interpreter\r\n");
    uart_puts("  AI        - On-box assistant (AI STATUS/STATS/TIP/HELP)\r\n");
    uart_puts("  DEBUG     - DEBUG.COM-style memory monitor (type ? inside it)\r\n");
    uart_puts("  VM        - EL2 hypervisor (VM STATUS, VM TEST runs a hello guest)\r\n");
    uart_puts("  EXIT      - Exit to idle loop\r\n");
    uart_puts("  HELP      - Display this help\r\n");
    uart_puts("\r\n");
}

static int ramfs_ifs_init(const char *params) { (void)params; return 0; }

/* MEM: the kernel heap (a bump allocator) and the page-frame pool that
 * Linux programs, the RAM disk and kernel stacks allocate from. */
extern void mem_get_stats(uintptr_t *used, uintptr_t *total);
unsigned long mmu_frames_free_count(void);
unsigned long mmu_frames_total_count(void);

static void cmd_mem(void)
{
    uintptr_t used, total;
    mem_get_stats(&used, &total);
    unsigned long ft = mmu_frames_total_count(), ff = mmu_frames_free_count();
    unsigned long rd = ramfs_frames();
    kprintf("\nMemory:\n");
    kprintf("  Kernel heap  %8u KB of %u KB used (at 0x%x)\n",
            (unsigned)(used / 1024), (unsigned)(total / 1024), (unsigned)HEAP_START);
    kprintf("  Page frames  %8u KB of %u KB free (4 KB pages)\n", (unsigned)(ff * 4), (unsigned)(ft * 4));
    kprintf("    RAM disk   %8u KB\n", (unsigned)(rd * 4));
    kprintf("    other use  %8u KB (programs, stacks, buffers)\n",
            (unsigned)((ft - ff > rd ? ft - ff - rd : 0) * 4));
    kprintf("\n");
}

static void cmd_ifs(void)
{
    uart_puts("\r\n");
    uart_puts("Loaded IFS drivers:\r\n");
    uart_puts("\r\n");
    ifs_list_drivers();
    uart_puts("\r\n");
}

#ifdef LX_HAS_TEST_PAYLOAD
extern uint8_t lx_test_hello_start[];
extern uint8_t lx_test_hello_end[];

static void cmd_runtest(void)
{
    uart_puts("\r\n[LXTEST] Running embedded HELLO.EXE via the LX loader...\r\n");
    uint32_t size = (uint32_t)(lx_test_hello_end - lx_test_hello_start);
    int rc = lx_exec_image(lx_test_hello_start, size);
    uart_puts("[LXTEST] HELLO.EXE ");
    if (rc < 0) {
        uart_puts("failed to load\r\n");
    } else {
        uart_puts("returned ");
        uart_puthex((ULONG)rc);
        uart_puts("\r\n");
    }
}
#endif

extern void DosResolvePath(const char *in, char *out, uint32_t out_size);

static void cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: RUN <path.EXE>\r\n");
        return;
    }

    uart_puts("\r\n");
    /* As a process (kernel/src/linux: its own address space, at EL0) - a
     * program that fails ends alone; the kernel is only the fallback. */
    char resolved[VFS_PATH_MAX];
    DosResolvePath(argv[1], resolved, sizeof resolved);
    vfs_stat_t st;
    if (vfs_stat(resolved, &st) == VFS_OK) {
        argv[1] = resolved;
        int status = linux_run(resolved, argc - 1, argv + 1);
        if (status >= 0) {
            if (status != 0) kprintf("\n[LX] %s ended with status %d\n", resolved, status);
            return;
        }
    }
    int rc = lx_exec_file(argv[1]);

    /* If the loaded program was a normal function that returned (e.g.
     * HELLO.EXE), report its result. If it was a shell like CMD.EXE that
     * never returns, execution never reaches here. */
    uart_puts("\r\n[LX] ");
    uart_puts(argv[1]);
    if (rc < 0) {
        uart_puts(": load failed\r\n");
    } else {
        uart_puts(": returned ");
        uart_puthex((ULONG)rc);
        uart_puts("\r\n");
    }
}

/* Wall clock for filesystem timestamps: QEMU virt's PL031 RTC (data
 * register at 0x09010000, seconds since 1970). Used by fs/hpfs. */
uint32_t kernel_time_seconds(void)
{
    return *(volatile uint32_t *)0x09010000UL;
}

/* ── MOUNT / UMOUNT ──────────────────────────────────────────────────────────
 * Mount an HPFS or FAT16/FAT32 block device (e.g. a USB stick's
 * partition, usb0p1) after boot, reachable from the shell as \<DIR>:
 *   MOUNT                  list mounts and block devices
 *   MOUNT usb0p1 \USB      mount it at /USB
 *   UMOUNT \USB            unmount (refused while files are open there)
 */
#define USER_MOUNTS 4
static fat32_mount_t user_mnt[USER_MOUNTS];
static hpfs_mount_t  user_hpfs[USER_MOUNTS];
static char          user_mnt_path[USER_MOUNTS][32];
static blkdev_t     *user_mnt_dev[USER_MOUNTS];      /* what each slot mounted */

/* A block device went away (USB stick pulled): unmount what MOUNT put on
 * it. If files are still open there it stays mounted, failing cleanly,
 * until they're closed and it's UMOUNTed. */
static void user_mount_device_removed(blkdev_t *dev)
{
    for (int i = 0; i < USER_MOUNTS; i++) {
        if (!user_mnt_path[i][0] || user_mnt_dev[i] != dev) continue;
        if (vfs_unmount(user_mnt_path[i]) == VFS_OK) {
            kprintf("\n[MOUNT]  %s unmounted: %s was removed\n", user_mnt_path[i], dev->name);
            user_mnt_path[i][0] = '\0';
            user_mnt_dev[i] = 0;
        } else {
            kprintf("\n[MOUNT]  %s: %s was removed but files are still open there\n",
                    user_mnt_path[i], dev->name);
        }
    }
}

static int name_eq_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* "\usb", "/USB", "usb" -> "/USB" */
static int mount_path(const char *in, char *out, int size)
{
    int n = 0;
    if (*in != '/' && *in != '\\') out[n++] = '/';
    for (; *in && n < size - 1; in++) {
        char c = *in == '\\' ? '/' : *in;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[n++] = c;
    }
    while (n > 1 && out[n - 1] == '/') n--;           /* no trailing slash */
    out[n] = '\0';
    return n > 1 ? 0 : -1;                            /* not "/" itself */
}

/* The CHKDSK result, in OS/2's words. */
static void check_report(const char *dev, const hpfs_check_t *c, int problems)
{
    if (problems < 0) { kprintf("CHKDSK: %s couldn't be checked\n", dev); return; }
    kprintf("%u directories, %u files checked: ", c->dirs, c->files);
    if (!problems) kprintf("no problems found.\n");
    else kprintf("%d problem(s) found - nothing was changed.\n", problems);
    if (c->cleared) kprintf("The volume is marked clean%s.\n", c->now_rw ? " and is writable again" : "");
    else if (c->was_dirty && problems)
        kprintf("It stays read-only (marked as not shut down cleanly).\n");
}

/* AUTOCHECK (CONFIG.SYS IFS=...HPFS.IFS /AUTOCHECK:*): an HPFS volume left
 * dirty by an unclean shutdown is checked as it's mounted; when it's
 * sound it's marked clean and mounted read-write. */
static void autocheck(hpfs_mount_t *m)
{
    if (hpfs_is_rw(m) || !hpfs_is_dirty(m) || (m->dev->flags & BLKDEV_READONLY)) return;
    kprintf("\n%s was not shut down cleanly - checking it...\n", m->dev->name);
    hpfs_check_t c;
    int p = hpfs_check(m, 1, &c);
    check_report(m->dev->name, &c, p);
}

/* CHKDSK [path] [/F] on an HPFS volume mounted with MOUNT: the structure
 * check after DIR-style space figures (dos_chkdsk). /F: a dirty volume
 * that checks out is marked clean (and becomes writable). */
static void cmd_chkdsk_hpfs(int argc, char **argv)
{
    const char *arg = ".";
    int fix = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && (argv[i][1] | 0x20) == 'f') fix = 1;
        else arg = argv[i];
    }
    char resolved[VFS_PATH_MAX];
    extern void DosResolvePath(const char *in, char *out, uint32_t out_size);
    DosResolvePath(arg, resolved, sizeof resolved);
    for (int i = 0; i < USER_MOUNTS; i++) {
        const char *mp = user_mnt_path[i];
        if (!mp[0] || user_hpfs[i].dev != user_mnt_dev[i]) continue;
        int k = 0;
        while (mp[k] && resolved[k] && ((mp[k] | 0x20) == (resolved[k] | 0x20))) k++;
        if (mp[k] || (resolved[k] && resolved[k] != '/')) continue;
        kprintf("\nChecking %s (%s)...\n", user_mnt_dev[i]->name, mp);
        hpfs_check_t c;
        int p = hpfs_check(&user_hpfs[i], fix, &c);
        check_report(user_mnt_dev[i]->name, &c, p);
        if (!fix && c.was_dirty && p == 0)
            kprintf("CHKDSK /F marks it clean and writable.\n");
        return;
    }
}

static void cmd_mount(int argc, char **argv)
{
    if (argc == 1) {
        kprintf("\nMounted:\n");
        const char *mp, *fs;
        extern void DosDisplayPath(const char *vfs_path, char *out, uint32_t out_size);
        for (int i = 0; vfs_mount_info(i, &mp, &fs) == VFS_OK; i++) {
            if (vfs_mount_quiet(i)) continue;
            char drv[VFS_PATH_MAX];
            DosDisplayPath(mp, drv, sizeof drv);
            kprintf("  %-12s %-6s %s\n", drv, fs, mp);
        }
        kprintf("Block devices:\n");
        for (int i = 0; ; i++) {
            blkdev_t *d = blkdev_get_by_index(i);
            if (!d) break;
            unsigned mb = (unsigned)((d->num_sectors * (int64_t)d->sector_size) >> 20);
            kprintf("  %-12s %6u MB%s%s\n", d->name, mb,
                    (d->flags & BLKDEV_READONLY) ? "  read-only" : "",
                    (d->flags & BLKDEV_REMOVABLE) ? "  removable" : "");
        }
        return;
    }
    if (argc != 3) {
        kprintf("\nUsage: MOUNT [device directory]   e.g. MOUNT usb0p1 \\USB\n");
        return;
    }
    blkdev_t *dev = 0;
    for (int i = 0; (dev = blkdev_get_by_index(i)) != 0; i++)
        if (name_eq_nocase(dev->name, argv[1])) break;
    if (!dev) { kprintf("\nMOUNT: no block device '%s'\n", argv[1]); return; }

    char path[32];
    if (mount_path(argv[2], path, sizeof path) != 0) {
        kprintf("\nMOUNT: bad directory '%s'\n", argv[2]);
        return;
    }
    int slot = -1;
    for (int i = 0; i < USER_MOUNTS; i++)
        if (!user_mnt_path[i][0]) { slot = i; break; }
    if (slot < 0) { kprintf("\nMOUNT: too many mounts (max %d)\n", USER_MOUNTS); return; }

    int hpfs = hpfs_probe(dev), r;
    if (hpfs) {                                       /* HPFS: superblock at LSN 16 */
        kmemset(&user_hpfs[slot], 0, sizeof(user_hpfs[slot]));
        user_hpfs[slot].dev = dev;
        r = vfs_mount(path, &vfs_hpfs_fs, &user_hpfs[slot]);
        if (r == VFS_OK) autocheck(&user_hpfs[slot]);
    } else {
        kmemset(&user_mnt[slot], 0, sizeof(user_mnt[slot]));
        user_mnt[slot].dev = dev;
        r = vfs_mount(path, &vfs_fat32_fs, &user_mnt[slot]);
    }
    if (r != VFS_OK) {
        kprintf("\nMOUNT: %s at %s failed (%d)%s\n", dev->name, path, r,
                r == VFS_ERR_INVAL ? " - already mounted?" : " - not HPFS/FAT16/FAT32?");
        return;
    }
    kstrncpy(user_mnt_path[slot], path, sizeof user_mnt_path[slot]);
    user_mnt_dev[slot] = dev;
    blkdev_set_remove_hook(user_mount_device_removed);
    char drv[VFS_PATH_MAX];
    extern void DosDisplayPath(const char *vfs_path, char *out, uint32_t out_size);
    DosDisplayPath(path, drv, sizeof drv);
    kprintf("\n%s is drive %c: (%s%s)\n", dev->name, drv[0],
            hpfs ? (hpfs_is_rw(&user_hpfs[slot]) ? "HPFS" : "HPFS, read-only") : "FAT",
            (!hpfs && (dev->flags & BLKDEV_READONLY)) ? ", read-only" : "");
    /* UNIXROOT's drive (CONFIG.SYS SET UNIXROOT=D:): its tree, for the
     * programs (and the package database) */
    const char *ur = unixroot_drive();
    if (ur[1] == ':' && (ur[0] | 0x20) == (drv[0] | 0x20) && (drv[0] | 0x20) != 'c') {
        unixroot_create_tree();
        kprintf("%c: holds UNIXROOT (the programs' /usr, /etc, ...)\n", drv[0]);
    }
}

static void cmd_umount(int argc, char **argv)
{
    char path[32];
    if (argc != 2 || mount_path(argv[1], path, sizeof path) != 0) {
        kprintf("\nUsage: UMOUNT directory   e.g. UMOUNT \\USB\n");
        return;
    }
    int slot = -1;
    for (int i = 0; i < USER_MOUNTS; i++)
        if (user_mnt_path[i][0] && name_eq_nocase(user_mnt_path[i], path)) slot = i;
    if (slot < 0) { kprintf("\nUMOUNT: %s was not mounted with MOUNT\n", path); return; }
    int r = vfs_unmount(user_mnt_path[slot]);
    if (r == VFS_ERR_BUSY) { kprintf("\nUMOUNT: %s is busy (files open)\n", path); return; }
    if (r != VFS_OK)       { kprintf("\nUMOUNT: failed (%d)\n", r); return; }
    user_mnt_path[slot][0] = '\0';
    user_mnt_dev[slot] = 0;
    kprintf("\n%s unmounted\n", path);
}

/* SHUTDOWN: every volume unmounted cleanly (HPFS marked clean, FAT32's
 * free count written), then the machine powered off (PSCI SYSTEM_OFF).
 * From a desktop window the programs are ended first (os2_shell_line ->
 * linux_shutdown_request; cmd_desktop() comes here when they're gone). */
static void system_shutdown(void)
{
    kprintf("\nShutting down...\n");
    for (int i = 0; i < USER_MOUNTS; i++) {
        if (!user_mnt_path[i][0]) continue;
        char path[34];
        kstrncpy(path, user_mnt_path[i], sizeof path);
        char *uv[2] = { "UMOUNT", path };
        cmd_umount(2, uv);
    }
    kprintf("\nShutdown has completed. It is now safe to turn off your computer.\n");
    virtio_gpu_shutdown();                              /* (the host's display let go of first) */
    hyp_system_off();
    for (;;) __asm__ volatile("wfi");                 /* no PSCI: stay halted */
}

/* DESKTOP [layout [variant]]: the Wayland desktop - the Workplace Shell
 * (userland/wps, userland/rootfs/wps.sh), or dwl (dwl.sh) - from the USB stick - mounted at \h first if it isn't: the first
 * USB volume with dwl.sh on it. Back here when dwl quits (Alt+Shift+Q). */
static void cmd_desktop(int argc, char **argv)
{
    vfs_stat_t st;
    /* the framebuffer also works in plain memory: insist on a real display */
    if (!linux_display_ok() || !(virtio_gpu_available() || ramfb_available())) {
        kprintf("\nDESKTOP: no display (start QEMU with scripts/run-vm.sh --wayland)\n");
        return;
    }
    if (vfs_stat("/h/OS2/dwl.sh", &st) != VFS_OK) {
        if (vfs_stat("/h", &st) == VFS_OK) {
            kprintf("\nDESKTOP: \\h is mounted but has no desktop on it (see userland/make-stick.sh)\n");
            return;
        }
        /* partitions (usb0p1) before whole disks (usb0) */
        for (int pass = 0; pass < 2 && vfs_stat("/h/OS2/dwl.sh", &st) != VFS_OK; pass++)
            for (int i = 0; ; i++) {
                blkdev_t *d = blkdev_get_by_index(i);
                if (!d) break;
                const char *n = d->name;
                if ((n[0] | 0x20) != 'u' || (n[1] | 0x20) != 's' || (n[2] | 0x20) != 'b') continue;
                int part = 0;
                for (const char *c = n + 3; *c; c++) if (*c == 'p') part = 1;
                if (part != (pass == 0)) continue;
                char *mv[3] = { "MOUNT", (char *)n, "\\h" };
                cmd_mount(3, mv);
                if (vfs_stat("/h/OS2/dwl.sh", &st) == VFS_OK) break;
                if (vfs_stat("/h", &st) == VFS_OK) {        /* mounted, but not the stick */
                    char *uv[2] = { "UMOUNT", "\\h" };
                    cmd_umount(2, uv);
                }
            }
        if (vfs_stat("/h/OS2/dwl.sh", &st) != VFS_OK) {
            kprintf("\nDESKTOP: no USB stick with the desktop on it (userland/make-stick.sh;\n"
                    "         scripts/run-vm.sh --wayland attaches build/userland/stick.img)\n");
            return;
        }
    }
    /* the Workplace Shell (wps.sh) when the stick has it, else dwl */
    char *script = vfs_stat("/h/OS2/wps.sh", &st) == VFS_OK ? "/h/OS2/wps.sh" : "/h/OS2/dwl.sh";
    char *av[6] = { "/h/OS2/busybox", "sh", script, 0, 0, 0 };
    int ac = 3;
    for (int i = 1; i < argc && ac < 5; i++) av[ac++] = argv[i];
    for (char *c = av[3]; c && *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;   /* layouts: lower case */
    kprintf("\nStarting the desktop - Alt+O: OS/2 Command Prompt, Alt+P: programs,\n"
            "Ctrl+Esc: Window List, Alt+Shift+Q: back to this console\n");
    int status = linux_run("/h/OS2/busybox", ac, av);
    if (linux_shutdown_pending()) system_shutdown();
    if (status != 0) kprintf("\nThe desktop ended unexpectedly (status %d)\n", status);
}

/* ── Command dispatcher ──────────────────────────────────────────────────── */

static void execute_command(char *cmdline)
{
    char *argv[16];
    int argc = str_tokenize(cmdline, argv, 16);

    dos_cmd_rc = 0;
    if (argc == 0)
        return;

    str_toupper(argv[0]);
    ai_record_command(argv[0]);

    if (my_strcmp(argv[0], "VER") == 0) {
        cmd_ver();
    }
    else if (my_strcmp(argv[0], "HELP") == 0 || my_strcmp(argv[0], "?") == 0) {
        cmd_help();
    }
    else if (my_strcmp(argv[0], "CHKDSK") == 0) {
        dos_shell_dispatch(argc, argv);                /* size and free space */
        cmd_chkdsk_hpfs(argc, argv);                   /* HPFS: the structures */
    }
    else if (dos_shell_dispatch(argc, argv)) {
        /* handled by dos/ command dispatcher (DIR, ECHO, CLS, EXIT, TYPE,
         * COPY, DEL/ERASE, REN/RENAME, MD/MKDIR, RD/RMDIR, CD/CHDIR, VOL,
         * ATTRIB, CHKDSK, FORMAT) */
    }
    else if (my_strcmp(argv[0], "PING") == 0) {
        net_cmd_ping(argc, argv);
    }
    else if (my_strcmp(argv[0], "IFCONFIG") == 0 || my_strcmp(argv[0], "IPCONFIG") == 0) {
        net_cmd_ifconfig(argc, argv);
    }
    else if (my_strcmp(argv[0], "HOST") == 0) {
        net_cmd_host(argc, argv);
    }
    else if (my_strcmp(argv[0], "USB") == 0) {
        usbd_list();
    }
    else if (my_strcmp(argv[0], "RUNELF") == 0) {
        /* RUNELF \path\prog [args]: a static AArch64 Linux program */
        if (argc < 2) { kprintf("\nUsage: RUNELF \\path\\program [arguments]\n"); return; }
        char path[VFS_PATH_MAX];
        int n = 0;
        if (argv[1][0] != '\\' && argv[1][0] != '/') path[n++] = '/';
        for (const char *c = argv[1]; *c && n < VFS_PATH_MAX - 1; c++) path[n++] = *c == '\\' ? '/' : *c;
        path[n] = 0;
        argv[1] = path;
        kprintf("\n");
        int st = linux_run(path, argc - 1, argv + 1);
        if (st >= 0) kprintf("[PROG]   %s exited with status %d\n", path, st);
    }
    else if (my_strcmp(argv[0], "MOUNT") == 0) {
        cmd_mount(argc, argv);
    }
    else if (my_strcmp(argv[0], "DESKTOP") == 0) {
        cmd_desktop(argc, argv);
    }
    else if (my_strcmp(argv[0], "SHUTDOWN") == 0) {
        system_shutdown();
    }
    else if (my_strcmp(argv[0], "UMOUNT") == 0 || my_strcmp(argv[0], "UNMOUNT") == 0) {
        cmd_umount(argc, argv);
    }
    else if (my_strcmp(argv[0], "MEM") == 0) {
        cmd_mem();
    }
    else if (my_strcmp(argv[0], "IFS") == 0) {
        cmd_ifs();
    }
    else if (my_strcmp(argv[0], "RUN") == 0) {
        cmd_run(argc, argv);
    }
#ifdef LX_HAS_TEST_PAYLOAD
    else if (my_strcmp(argv[0], "RUNTEST") == 0) {
        cmd_runtest();
    }
#endif
    else if (my_strcmp(argv[0], "BASIC") == 0) {
        cmd_basic();
    }
    else if (my_strcmp(argv[0], "AI") == 0) {
        ai_run(argc, argv);
    }
    // In command handler:
    else if (my_strcmp(argv[0], "GUI") == 0) {
        cmd_gui();
    }
    else if (my_strcmp(argv[0], "VM") == 0) {
        if (argc > 1)
            str_toupper(argv[1]);
        cmd_vm(argc, argv);
    }
    else {
        uart_puts("\r\nBad command or file name: ");
        uart_puts(argv[0]);
        uart_puts("\r\nType HELP for list of commands\r\n");
        dos_cmd_rc = 1041;                             /* (SYS1041, as CMD.EXE) */
    }
}

/* ── The shell for Linux programs (/dev/os2cmd) ──────────────────────────
 * kernel/src/linux/linux.c runs command lines a Linux program writes to
 * /dev/os2cmd through here, with console output captured for it to read
 * back: the same interpreter, mounts and current directory as this shell,
 * in a terminal window on the Wayland desktop (userland/apps/os2cmd.c).
 * What reads the keyboard itself or takes over the machine can't run
 * from inside a Linux program; UMOUNT could pull files from under the
 * running programs (they demand-page their code from the volume). */
/* Each /dev/os2cmd session (window) has its own current directory, as
 * OS/2 sessions do: it starts in this shell's, and is swapped in around
 * each of its commands. */
APIRET DosQueryCurrentDir(char *buffer, uint32_t buffer_len);   /* os2api */
APIRET DosSetCurrentDir(const char *dirname);
static char os2_saved_cwd[VFS_PATH_MAX];

void os2_session_enter(char *cwd)
{
    if (DosQueryCurrentDir(os2_saved_cwd, sizeof os2_saved_cwd) != NO_ERROR)
        kstrncpy(os2_saved_cwd, "/", sizeof os2_saved_cwd);
    if (!cwd[0]) {
        kstrncpy(cwd, os2_saved_cwd, VFS_PATH_MAX);
    } else if (DosSetCurrentDir(cwd) != NO_ERROR) {      /* gone (unmounted?) */
        kstrncpy(cwd, "/", VFS_PATH_MAX);
        DosSetCurrentDir("/");
    }
}

void os2_session_leave(char *cwd)
{
    if (DosQueryCurrentDir(cwd, VFS_PATH_MAX) != NO_ERROR) kstrncpy(cwd, "/", VFS_PATH_MAX);
    DosSetCurrentDir(os2_saved_cwd);
}

void os2_shell_banner(void)
{
    uart_puts("\r\nOS/2 Command Prompt\r\n");
    uart_puts("Type HELP for the commands, EXIT to close the window.\r\n\r\n");
}

void os2_shell_prompt(void)
{
    char prompt[VFS_PATH_MAX + 8];
    dos_prompt(prompt, sizeof prompt);
    uart_puts(prompt);
}

void os2_shell_line(const char *line)
{
    static const char *const refused[] = {
        "RUN", "RUNELF", "RUNTEST", "BASIC", "DEBUG", "GUI", "VM", "MORE",
        "DOSKEY", "UMOUNT", "UNMOUNT", "EXIT", "DESKTOP",
    };
    char buffer[256], word[16];
    int n = 0;
    while (line[n] && line[n] != '\n' && line[n] != '\r' && n < (int)sizeof buffer - 1) {
        buffer[n] = line[n];
        n++;
    }
    buffer[n] = '\0';

    const char *p = buffer;
    while (*p == ' ' || *p == '\t') p++;
    int w = 0;
    while (p[w] && p[w] != ' ' && p[w] != '\t' && w < (int)sizeof word - 1) {
        word[w] = (p[w] >= 'a' && p[w] <= 'z') ? (char)(p[w] - 32) : p[w];
        w++;
    }
    word[w] = '\0';
    if (!w) return;
    if (my_strcmp(word, "SHUTDOWN") == 0) {          /* the programs first */
        uart_puts("\r\nShutting down...\r\n");
        linux_shutdown_request();
        return;
    }
    for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++)
        if (my_strcmp(word, refused[i]) == 0) {
            uart_puts("\r\n");
            uart_puts(word);
            if (my_strcmp(word, "MORE") == 0)
                uart_puts(": use TYPE - this window scrolls\r\n");
            else if (my_strcmp(word, "DOSKEY") == 0)
                uart_puts(": line editing and history are the terminal's here\r\n");
            else if (my_strcmp(word, "EXIT") == 0)
                uart_puts(": close the window (or press Ctrl+D)\r\n");
            else if (my_strcmp(word, "DESKTOP") == 0)
                uart_puts(": the desktop is running - Alt+Shift+Q closes it\r\n");
            else
                uart_puts(" is only available at the system console\r\n");
            dos_cmd_rc = 1;
            return;
        }
    execute_command(buffer);
}

/* ── Kernel shell ────────────────────────────────────────────────────────── */

static void print_banner(void)
{
    uart_puts("\r\n");
    uart_puts("  ___  ____    ______     _  _  __              \r\n");
    uart_puts(" / _ \\/ ___|  / /___ \\   | || |/ /___ _  __    \r\n");
    uart_puts("| | | \\___ \\ / /  __) |  | || '_// _ \\ \\/ /   \r\n");
    uart_puts("| |_| |___) / /  / __/   |__   _|  __/>  <    \r\n");
    uart_puts(" \\___/|____/_/  |_____|     |_|  \\___/_/\\_\\  \r\n");
    uart_puts("\r\n");
    uart_puts("  OS/2 Warp 4.52  -  ARM64 Bare-Metal Kernel\r\n");
    uart_puts("  Target : QEMU virt  |  CPU : Cortex-A72\r\n");
    uart_puts("  UART   : PL011 @ ");
    uart_puthex(0x09000000UL);
    uart_puts("\r\n\r\n");
}

/* Cmd+Escape (from the display window, not this serial console - see
 * gui/virtio_input.h) cycles workplaces regardless of what's happening
 * on the shell; dos_read_line() calls this whenever it's not blocked
 * reading a key, so it isn't stuck behind a keystroke while idle. */
/* USB mouse / tablet (drivers/usb/usbmouse.c) -> compositor pointer. */
static bool usb_pointer_poll(void)
{
    static int px, py;
    static unsigned pbuttons;
    usbmouse_event_t ev;
    bool changed = false;

    while (usbmouse_poll(&ev)) {
        int w = fb_get_width(), h = fb_get_height();
        if (w <= 0 || h <= 0) continue;
        if (ev.absolute) {
            px = (int)((long)ev.x * (w - 1) / USBMOUSE_ABS_MAX);
            py = (int)((long)ev.y * (h - 1) / USBMOUSE_ABS_MAX);
        } else {
            px += ev.dx;
            py += ev.dy;
        }
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= w) px = w - 1;
        if (py >= h) py = h - 1;
        wl_seat_pointer_motion(px, py);
        for (int b = 0; b < 2; b++)               /* compositor: left, right */
            if ((ev.buttons ^ pbuttons) & (1u << b))
                wl_seat_pointer_button(px, py, b, (ev.buttons >> b) & 1);
        pbuttons = ev.buttons;
        changed = true;
    }
    return changed;
}

int  uart_ready(void);          /* uart.c (its header clashes with the local helpers above) */
char uart_getc(void);
void uart_idle_wait(void);

static void kernel_shell_idle(void)
{
    usbd_daemon();          /* USB: completions, hot-plug */
    net_poll();             /* the network: frames, timers */
    if (usb_pointer_poll()) {
        wl_compositor_flush();
    }
    if (vinput_poll_workplace_hotkey()) {
        wl_compositor_cycle_workplace();
    }
    if (vinput_poll_pointer()) {
        wl_compositor_flush(); /* redraw so the cursor's new position is visible */
    }
    if (!uart_ready()) uart_idle_wait();   /* nothing typed: don't spin the CPU */
}

/* Boot straight into the desktop (CONFIG.SYS: SET RUNWORKPLACE): if
 * there's a display, wait up to 5 s for the USB stick to show up, then
 * DESKTOP. A key on the console meanwhile keeps the console. */
static void desktop_at_boot(void)
{
    if (!run_desktop_at_boot) return;
    if (!linux_display_ok() || !(virtio_gpu_available() || ramfb_available())) return;
    uart_puts("\r\nStarting the desktop - press a key here to stay at the console\r\n");
    for (int t = 0; t < 1000; t++) {                    /* 5 ms each */
        if (uart_ready()) { (void)uart_getc(); uart_puts("Staying at the console.\r\n"); return; }
        usbd_daemon();                                  /* USB enumeration, hot-plug */
        int stick = 0;
        for (int i = 0; ; i++) {
            blkdev_t *d = blkdev_get_by_index(i);
            if (!d) break;
            if ((d->name[0] | 0x20) == 'u' && (d->name[1] | 0x20) == 's' && (d->name[2] | 0x20) == 'b')
                stick = 1;
        }
        if (stick && t >= 100) break;                   /* (half a second to change your mind) */
        uart_idle_wait();
    }
    char *av[1] = { "DESKTOP" };
    cmd_desktop(1, av);
}

static void kernel_shell(void)
{
    char buffer[256];

    uart_puts("\r\n");
    uart_puts("OS/2 Command Prompt\r\n");
    uart_puts("Type HELP for available commands\r\n");
    uart_puts("Type IFS to list loaded filesystem drivers\r\n");
    uart_puts("Type BASIC to start BASIC interpreter\r\n");
    uart_puts("Type DESKTOP to start the graphical desktop\r\n");
    uart_puts("Type DOSKEY to enable command history/macros (see HELP)\r\n");
    uart_puts("\r\n");

    desktop_at_boot();

    for (;;) {
        char prompt[VFS_PATH_MAX + 8];
        dos_prompt(prompt, sizeof prompt);
        dos_read_line(prompt, buffer, sizeof(buffer), kernel_shell_idle);

        /* A DOSKEY macro using $T comes back as multiple '\n'-separated
         * commands - run each one in turn. */
        char *seg = buffer;
        while (*seg) {
            char *nl = strchr(seg, '\n');
            if (nl) *nl = '\0';
            if (*seg) execute_command(seg);
            if (!nl) break;
            seg = nl + 1;
        }
    }
}

/* ── Kernel entry point ──────────────────────────────────────────────────── */

extern void stack_protector_reseed(void);

void kernel_main(void)
{
    stack_protector_reseed();

    print_banner();

    uart_puts("[KERNEL] Subsystems initializing...\r\n");

    mem_init(HEAP_START, HEAP_SIZE);
    uart_puts("[MEM]    Allocator    : initialized\r\n");

    scheduler_init();
    uart_puts("[SCHED]  Scheduler    : initialized\r\n");

    if (hyp_init() == 0)
        uart_puts("[HYP]    EL2 hypervisor : ready (type VM TEST)\r\n");
    else if (hyp_boot_el == 2)
        uart_puts("[HYP]    EL2 hypervisor : bring-up FAILED\r\n");

    uart_puts("[IPC]    Semaphores   : stub\r\n");

    uart_puts("[VFS]    Initializing VFS and filesystem drivers...\r\n");
    vfs_init();  // ← Call vfs_init first

    uart_puts("[BLK]    Initializing block devices...\r\n");
    blk_init();


    /* ── IFS Loader Integration ─────────────────────────────────────────── */
    uart_puts("[IFS]    Initializing IFS loader...\r\n");
    ifs_loader_init();

    /* Register built-in filesystem drivers */
    uart_puts("[IFS]    Registering ext4 driver...\r\n");
    if (ifs_register_builtin("EXT4", ext4_init, ext4_get_fs()) == 0) {
        uart_puts("[IFS]    ext4 driver registered successfully\r\n");
    } else {
        uart_puts("[IFS]    ERROR: Failed to register ext4 driver\r\n");
    }

    uart_puts("[IFS]    Registering FAT driver...\r\n");
    if (ifs_register_builtin("FAT", fat32_init, &vfs_fat32_fs) == 0) {
        uart_puts("[IFS]    FAT driver registered successfully\r\n");
    } else {
        uart_puts("[IFS]    ERROR: Failed to register FAT driver\r\n");
    }

    uart_puts("[IFS]    Registering HPFS driver...\r\n");
    if (ifs_register_builtin("HPFS", hpfs_init, &vfs_hpfs_fs) == 0) {
        uart_puts("[IFS]    HPFS driver registered successfully\r\n");
    } else {
        uart_puts("[IFS]    ERROR: Failed to register HPFS driver\r\n");
    }

    uart_puts("[IFS]    Registering btrfs driver...\r\n");
    if (ifs_register_builtin("BTRFS", btrfs_init, btrfs_get_fs()) == 0) {
        uart_puts("[IFS]    btrfs driver registered successfully\r\n");
    } else {
        uart_puts("[IFS]    ERROR: Failed to register btrfs driver\r\n");
    }

    /* RAMFS has nothing to probe: Linux programs get it on /tmp. */
    if (ifs_register_builtin("RAMFS", ramfs_ifs_init, &vfs_ramfs) != 0)
        uart_puts("[IFS]    ERROR: Failed to register RAMFS driver\r\n");

    /* For now, manually initialize all three with default parameters; each
     * looks for its own block device and no-ops if it isn't present. FAT
     * takes the "BOOT" partition (/BOOT); ext4 and btrfs both try "C" (/)
     * and only the one whose superblock matches mounts it. */
    uart_puts("[IFS]    Loading drivers with default parameters...\r\n");
    fat32_init(NULL);
    ext4_init("/CACHE:1024");
    btrfs_init(NULL);
    hpfs_init("/CACHE:1024");   /* any HPFS volume -> /HPFS */

    /* ── CONFIG.SYS / UNIXROOT ────────────────────────────────────────────
     * Root is now mounted at "/" (see ext4_init/btrfs_init above - that
     * mount IS drive C:, per unixroot.h). Load CONFIG.SYS from it if
     * present, so "SET UNIXROOT=..." / "SET USER=..." there can steer the
     * Unix-style directory tree unixroot_create_tree() lays down. */
    {
        extern void *mem_alloc(size_t size);
        extern void mem_free(void *ptr);

        uart_puts("[CFG]    Loading CONFIG.SYS...\r\n");

        vfs_stat_t cfg_st;
        /* ext4/btrfs names are case-sensitive; try the OS/2 spelling first */
        static const char *const cfg_paths[] = {
            "/CONFIG.SYS", "/config.sys", "/etc/config.sys", NULL
        };
        const char *cfg_path = NULL;
        for (int i = 0; cfg_paths[i]; i++) {
            if (vfs_stat(cfg_paths[i], &cfg_st) == VFS_OK) {
                cfg_path = cfg_paths[i];
                break;
            }
        }

        char *cfg_text = NULL;
        if (cfg_path && cfg_st.size > 0 && cfg_st.size < 65536) {
            int cfg_fd = vfs_open(cfg_path, O_RDONLY);
            if (cfg_fd >= 0) {
                cfg_text = (char *)mem_alloc(cfg_st.size + 1);
                if (cfg_text) {
                    int n = vfs_read(cfg_fd, cfg_text, cfg_st.size);
                    cfg_text[(n > 0) ? (uint32_t)n : 0u] = '\0';
                }
                vfs_close(cfg_fd);
            }
        }

        if (cfg_text) {
            kprintf("[CFG]    CONFIG.SYS loaded from %s\n", cfg_path);
        } else {
            kprintf("[CFG]    CONFIG.SYS not found; using UNIXROOT defaults\n");
        }

        /* SET RUNWORKPLACE=DESKTOP (or no such line): the desktop starts
         * at boot when there's a display; CMD (anything else): the console */
        char rw[32];
        if (cfg_text && config_sys_value(cfg_text, "RUNWORKPLACE", rw, sizeof rw) == 0) {
            const char *leaf = rw;                 /* C:\OS2\PMSHELL.EXE -> PMSHELL.EXE */
            for (const char *c = rw; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') leaf = c + 1;
            run_desktop_at_boot = !(name_eq_nocase(leaf, "CMD") || name_eq_nocase(leaf, "CMD.EXE"));
        }

        unixroot_configure(cfg_text);
        kprintf("[UNIXROOT] Drive: %s\n", unixroot_drive());
        unixroot_create_tree();

        /* USB stack: BASEDEV=/DEVICE= lines (defaults without CONFIG.SYS) */
        usb_init(cfg_text);

        if (cfg_text) mem_free(cfg_text);
    }

    /* The network: a virtio-net card, lwIP, an address from DHCP */
    net_init();

    uart_puts("\r\n[KERNEL] Boot complete.\r\n");
    uart_puts("[VFS]    Filesystem ready for mounting\r\n");
    uart_puts("[IFS]    Type 'IFS' command to list loaded drivers\r\n");
    uart_puts("\r\n");

    kbd_init();

    kernel_shell();
}