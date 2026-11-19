/* ============================================================================
 * kernel/shell.c  —  OS/2 Warp 4.52-style command shell
 *
 * Emulates a DOS/OS2 command prompt with colour, history, and built-in cmds.
 * ========================================================================== */

#include "shell.h"
#include "kio.h"
#include "uart.h"
#include "memory.h"
#include "basic.h"
#include "types.h"

#define MAX_CMD_LEN   256
#define MAX_ARGS      32
#define HISTORY_LEN   16

/* Command history */
static char history[HISTORY_LEN][MAX_CMD_LEN];
static int  hist_count = 0;
static int  hist_head  = 0;

static void hist_push(const char *cmd)
{
    if (cmd[0] == '\0') return;
    kstrncpy(history[hist_head], cmd, MAX_CMD_LEN);
    hist_head = (hist_head + 1) % HISTORY_LEN;
    if (hist_count < HISTORY_LEN) hist_count++;
}

static int ends_with_cmd(const char* s)
{
    size_t len = strlen(s);

    if (len < 4)
        return 0;

    return (s[len-4] == '.' &&
            s[len-3] == 'c' &&
            s[len-2] == 'm' &&
            s[len-1] == 'd');
}


/* Current directory, until shell commands use the VFS */
static char cwd[128] = "C:\\OS2\\";

/* Append an uppercased path component without depending on libc strcat. */
static void cwd_append(const char *s)
{
    char *p = cwd + kstrlen(cwd);
    while (*s) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') c -= 32; /* uppercase */
        *p++ = c;
    }
    *p = '\0';
}

/* Split on spaces; quoted arguments and escapes are not supported. */
static int parse_args(char *line, char *argv[])
{
    int argc = 0;
    while (*line) {
        while (*line == ' ') line++;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = '\0';
        if (argc >= MAX_ARGS) break;
    }
    return argc;
}

/* Built-in command implementations */

static void cmd_help(void)
{
    kprintf(ANSI_CYAN
    "┌──────────────────────────────────────────────────────┐\n"
    "│      OS/2 Warp 4.52 ARM64 — Built-in Commands        │\n"
    "└──────────────────────────────────────────────────────┘\n"
    ANSI_RESET);
    kprintf("  %-12s  %s\n", "HELP",      "Show this help");
    kprintf("  %-12s  %s\n", "CLS",       "Clear screen");
    kprintf("  %-12s  %s\n", "VER",       "Show OS version");
    kprintf("  %-12s  %s\n", "DIR",       "List directory contents");
    kprintf("  %-12s  %s\n", "CD <path>", "Change directory");
    kprintf("  %-12s  %s\n", "MD <dir>",  "Make directory");
    kprintf("  %-12s  %s\n", "TYPE <f>",  "Display file contents");
    kprintf("  %-12s  %s\n", "ECHO <..>", "Echo text to screen");
    kprintf("  %-12s  %s\n", "SET",       "Show environment variables");
    kprintf("  %-12s  %s\n", "MEM",       "Show memory usage");
    kprintf("  %-12s  %s\n", "UNAME",     "Show system information");
    kprintf("  %-12s  %s\n", "BASIC",     "Start BASIC interpreter");
    kprintf("  %-12s  %s\n", "HISTORY",   "Show command history");
    kprintf("  %-12s  %s\n", "DATE",      "Show system date/time");
    kprintf("  %-12s  %s\n", "SHUTDOWN",  "Shutdown the system");
    kprintf("\n");
}

static void cmd_cls(void)
{
    /* ANSI: clear screen + move cursor home */
    uart_puts("\033[2J\033[H");
}

static void cmd_ver(void)
{
    kprintf(ANSI_BOLD ANSI_CYAN
    "\n"
    "  ██████╗ ███████╗     ██████╗      █████╗ ██████╗ ███╗   ███╗\n"
    "  ██╔══██╗██╔════╝    ╚════██╗    ██╔══██╗██╔══██╗████╗ ████║\n"
    "  ██║  ██║███████╗     █████╔╝    ███████║██████╔╝██╔████╔██║\n"
    "  ██║  ██║╚════██║    ██╔═══╝     ██╔══██║██╔══██╗██║╚██╔╝██║\n"
    "  ██████╔╝███████║    ███████╗    ██║  ██║██║  ██║██║ ╚═╝ ██║\n"
    "  ╚═════╝ ╚══════╝    ╚══════╝    ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝     ╚═╝\n"
    ANSI_RESET "\n");
    kprintf("  " ANSI_WHITE "OS/2 Warp Version 4.52" ANSI_RESET
            " for ARM64 (Apple Silicon / QEMU)\n");
    kprintf("  Architecture : AArch64  |  CPU: cortex-a72\n");
    kprintf("  Build        : " __DATE__ "  " __TIME__ "\n");
    kprintf("  Copyright    : Handbuilt bare-metal kernel\n\n");
}

