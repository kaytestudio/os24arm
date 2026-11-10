/**
 * @file cmd_main.c
 * @brief Standalone entry point for CMD.EXE
 *
 * This is the top-level command interpreter loop for the DOS command set
 * (dos_commands.c / dos_shell_dispatch), built as its own freestanding
 * program and wrapped as a genuine OS/2 LX CMD.EXE (see the CMD_EXE build
 * target in the top-level CMakeLists.txt).
 *
 * It calls the exact same os2api DOS-call bridge as the in-kernel shell -
 * everything here runs in the same flat, unprotected address space as the
 * kernel (there is no process isolation or syscall boundary in this
 * project), so this is the real command-interpreter logic, not a demo.
 *
 * The kernel's built-in LX loader (kernel/src/lx_loader.c, RUN CMD.EXE)
 * can now load and jump into this binary directly. Unlike a plain in-
 * process function call, DosOpen/DosRead/... here are resolved through
 * DOSCALLS.DLL - a real OS/2 LX DLL (see add_lx_dll(DOSCALLS ...) in the
 * top-level CMakeLists.txt) that CMD.EXE embeds a build-time copy of
 * (os2api/doscalls_embed.S.in) and loads via the kernel's built-in DLL
 * loader (kernel/src/lx_dll_loader.c) - see os2api/os2_api_import.h/.c.
 * The DLL owns its own private vfs.c/blkdev.c/ext4.c/btrfs.c state
 * (entirely disconnected from whatever the kernel or another program
 * already mounted) and mounts it lazily, on whichever Dos* call touches
 * storage first.
 */

#include "dos.h"
#include "os2_api_import.h"
#include "uart.h"
#include "string.h"

#define CMD_LINE_MAX 256

static int cmd_tokenize(char *str, char **tokens, int max_tokens)
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

int main(void)
{
    char buffer[CMD_LINE_MAX];

    /* Resolves DosOpen/DosRead/... against the embedded DOSCALLS.DLL
     * (see os2api/os2_api_import.h) - it mounts its own storage lazily,
     * on whichever Dos* call touches it first, so there's nothing else
     * to initialize here (this used to mount CMD.EXE's own private
     * vfs_init/blk_init/ext4_init/btrfs_init state directly; that now
     * happens inside the DLL instead). */
    if (os2_api_import_init() != 0) {
        uart_puts("\r\nOS/2 Warp ARM64 - CMD.EXE: failed to load DOSCALLS.DLL\r\n");
        return 1;
    }

    uart_puts("\r\nOS/2 Warp ARM64 - CMD.EXE\r\n");
    uart_puts("Type a DOS command (DIR, TYPE, COPY, DEL, REN, MD, RD, CD, VOL, ATTRIB, CHKDSK, FORMAT)\r\n");
    uart_puts("Type DOSKEY to enable command history/macros\r\n\r\n");

    for (;;) {
        dos_read_line("[C:\\]> ", buffer, sizeof(buffer), NULL);

        /* A DOSKEY macro using $T comes back as multiple '\n'-separated
         * commands - run each one in turn. */
        char *seg = buffer;
        while (*seg) {
            char *nl = strchr(seg, '\n');
            if (nl) *nl = '\0';
            if (*seg) {
                char *argv[16];
                int argc = cmd_tokenize(seg, argv, 16);
                if (argc > 0 && !dos_shell_dispatch(argc, argv)) {
                    uart_puts("Bad command or file name: ");
                    uart_puts(argv[0]);
                    uart_puts("\r\n");
                }
            }
            if (!nl) break;
            seg = nl + 1;
        }
    }

    return 0;
}
