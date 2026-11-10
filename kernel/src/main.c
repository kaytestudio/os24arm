#include "os2.h"
#include "keyboard.h"
#include "vfs.h"
#include "blkdev.h"
#include "ifs_loader.h"
#include "../../fs/ext4/ext4.h"
#include "../../fs/btrfs/btrfs.h"
#include "fat32.h"
#include "basic.h"
#include "gui/framebuffer.h"
#include "gui/wayland.h"
#include "gui/virtio_input.h"
#include "dos.h"
#include "lx_loader.h"
#include "ai.h"
#include "string.h"
#include "unixroot.h"
#include "kprintf.h"
#include "hyp.h"


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

static void uart_putc(char c)
{
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
    uart_puts("Internal revision: Bare Metal Build\r\n");
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
    uart_puts("  COPY      - Copy a file\r\n");
    uart_puts("  COMP      - Byte-compare two files\r\n");
    uart_puts("  FC        - Line-compare two files\r\n");
    uart_puts("  DEL       - Delete a file (alias: ERASE)\r\n");
    uart_puts("  REN       - Rename a file (alias: RENAME)\r\n");
    uart_puts("  MD        - Create a directory (alias: MKDIR)\r\n");
    uart_puts("  RD        - Remove a directory (alias: RMDIR)\r\n");
    uart_puts("  CD        - Show/change current directory (alias: CHDIR)\r\n");
    uart_puts("  VOL       - Display volume label\r\n");
    uart_puts("  ATTRIB    - Display file attributes\r\n");
    uart_puts("  CHKDSK    - Check disk (not supported, read-only drivers)\r\n");
    uart_puts("  FORMAT    - Format disk (not supported, read-only drivers)\r\n");
    uart_puts("  ECHO      - Display message\r\n");
    uart_puts("  CLS       - Clear screen\r\n");
    uart_puts("  DOSKEY    - Enable command history/macros (arrows, F7/F8/F9,\r\n");
    uart_puts("              /HISTORY, /MACROS, name=text)\r\n");
    uart_puts("  MEM       - Display memory information\r\n");
    uart_puts("  IFS       - List loaded filesystem drivers\r\n");
    uart_puts("  RUN       - Load and run an OS/2 LX .EXE (e.g. RUN CMD.EXE)\r\n");
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

static void cmd_mem(void)
{
    uart_puts("\r\n");
    uart_puts("Memory Information:\r\n");
    uart_puts("  Heap start: ");
    uart_puthex(HEAP_START);
    uart_puts("\r\n");
    uart_puts("  Heap size:  ");
    uart_puthex(HEAP_SIZE);
    uart_puts(" (8 MB)\r\n");
    uart_puts("  Status:     Active\r\n");
    uart_puts("\r\n");
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

static void cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: RUN <path.EXE>\r\n");
        return;
    }

    uart_puts("\r\n");
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

/* ── Command dispatcher ──────────────────────────────────────────────────── */

static void execute_command(char *cmdline)
{
    char *argv[16];
    int argc = str_tokenize(cmdline, argv, 16);

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
    else if (dos_shell_dispatch(argc, argv)) {
        /* handled by dos/ command dispatcher (DIR, ECHO, CLS, EXIT, TYPE,
         * COPY, DEL/ERASE, REN/RENAME, MD/MKDIR, RD/RMDIR, CD/CHDIR, VOL,
         * ATTRIB, CHKDSK, FORMAT) */
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
    }
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
static void kernel_shell_idle(void)
{
    if (vinput_poll_workplace_hotkey()) {
        wl_compositor_cycle_workplace();
    }
    if (vinput_poll_pointer()) {
        wl_compositor_flush(); /* redraw so the cursor's new position is visible */
    }
}

static void kernel_shell(void)
{
    char buffer[256];

    uart_puts("\r\n");
    uart_puts("OS/2 Command Prompt\r\n");
    uart_puts("Type HELP for available commands\r\n");
    uart_puts("Type IFS to list loaded filesystem drivers\r\n");
    uart_puts("Type BASIC to start BASIC interpreter\r\n");
    uart_puts("Type DOSKEY to enable command history/macros (see HELP)\r\n");
    uart_puts("\r\n");

    for (;;) {
        dos_read_line("[C:\\]> ", buffer, sizeof(buffer), kernel_shell_idle);

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
    uart_puts("DEBUG: After mem_init\r\n");  // ← Add these

    scheduler_init();
    uart_puts("[SCHED]  Scheduler    : initialized\r\n");

    if (hyp_init() == 0)
        uart_puts("[HYP]    EL2 hypervisor : ready (type VM TEST)\r\n");
    else if (hyp_boot_el == 2)
        uart_puts("[HYP]    EL2 hypervisor : bring-up FAILED\r\n");
    uart_puts("DEBUG: After scheduler_init\r\n");  // ← Add these

    uart_puts("[IPC]    Semaphores   : stub\r\n");
    uart_puts("DEBUG: Before vfs_init\r\n");  // ← Add these

    uart_puts("[VFS]    Initializing VFS and filesystem drivers...\r\n");
    vfs_init();  // ← Call vfs_init first
    uart_puts("DEBUG: After vfs_init - MADE IT HERE!\r\n");  // ← Then print debug

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

    uart_puts("[IFS]    Registering btrfs driver...\r\n");
    if (ifs_register_builtin("BTRFS", btrfs_init, btrfs_get_fs()) == 0) {
        uart_puts("[IFS]    btrfs driver registered successfully\r\n");
    } else {
        uart_puts("[IFS]    ERROR: Failed to register btrfs driver\r\n");
    }

    /* For now, manually initialize all three with default parameters; each
     * looks for its own block device and no-ops if it isn't present. FAT
     * takes the "BOOT" partition (/BOOT); ext4 and btrfs both try "C" (/)
     * and only the one whose superblock matches mounts it. */
    uart_puts("[IFS]    Loading drivers with default parameters...\r\n");
    fat32_init(NULL);
    ext4_init("/CACHE:1024");
    btrfs_init(NULL);

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

        unixroot_configure(cfg_text);
        kprintf("[UNIXROOT] Drive: %s\n", unixroot_drive());
        unixroot_create_tree();

        if (cfg_text) mem_free(cfg_text);
    }

    uart_puts("\r\n[KERNEL] Boot complete.\r\n");
    uart_puts("[VFS]    Filesystem ready for mounting\r\n");
    uart_puts("[IFS]    Type 'IFS' command to list loaded drivers\r\n");
    uart_puts("\r\n");

    kbd_init();

    kernel_shell();
}