static void cmd_dir(void)
{
    kprintf(" Directory of " ANSI_YELLOW "%s" ANSI_RESET "\n\n", cwd);
    /* This demo listing is static and does not read the mounted filesystem. */
    const char *entries[] = {
        ".", "..", "SYSTEM", "OS2BOOT", "CONFIG.SYS",
        "AUTOEXEC.BAT", "BASIC.EXE", "KERNEL.EXE"
    };
    bool is_dir[] = {true, true, true, false, false, false, false, false};

    for (size_t i = 0; i < ARRAY_SIZE(entries); i++) {
        if (is_dir[i]) {
            kprintf("  " ANSI_CYAN "<DIR>" ANSI_RESET "  %s\n", entries[i]);
        } else {
            kprintf("         %s\n", entries[i]);
        }
    }
    kprintf("\n  %d file(s)    %d dir(s)\n\n",
            (int)(ARRAY_SIZE(entries) - 3),
            3);
}

static void cmd_cd(int argc, char *argv[])
{
    if (argc < 2) {
        kprintf("%s\n", cwd);
        return;
    }
    /* Simulate navigation */
    if (kstrcmp(argv[1], "..") == 0) {
        /* Go up */
        size_t len = kstrlen(cwd);
        if (len > 3) { /* not root "C:\" */
            cwd[len-1] = '\0'; /* remove trailing \ */
            char *last = cwd;
            while (kstrchr(last+1, '\\')) last = kstrchr(last+1, '\\');
            *(last+1) = '\0';
        }
    } else if (kstrcmp(argv[1], "\\") == 0) {
        kstrcpy(cwd, "C:\\");
    } else {
        /* Append subdirectory */
        size_t clen = kstrlen(cwd);
        size_t alen = kstrlen(argv[1]);
        if (clen + alen + 2 < sizeof(cwd)) {
            cwd_append(argv[1]);
            cwd_append("\\");
        }
    }
}


static void cmd_echo(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        kprintf("%s", argv[i]);
        if (i < argc-1) kprintf(" ");
    }
    kprintf("\n");
}

static void cmd_set(void)
{
    kprintf("COMSPEC=C:\\OS2\\CMD.EXE\n");
    kprintf("PATH=C:\\OS2;C:\\OS2\\SYSTEM\n");
    kprintf("PROMPT=$P$G\n");
    kprintf("LIBPATH=C:\\OS2\\DLL\n");
    kprintf("DPATH=C:\\OS2\n");
    kprintf("ARCH=ARM64\n");
    kprintf("PROCESSOR=CORTEX-A72\n");
}

static void cmd_uname(void)
{
    kprintf("OS/2 Warp 4.52 arm64 ARM64 OS2/2-ARM64\n");
}

static void cmd_date(void)
{
    /* Without an RTC, report fixed fallback values. */
    kprintf("Current date: SUN 02-15-2026\n");
    kprintf("Current time: 00:00:00.00\n");
    kprintf("(No real-time clock in bare-metal build)\n");
}

static void cmd_history(void)
{
    int start = (hist_head - hist_count + HISTORY_LEN) % HISTORY_LEN;
    for (int i = 0; i < hist_count; i++) {
        int idx = (start + i) % HISTORY_LEN;
        kprintf("  %3d  %s\n", i+1, history[idx]);
    }
}

static void cmd_shutdown(void)
{
    kprintf(ANSI_RED
    "\n  System halting. It is now safe to power off.\n\n"
    ANSI_RESET);
    /* PSCI SYSTEM_OFF (0x84000008) via HVC — works on QEMU virt */
    __asm__ volatile(
        "mov x0, #0x84000000\n"
        "add x0, x0, #0x08\n"
        "hvc #0\n"
    );
    /* If PSCI is not available, spin forever */
    while (1) {
        __asm__ volatile("wfe");
    }
}

static void cmd_type(int argc, char *argv[])
{
    if (argc < 2) {
        kprintf("Usage: TYPE <filename>\n");
        return;
    }
    /* Simulated files */
    if (kstrcmp(argv[1], "CONFIG.SYS") == 0) {
        kprintf("PROTSHELL=C:\\OS2\\PMSHELL.EXE\n");
        kprintf("LIBPATH=C:\\OS2\\DLL\n");
        kprintf("SET PATH=C:\\OS2;C:\\OS2\\SYSTEM\n");
        kprintf("BUFFERS=40,0\n");
        kprintf("IFS=C:\\OS2\\HPFS.IFS\n");
    } else if (kstrcmp(argv[1], "AUTOEXEC.BAT") == 0) {
        kprintf("@ECHO OFF\n");
        kprintf("PROMPT $P$G\n");
        kprintf("PATH C:\\OS2;C:\\OS2\\SYSTEM\n");
    } else {
        kprintf("File not found: %s\n", argv[1]);
    }
}

static void cmd_md(int argc, char *argv[])
{
    if (argc < 2) { kprintf("Usage: MD <dirname>\n"); return; }
    kprintf("Directory %s created.\n", argv[1]);
}

/* ── Prompt drawing ──────────────────────────────────────────────────────── */

static void draw_prompt(void)
{
    kprintf(ANSI_GREEN "[OS2ARM]" ANSI_RESET
            ANSI_YELLOW " %s" ANSI_RESET
            ANSI_WHITE  "> " ANSI_RESET, cwd);
}

/* ── Main shell loop ─────────────────────────────────────────────────────── */

void shell_run(void)
{
    cmd_cls();
    cmd_ver();

    kprintf("Type " ANSI_CYAN "HELP" ANSI_RESET " for available commands.\n\n");

    static char line[MAX_CMD_LEN];
    static char *argv[MAX_ARGS];

    for (;;) {
        draw_prompt();
        kgets(line, sizeof(line));

        if (line[0] == '\0') continue;
        hist_push(line);

        /* Uppercase the command token for case-insensitive matching */
        for (size_t i = 0; line[i] && line[i] != ' '; i++) {
            if (line[i] >= 'a' && line[i] <= 'z') line[i] -= 32;
        }

        int argc = parse_args(line, argv);
        if (argc == 0) continue;

        const char *cmd = argv[0];

        if      (kstrcmp(cmd, "HELP") == 0)     cmd_help();
        else if (kstrcmp(cmd, "CLS")  == 0)     cmd_cls();
        else if (kstrcmp(cmd, "VER")  == 0)     cmd_ver();
        else if (kstrcmp(cmd, "DIR")  == 0)     cmd_dir();
        else if (kstrcmp(cmd, "CD")   == 0)     cmd_cd(argc, argv);
        else if (kstrcmp(cmd, "ECHO") == 0)     cmd_echo(argc, argv);
        else if (kstrcmp(cmd, "SET")  == 0)     cmd_set();
        else if (kstrcmp(cmd, "UNAME")== 0)     cmd_uname();
        else if (kstrcmp(cmd, "MEM")  == 0)     mem_dump();
        else if (kstrcmp(cmd, "DATE") == 0)     cmd_date();
        else if (kstrcmp(cmd, "HISTORY")==0)    cmd_history();
        else if (kstrcmp(cmd, "TYPE") == 0)     cmd_type(argc, argv);
        else if (kstrcmp(cmd, "MD")   == 0)     cmd_md(argc, argv);
        else if (kstrcmp(cmd, "BASIC")== 0)     basic_run();
        else if (kstrcmp(cmd, "SHUTDOWN")==0)   cmd_shutdown();
        else {
            kprintf("'%s' is not recognized as an internal command.\n"
                    "Type HELP for a list of commands.\n", cmd);
        }
    }

    if (ends_with_cmd(input_buffer)) {

        static char script_buffer[4096];

        int size = fs_read_file(input_buffer,
                                script_buffer,
                                sizeof(script_buffer));

        if (size < 0) {
            kputs("File not found\n");
        } else {
            rexx_execute(script_buffer);
        }

    } else {
        rexx_execute(input_buffer);
    }

}

