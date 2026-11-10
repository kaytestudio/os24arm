/**
 * @file dos_commands.c
 * @brief DOS-style command implementations
 *
 * Every command here talks to the system exclusively through os2api's
 * Dos* calls (DosOpen, DosFindFirst, DosCreateDir, ...) - never vfs_*
 * directly - matching how a real DOS/OS2 command-line utility only ever
 * calls DOSCALLS.DLL, not the filesystem driver underneath it.
 */

#include "dos.h"
/* Shared between two very different builds (see the big comment on
 * dos_shell_dispatch() in dos.h): the in-kernel shell / dos static
 * library (dos/CMakeLists.txt) links the real os2api static library
 * directly, so DosOpen/DosRead/... are plain functions (os2_api.h) -
 * but the standalone CMD.EXE/KAYTEVM.EXE builds (top-level
 * CMakeLists.txt) resolve them at runtime from an embedded DOSCALLS.DLL
 * instead, as function *pointer* variables of the same names
 * (os2_api_import.h) - see that header's comment. Those builds define
 * OS2_API_USE_IMPORT_TABLE to select it here. */
#ifdef OS2_API_USE_IMPORT_TABLE
#include "os2_api_import.h"
#else
#include "os2_api.h"
#endif
#include "uart.h"
#include "kprintf.h"
#include "string.h"

static void print_dos_error(const char *cmd, APIRET rc)
{
    uart_puts("\r\n");
    uart_puts(cmd);
    uart_puts(": ");
    switch (rc) {
        case ERROR_FILE_NOT_FOUND:  uart_puts("File not found\r\n"); break;
        case ERROR_PATH_NOT_FOUND:  uart_puts("Path not found\r\n"); break;
        case ERROR_ACCESS_DENIED:   uart_puts("Access denied\r\n"); break;
        case ERROR_INVALID_HANDLE:  uart_puts("Invalid handle\r\n"); break;
        case ERROR_INVALID_DRIVE:   uart_puts("Invalid drive or filesystem not mounted\r\n"); break;
        case ERROR_NO_MORE_FILES:   uart_puts("File not found\r\n"); break;
        case ERROR_WRITE_PROTECT:   uart_puts("Disk is write-protected\r\n"); break;
        case ERROR_NOT_SUPPORTED:   uart_puts("Not supported by this filesystem\r\n"); break;
        case ERROR_INVALID_PARAMETER: uart_puts("Invalid parameter\r\n"); break;
        case ERROR_BUFFER_OVERFLOW: uart_puts("Path too long\r\n"); break;
        default: kprintf("Error %u\r\n", rc); break;
    }
}


/* ── ECHO ─────────────────────────────────────────────────────────────────── */

void dos_echo(int argc, char **argv)
{
    uart_puts("\r\n");
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            uart_putc(' ');
        uart_puts(argv[i]);
    }
    uart_puts("\r\n");
}

/* ── DEBUG ─────────────────────────────────────────────────────────────────
 * A partial reimplementation of the classic MS-DOS DEBUG.COM/DEBUG.EXE
 * shell: its own interactive "-" sub-prompt (reusing dos_read_line(), so
 * DOSKEY history/macros work inside it too) until Q returns to the normal
 * DOS prompt.
 *
 * The memory commands - D(ump)/E(nter)/F(ill)/C(ompare)/M(ove)/S(earch),
 * plus H(ex arithmetic) - are pure byte-addressed operations with nothing
 * x86-specific about them, so they're implemented for real here: real
 * flat 64-bit addresses (no x86 segment:offset), parsed the way DEBUG.COM
 * itself did (bare hex, "start end" or "start Llength" ranges). R shows
 * genuinely-current AArch64 processor state (SP, exception level, DAIF)
 * rather than a fake x86 AX/BX/CX/DX register file.
 *
 * The rest of DEBUG.COM's command set doesn't have a sensible equivalent
 * on this platform and stays unimplemented (recognized, so typing one
 * reports "not implemented" rather than a misleading "bad command"):
 * I and O (IN/OUT) address x86 port I/O, which doesn't exist on ARM64
 * (devices are memory-mapped, not port-mapped); XA/XD/XM/XS manage EMS
 * (bank-switched memory above real mode's 640KB conventional-memory
 * ceiling) - meaningless with a flat 64-bit address space; A/U
 * (assemble/unassemble) would need a full AArch64 assembler/disassembler;
 * G/P/T (go/proceed/trace) would need breakpoint/single-step support this
 * kernel's exception vectors don't wire up; L/W (load/write sectors) and
 * N (name a program to load) belong to DEBUG.COM's real-mode program
 * loader, which this kernel's own LX loader (RUN, kernel/src/lx_loader.c)
 * already covers differently.
 *
 * Reading/writing an address the caller got wrong can fault exactly like
 * it could in real DEBUG.COM - a raw memory monitor is expected to trust
 * its operator, not second-guess every address against a permission
 * table this kernel doesn't even have.
 */

static void dos_debug_help(void)
{
    uart_puts("\r\n");
    uart_puts("assemble     A [address]\r\n");
    uart_puts("compare      C range address\r\n");
    uart_puts("dump         D [range]\r\n");
    uart_puts("enter        E address [list]\r\n");
    uart_puts("fill         F range list\r\n");
    uart_puts("go           G [=address] [addresses]\r\n");
    uart_puts("hex          H value1 value2\r\n");
    uart_puts("input        I port\r\n");
    uart_puts("load         L [address] [drive] [firstsector] [number]\r\n");
    uart_puts("move         M range address\r\n");
    uart_puts("name         N [pathname] [arglist]\r\n");
    uart_puts("output       O port byte\r\n");
    uart_puts("proceed      P [=address] [number]\r\n");
    uart_puts("quit         Q\r\n");
    uart_puts("register     R [register]\r\n");
    uart_puts("search       S range list\r\n");
    uart_puts("trace        T [=address] [value]\r\n");
    uart_puts("unassemble   U [range]\r\n");
    uart_puts("write        W [address] [drive] [firstsector] [number]\r\n");
    uart_puts("allocate expanded memory        XA [#pages]\r\n");
    uart_puts("deallocate expanded memory      XD [handle]\r\n");
    uart_puts("map expanded memory pages       XM [Lpage] [Ppage] [handle]\r\n");
    uart_puts("display expanded memory status  XS\r\n");
}

/* ── DEBUG memory commands: shared parsing helpers ──────────────────────── */

static void debug_tokenize(char *str, char **tokens, int max_tokens, int *out_count)
{
    int count = 0;
    char *p = str;
    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    *out_count = count;
}

/* Bare hex (DEBUG.COM's native convention, e.g. "D 100 110") or 0x-prefixed. */
static int debug_parse_hex(const char *s, unsigned long *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return 0;
    unsigned long v = 0;
    for (; *s; s++) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (unsigned long)d;
    }
    *out = v;
    return 1;
}

static void debug_print_hex(unsigned long v)
{
    char buf[17];
    int i = 16;
    buf[16] = '\0';
    if (v == 0) { uart_putc('0'); return; }
    while (v && i > 0) {
        int d = (int)(v & 0xF);
        buf[--i] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        v >>= 4;
    }
    uart_puts(&buf[i]);
}

/* "start end" (end inclusive) or "start Llength" - the two range forms
 * DEBUG.COM itself accepted. *next_arg is set to the index of the first
 * argument after the range (the byte/pattern list, for F/E/S). */
static int debug_parse_range(int argc, char **argv, unsigned long *start, unsigned long *len, int *next_arg)
{
    if (argc < 1 || !debug_parse_hex(argv[0], start)) return 0;
    if (argc < 2) { *len = 0; *next_arg = 1; return 1; }

    if (argv[1][0] == 'L' || argv[1][0] == 'l') {
        if (!debug_parse_hex(argv[1] + 1, len)) return 0;
    } else {
        unsigned long end;
        if (!debug_parse_hex(argv[1], &end)) return 0;
        *len = (end >= *start) ? (end - *start + 1) : 0;
    }
    *next_arg = 2;
    return 1;
}

/* ── DEBUG memory commands ───────────────────────────────────────────────── */

static unsigned long debug_last_addr = 0;

static void debug_cmd_dump(int argc, char **argv)
{
    unsigned long start = debug_last_addr;
    unsigned long len = 128; /* DEBUG.COM's default dump size */

    if (argc == 1) {
        if (!debug_parse_hex(argv[0], &start)) { uart_puts("^ Error\r\n"); return; }
    } else if (argc >= 2) {
        int next;
        if (!debug_parse_range(argc, argv, &start, &len, &next) || len == 0) {
            uart_puts("^ Error\r\n");
            return;
        }
    }
    /* argc == 0: start/len stay as-is, continuing from the previous dump -
     * matching real DEBUG.COM's D with no arguments. */

    const uint8_t *p = (const uint8_t *)(uintptr_t)start;
    for (unsigned long off = 0; off < len; off += 16) {
        debug_print_hex(start + off);
        uart_puts("  ");
        unsigned long line_len = (len - off < 16) ? (len - off) : 16;
        for (unsigned long i = 0; i < 16; i++) {
            if (i < line_len) kprintf("%02x ", p[off + i]);
            else uart_puts("   ");
            if (i == 7) uart_putc(' ');
        }
        uart_putc(' ');
        for (unsigned long i = 0; i < line_len; i++) {
            uint8_t b = p[off + i];
            uart_putc((b >= 32 && b < 127) ? (char)b : '.');
        }
        uart_puts("\r\n");
    }
    debug_last_addr = start + len;
}

/* E address byte [byte...] - writes the given bytes starting at address.
 * Real DEBUG.COM also had an interactive per-byte prompt mode when no
 * byte list was given; not implemented here (this scriptable form is the
 * one that's actually useful from a macro or over a slow serial link). */
static void debug_cmd_enter(int argc, char **argv)
{
    unsigned long addr;
    if (argc < 2 || !debug_parse_hex(argv[0], &addr)) { uart_puts("^ Error\r\n"); return; }

    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    for (int i = 1; i < argc; i++) {
        unsigned long v;
        if (!debug_parse_hex(argv[i], &v) || v > 0xFF) { uart_puts("^ Error\r\n"); return; }
        p[i - 1] = (uint8_t)v;
    }
    debug_last_addr = addr;
}

static void debug_cmd_fill(int argc, char **argv)
{
    unsigned long start, len;
    int next;
    if (!debug_parse_range(argc, argv, &start, &len, &next) || len == 0) { uart_puts("^ Error\r\n"); return; }

    int nvals = argc - next;
    if (nvals <= 0) { uart_puts("^ Error\r\n"); return; }
    if (nvals > 64) nvals = 64;
    uint8_t vals[64];
    for (int i = 0; i < nvals; i++) {
        unsigned long v;
        if (!debug_parse_hex(argv[next + i], &v) || v > 0xFF) { uart_puts("^ Error\r\n"); return; }
        vals[i] = (uint8_t)v;
    }

    uint8_t *p = (uint8_t *)(uintptr_t)start;
    for (unsigned long i = 0; i < len; i++) p[i] = vals[i % (unsigned long)nvals];
    debug_last_addr = start;
}

static void debug_cmd_compare(int argc, char **argv)
{
    unsigned long start, len;
    int next;
    if (!debug_parse_range(argc, argv, &start, &len, &next) || len == 0 || next >= argc) {
        uart_puts("^ Error\r\n");
        return;
    }
    unsigned long target;
    if (!debug_parse_hex(argv[next], &target)) { uart_puts("^ Error\r\n"); return; }

    const uint8_t *a = (const uint8_t *)(uintptr_t)start;
    const uint8_t *b = (const uint8_t *)(uintptr_t)target;
    int any = 0;
    for (unsigned long i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            debug_print_hex(start + i);
            uart_putc(' ');
            kprintf("%02x", a[i]);
            uart_putc(' ');
            kprintf("%02x", b[i]);
            uart_putc(' ');
            debug_print_hex(target + i);
            uart_puts("\r\n");
            any = 1;
        }
    }
    if (!any) uart_puts("(no differences)\r\n");
}

static void debug_cmd_move(int argc, char **argv)
{
    unsigned long start, len;
    int next;
    if (!debug_parse_range(argc, argv, &start, &len, &next) || len == 0 || next >= argc) {
        uart_puts("^ Error\r\n");
        return;
    }
    unsigned long dest;
    if (!debug_parse_hex(argv[next], &dest)) { uart_puts("^ Error\r\n"); return; }

    memmove((void *)(uintptr_t)dest, (const void *)(uintptr_t)start, (size_t)len);
    debug_last_addr = dest;
}

static void debug_cmd_search(int argc, char **argv)
{
    unsigned long start, len;
    int next;
    if (!debug_parse_range(argc, argv, &start, &len, &next)) { uart_puts("^ Error\r\n"); return; }

    int nvals = argc - next;
    if (nvals <= 0) { uart_puts("^ Error\r\n"); return; }
    if (nvals > 32) nvals = 32;
    uint8_t pat[32];
    for (int i = 0; i < nvals; i++) {
        unsigned long v;
        if (!debug_parse_hex(argv[next + i], &v) || v > 0xFF) { uart_puts("^ Error\r\n"); return; }
        pat[i] = (uint8_t)v;
    }

    const uint8_t *p = (const uint8_t *)(uintptr_t)start;
    int found = 0;
    for (unsigned long i = 0; i + (unsigned long)nvals <= len; i++) {
        int match = 1;
        for (int j = 0; j < nvals; j++) {
            if (p[i + j] != pat[j]) { match = 0; break; }
        }
        if (match) {
            debug_print_hex(start + i);
            uart_puts("\r\n");
            found = 1;
        }
    }
    if (!found) uart_puts("(pattern not found)\r\n");
}

static void debug_cmd_hex(int argc, char **argv)
{
    unsigned long a, b;
    if (argc < 2 || !debug_parse_hex(argv[0], &a) || !debug_parse_hex(argv[1], &b)) {
        uart_puts("^ Error\r\n");
        return;
    }
    debug_print_hex(a + b);
    uart_putc(' ');
    debug_print_hex(a - b);
    uart_puts("\r\n");
}

/* Genuinely-current AArch64 processor state - not a stopped debuggee's
 * general-purpose registers (X0-X30/PC), which aren't available without
 * breakpoint/single-step support (see this file's G/P/T comment). */
static void debug_cmd_registers(void)
{
    unsigned long sp, currentel, daif;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(currentel));
    __asm__ volatile("mrs %0, DAIF" : "=r"(daif));

    uart_puts("SP=");
    debug_print_hex(sp);
    uart_puts("  EL=");
    kprintf("%u", (unsigned)((currentel >> 2) & 3));
    uart_puts("  DAIF=");
    debug_print_hex(daif);
    uart_puts("\r\n(X0-X30/PC need a stopped breakpoint context - not available; see G/P/T)\r\n");
}

void dos_debug(int argc, char **argv)
{
    (void)argc; (void)argv;

    static const char *known1[] = {
        "A", "C", "D", "E", "F", "G", "H", "I", "L", "M",
        "N", "O", "P", "R", "S", "T", "U", "W"
    };
    static const char *known2[] = { "XA", "XD", "XM", "XS" };

    char line[256];

    for (;;) {
        dos_read_line("-", line, sizeof(line), NULL);

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        if (*p == '?') {
            dos_debug_help();
            continue;
        }

        /* DEBUG.COM's command letter can be glued directly to its
         * arguments (e.g. "D100 110", not just "D 100 110") - pull just
         * the leading letters, not a whitespace-delimited token. */
        char mnem[3];
        int mlen = 0;
        while (*p && mlen < 2 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            mnem[mlen++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
            p++;
        }
        mnem[mlen] = '\0';

        if (mlen == 1 && mnem[0] == 'Q') {
            uart_puts("\r\n");
            return;
        }

        char *cargv[16];
        int cargc;
        debug_tokenize(p, cargv, 16, &cargc);

        if (mlen == 1 && strcmp(mnem, "D") == 0) { uart_puts("\r\n"); debug_cmd_dump(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "E") == 0) { uart_puts("\r\n"); debug_cmd_enter(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "F") == 0) { uart_puts("\r\n"); debug_cmd_fill(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "C") == 0) { uart_puts("\r\n"); debug_cmd_compare(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "M") == 0) { uart_puts("\r\n"); debug_cmd_move(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "S") == 0) { uart_puts("\r\n"); debug_cmd_search(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "H") == 0) { uart_puts("\r\n"); debug_cmd_hex(cargc, cargv); continue; }
        if (mlen == 1 && strcmp(mnem, "R") == 0) { uart_puts("\r\n"); debug_cmd_registers(); continue; }

        int recognized = 0;
        if (mlen == 1) {
            for (unsigned i = 0; i < sizeof(known1) / sizeof(known1[0]); i++) {
                if (strcmp(mnem, known1[i]) == 0) { recognized = 1; break; }
            }
        } else if (mlen == 2) {
            for (unsigned i = 0; i < sizeof(known2) / sizeof(known2[0]); i++) {
                if (strcmp(mnem, known2[i]) == 0) { recognized = 1; break; }
            }
        }

        uart_puts("\r\n");
        if (recognized) {
            uart_puts(mnem);
            uart_puts(": not implemented in this build (? for the command list)\r\n");
        } else {
            uart_puts("^ Error\r\n");
        }
    }
}


/* ── COMP ─────────────────────────────────────────────────────────────────
 * Byte-for-byte binary comparison of two files (classic DOS COMP): reports
 * the offset and differing byte values at the first point the files
 * diverge (or a size mismatch), else "Files compare OK". */

void dos_comp(int argc, char **argv)
{
    if (argc < 3) {
        uart_puts("\r\nUsage: COMP file1 file2\r\n");
        return;
    }

    HFILE h1, h2;
    uint32_t action;

    APIRET rc = DosOpen(argv[1], &h1, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        print_dos_error("COMP", rc);
        return;
    }
    rc = DosOpen(argv[2], &h2, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        DosClose(h1);
        print_dos_error("COMP", rc);
        return;
    }

    char buf1[512], buf2[512];
    uint32_t offset = 0;
    int differs = 0;

    for (;;) {
        uint32_t got1, got2;
        rc = DosRead(h1, buf1, sizeof(buf1), &got1);
        if (rc != NO_ERROR) { print_dos_error("COMP", rc); differs = 1; break; }
        rc = DosRead(h2, buf2, sizeof(buf2), &got2);
        if (rc != NO_ERROR) { print_dos_error("COMP", rc); differs = 1; break; }

        uint32_t n = (got1 < got2) ? got1 : got2;
        uint32_t i;
        for (i = 0; i < n; i++) {
            if (buf1[i] != buf2[i]) break;
        }
        if (i < n) {
            uart_puts("\r\nCompare error at offset ");
            kprintf("%u", offset + i);
            uart_puts("\r\n  ");
            uart_puts(argv[1]);
            uart_puts(": ");
            kprintf("%02x", (unsigned char)buf1[i]);
            uart_puts("\r\n  ");
            uart_puts(argv[2]);
            uart_puts(": ");
            kprintf("%02x", (unsigned char)buf2[i]);
            uart_puts("\r\n");
            differs = 1;
            break;
        }

        if (got1 != got2) {
            uart_puts("\r\nFiles are different sizes\r\n");
            differs = 1;
            break;
        }

        offset += n;
        if (got1 == 0) break; /* both exhausted - no differences found */
    }

    DosClose(h1);
    DosClose(h2);

    if (!differs) {
        uart_puts("\r\nFiles compare OK\r\n");
    }
}

/* ── FC ───────────────────────────────────────────────────────────────────
 * Line-oriented text comparison (classic DOS FC): reports the line number
 * and content of every differing line, and which file has extra trailing
 * lines if their line counts don't match. Unlike real FC, this doesn't
 * try to resynchronize after a mismatch (no diff/LCS alignment) - each
 * pair of corresponding lines is just compared by position. */

#define FC_LINE_MAX 256

typedef struct {
    HFILE    h;
    char     chunk[512];
    uint32_t chunk_len;
    uint32_t chunk_pos;
    int      eof;
} fc_reader_t;

static void fc_reader_init(fc_reader_t *r, HFILE h)
{
    r->h = h;
    r->chunk_len = 0;
    r->chunk_pos = 0;
    r->eof = 0;
}

/* Reads one '\n'-terminated (CRLF normalized) line into line[max_len],
 * NUL-terminated and truncated if longer than the buffer. Returns 1 if a
 * line was read, 0 at true end-of-file with nothing left to read. */
static int fc_read_line(fc_reader_t *r, char *line, int max_len)
{
    if (r->eof && r->chunk_pos >= r->chunk_len) return 0;

    int len = 0;
    int saw_any = 0;
    for (;;) {
        if (r->chunk_pos >= r->chunk_len) {
            if (r->eof) break;
            uint32_t got;
            APIRET rc = DosRead(r->h, r->chunk, sizeof(r->chunk), &got);
            if (rc != NO_ERROR || got == 0) { r->eof = 1; break; }
            r->chunk_len = got;
            r->chunk_pos = 0;
        }
        char c = r->chunk[r->chunk_pos++];
        saw_any = 1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len < max_len - 1) line[len++] = c;
    }
    line[len] = '\0';
    return saw_any;
}

void dos_fc(int argc, char **argv)
{
    if (argc < 3) {
        uart_puts("\r\nUsage: FC file1 file2\r\n");
        return;
    }

    HFILE h1, h2;
    uint32_t action;

    APIRET rc = DosOpen(argv[1], &h1, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        print_dos_error("FC", rc);
        return;
    }
    rc = DosOpen(argv[2], &h2, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        DosClose(h1);
        print_dos_error("FC", rc);
        return;
    }

    fc_reader_t r1, r2;
    fc_reader_init(&r1, h1);
    fc_reader_init(&r2, h2);

    char line1[FC_LINE_MAX], line2[FC_LINE_MAX];
    uint32_t lineno = 0;
    int any_diff = 0;

    for (;;) {
        int have1 = fc_read_line(&r1, line1, sizeof(line1));
        int have2 = fc_read_line(&r2, line2, sizeof(line2));
        if (!have1 && !have2) break;
        lineno++;

        if (!have1 || !have2) {
            uart_puts("\r\n***** ");
            uart_puts(have1 ? argv[1] : argv[2]);
            uart_puts(" has more lines, starting at line ");
            kprintf("%u", lineno);
            uart_puts("\r\n");
            any_diff = 1;
            break;
        }

        if (strcmp(line1, line2) != 0) {
            uart_puts("\r\n***** Line ");
            kprintf("%u", lineno);
            uart_puts(" differs:\r\n  ");
            uart_puts(argv[1]);
            uart_puts(": ");
            uart_puts(line1);
            uart_puts("\r\n  ");
            uart_puts(argv[2]);
            uart_puts(": ");
            uart_puts(line2);
            uart_puts("\r\n");
            any_diff = 1;
        }
    }

    DosClose(h1);
    DosClose(h2);

    if (!any_diff) {
        uart_puts("\r\nFC: no differences encountered\r\n");
    }
}


/* ── CLS ─────────────────────────────────────────────────────────────────── */

void dos_cls(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("\x1B[2J\x1B[H");
}


/* ── DOSKEY ───────────────────────────────────────────────────────────────
 * Based on https://en.wikipedia.org/wiki/DOSKEY : command history (arrow
 * keys, F7 list, F8 prefix search, F9 select-by-number), line editing
 * (Esc clears the line; insert/overstrike), and $-macros ($1-$9, $*, $T,
 * $$).
 *
 * Real DOSKEY is a TSR that hooks INT 21h's keyboard-input routine, so
 * once installed it transparently augments every later command line.
 * There's no such hook point here - dos_read_line() (below) is the
 * closest equivalent: the one place both shells (kernel_shell() in
 * kernel/src/main.c and CMD.EXE's main() in dos/cmd_main.c) already read
 * a full command line, so that's where "is DOSKEY installed" is checked
 * and where history/macro handling lives. Until DOSKEY is run once
 * (doskey_installed == 0), dos_read_line() behaves exactly like the
 * plain byte-at-a-time input loop both shells used to have inline -
 * matching real DOS, where an unloaded DOSKEY changes nothing.
 *
 * Real Ctrl+Home/Ctrl+End (delete to line start/end) don't have a
 * reliable short byte sequence over a raw VT100-ish serial link, so
 * this binds the same *function* to the readline/bash convention for
 * it instead: Ctrl+U (kill to start) and Ctrl+K (kill to end). Esc
 * still clears the whole line, exactly as the real DOSKEY does.
 */

#define DOSKEY_LINE_MAX        256
#define DOSKEY_MAX_HISTORY     32
#define DOSKEY_MAX_MACROS      16
#define DOSKEY_MACRO_NAME_MAX  32
#define DOSKEY_MACRO_TEXT_MAX  200

typedef struct {
    char name[DOSKEY_MACRO_NAME_MAX];
    char text[DOSKEY_MACRO_TEXT_MAX];
    int  used;
} doskey_macro_t;

static char history[DOSKEY_MAX_HISTORY][DOSKEY_LINE_MAX];
static int  history_count = 0;                  /* valid entries, <= history_limit */
static int  history_next  = 0;                   /* ring buffer write cursor */
static int  history_limit = DOSKEY_MAX_HISTORY;   /* effective size, see /LISTSIZE */

static doskey_macro_t macros[DOSKEY_MAX_MACROS];

static int doskey_installed  = 0;
static int doskey_insert_mode = 1; /* 1 = insert, 0 = overstrike; toggled by Insert key or /INSERT,/OVERSTRIKE */

/* n is 1-indexed, oldest entry = 1 (matches how /HISTORY and F7 number
 * their listing, and how F9 asks "Line number:"). */
static int history_slot(int n)
{
    int browse = history_count - n + 1;
    return (history_next - browse + history_limit) % history_limit;
}

static void history_add(const char *line)
{
    if (line[0] == '\0') return;
    if (history_count > 0) {
        int last = (history_next - 1 + history_limit) % history_limit;
        if (strcmp(history[last], line) == 0) return; /* don't duplicate the previous entry */
    }
    strncpy(history[history_next], line, DOSKEY_LINE_MAX - 1);
    history[history_next][DOSKEY_LINE_MAX - 1] = '\0';
    history_next = (history_next + 1) % history_limit;
    if (history_count < history_limit) history_count++;
}

static int macro_find(const char *name)
{
    for (int i = 0; i < DOSKEY_MAX_MACROS; i++)
        if (macros[i].used && strncasecmp(macros[i].name, name, DOSKEY_MACRO_NAME_MAX) == 0)
            return i;
    return -1;
}

static void macro_set(const char *name, const char *text)
{
    int idx = macro_find(name);
    if (text[0] == '\0') {
        if (idx >= 0) macros[idx].used = 0;
        return;
    }
    if (idx < 0) {
        for (int i = 0; i < DOSKEY_MAX_MACROS; i++) {
            if (!macros[i].used) { idx = i; break; }
        }
        if (idx < 0) {
            uart_puts("DOSKEY: macro table full\r\n");
            return;
        }
    }
    strncpy(macros[idx].name, name, DOSKEY_MACRO_NAME_MAX - 1);
    macros[idx].name[DOSKEY_MACRO_NAME_MAX - 1] = '\0';
    strncpy(macros[idx].text, text, DOSKEY_MACRO_TEXT_MAX - 1);
    macros[idx].text[DOSKEY_MACRO_TEXT_MAX - 1] = '\0';
    macros[idx].used = 1;
}

/* If line's first word names a macro, expands it into out ($1-$9 = the
 * Nth whitespace-separated word after the macro name, $* = all of it,
 * $T = a literal '\n' command separator - dos_read_line()'s callers
 * split the returned line on '\n' and run each piece as its own
 * command, $$ = literal '$') and returns 1. Returns 0 (out untouched)
 * if the first word isn't a macro. */
static int doskey_expand_macro(const char *line, char *out, int out_size)
{
    int wordlen = 0;
    while (line[wordlen] && line[wordlen] != ' ') wordlen++;
    if (wordlen == 0 || wordlen >= DOSKEY_MACRO_NAME_MAX) return 0;

    char name[DOSKEY_MACRO_NAME_MAX];
    memcpy(name, line, wordlen);
    name[wordlen] = '\0';

    int idx = macro_find(name);
    if (idx < 0) return 0;

    const char *rest = line + wordlen;
    while (*rest == ' ') rest++;

    const char *args[9];
    int arglen[9];
    int nargs = 0;
    {
        const char *p = rest;
        while (*p && nargs < 9) {
            while (*p == ' ') p++;
            if (!*p) break;
            args[nargs] = p;
            int l = 0;
            while (p[l] && p[l] != ' ') l++;
            arglen[nargs] = l;
            p += l;
            nargs++;
        }
    }

    int oi = 0;
    const char *t = macros[idx].text;
    while (*t && oi < out_size - 1) {
        if (*t == '$' && t[1]) {
            char d = t[1];
            if (d >= '1' && d <= '9') {
                int n = d - '1';
                if (n < nargs)
                    for (int i = 0; i < arglen[n] && oi < out_size - 1; i++) out[oi++] = args[n][i];
                t += 2;
                continue;
            } else if (d == '*') {
                for (const char *r = rest; *r && oi < out_size - 1; r++) out[oi++] = *r;
                t += 2;
                continue;
            } else if (d == 'T' || d == 't') {
                out[oi++] = '\n';
                t += 2;
                continue;
            } else if (d == '$') {
                out[oi++] = '$';
                t += 2;
                continue;
            }
        }
        out[oi++] = *t++;
    }
    out[oi] = '\0';
    return 1;
}

static void doskey_load_macrofile(const char *filename)
{
    HFILE h;
    uint32_t action;
    APIRET rc = DosOpen(filename, &h, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        print_dos_error("DOSKEY", rc);
        return;
    }

    char filebuf[512];
    char lineb[DOSKEY_MACRO_NAME_MAX + DOSKEY_MACRO_TEXT_MAX];
    int lpos = 0, loaded = 0;
    uint32_t got;

    while (DosRead(h, filebuf, sizeof(filebuf), &got) == NO_ERROR && got > 0) {
        for (uint32_t i = 0; i < got; i++) {
            char c = filebuf[i];
            if (c == '\n' || c == '\r') {
                if (lpos > 0) {
                    lineb[lpos] = '\0';
                    char *eq = strchr(lineb, '=');
                    if (eq) { *eq = '\0'; macro_set(lineb, eq + 1); loaded++; }
                    lpos = 0;
                }
            } else if (lpos < (int)sizeof(lineb) - 1) {
                lineb[lpos++] = c;
            }
        }
    }
    if (lpos > 0) {
        lineb[lpos] = '\0';
        char *eq = strchr(lineb, '=');
        if (eq) { *eq = '\0'; macro_set(lineb, eq + 1); loaded++; }
    }

    DosClose(h);
    kprintf("%d macro(s) loaded from %s\r\n", loaded, filename);
}

void dos_doskey(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\n");
        if (doskey_installed) {
            uart_puts("DOSKEY is already installed.\r\n");
        } else {
            doskey_installed = 1;
            uart_puts("DOSKEY installed.\r\n");
        }
        return;
    }

    uart_puts("\r\n");

    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (strncasecmp(arg, "/REINSTALL", 10) == 0) {
            history_count = 0;
            history_next = 0;
            for (int m = 0; m < DOSKEY_MAX_MACROS; m++) macros[m].used = 0;
            doskey_installed = 1;
            uart_puts("DOSKEY installed (history and macros cleared).\r\n");
        }
        else if (strncasecmp(arg, "/HISTORY", 8) == 0) {
            for (int n = 1; n <= history_count; n++) {
                kprintf("%d", n);
                uart_puts(": ");
                uart_puts(history[history_slot(n)]);
                uart_puts("\r\n");
            }
        }
        else if (strncasecmp(arg, "/MACROS", 7) == 0) {
            int any = 0;
            for (int m = 0; m < DOSKEY_MAX_MACROS; m++) {
                if (macros[m].used) {
                    uart_puts(macros[m].name);
                    uart_putc('=');
                    uart_puts(macros[m].text);
                    uart_puts("\r\n");
                    any = 1;
                }
            }
            if (!any) uart_puts("No macros defined.\r\n");
        }
        else if (strncasecmp(arg, "/INSERT", 7) == 0) {
            doskey_insert_mode = 1;
        }
        else if (strncasecmp(arg, "/OVERSTRIKE", 11) == 0) {
            doskey_insert_mode = 0;
        }
        else if (strncasecmp(arg, "/LISTSIZE=", 10) == 0) {
            int n = 0;
            const char *p = arg + 10;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            if (n < 1) n = 1;
            if (n > DOSKEY_MAX_HISTORY) n = DOSKEY_MAX_HISTORY;
            history_limit = n;
            history_count = 0;
            history_next = 0;
            kprintf("History size set to %d (history cleared).\r\n", n);
        }
        else if (strncasecmp(arg, "/MACROFILE=", 11) == 0) {
            doskey_load_macrofile(arg + 11);
        }
        else {
            /* macroname=[text] - this project's tokenizer has no quoting,
             * so multi-word macro text is reconstructed by re-joining the
             * remaining tokens with single spaces. */
            char *eq = strchr(arg, '=');
            if (!eq) {
                uart_puts("DOSKEY: unrecognized option: ");
                uart_puts(arg);
                uart_puts("\r\n");
                continue;
            }
            *eq = '\0';
            char *name = arg;
            char textbuf[DOSKEY_MACRO_TEXT_MAX];
            int ti = 0;
            const char *first = eq + 1;
            while (*first && ti < DOSKEY_MACRO_TEXT_MAX - 1) textbuf[ti++] = *first++;
            for (int j = i + 1; j < argc && ti < DOSKEY_MACRO_TEXT_MAX - 1; j++) {
                textbuf[ti++] = ' ';
                for (const char *w = argv[j]; *w && ti < DOSKEY_MACRO_TEXT_MAX - 1; w++) textbuf[ti++] = *w;
            }
            textbuf[ti] = '\0';

            macro_set(name, textbuf);
            uart_puts("Macro '");
            uart_puts(name);
            uart_puts(textbuf[0] ? "' defined.\r\n" : "' deleted.\r\n");
            i = argc; /* the rest of argv[] was consumed as this macro's text */
        }
    }
}

/* ── Shared line editor (dos_read_line) ──────────────────────────────────
 * The one place both shells read a full command line - see the big
 * comment above dos_doskey() for why this, rather than dos_doskey()
 * itself, is where history/macro/editing actually happens. */

typedef enum {
    KEY_NONE = 0, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DELETE, KEY_INSERT,
    KEY_F7, KEY_F8, KEY_F9, KEY_ESC_ALONE
} doskey_key_t;

static char read_char(dos_idle_fn idle)
{
    for (;;) {
        if (uart_ready()) return uart_getc();
        if (idle) idle();
    }
}

static doskey_key_t read_escape_sequence(dos_idle_fn idle)
{
    /* A real terminal sends a CSI/SS3 sequence's bytes back-to-back as
     * part of one write() - by the time we get here (right after seeing
     * ESC), '[' or 'O' should already be sitting in the UART FIFO if this
     * really is a sequence. A non-blocking check (rather than read_char()'s
     * blocking wait) is what makes a *lone* Esc press distinguishable from
     * the start of a sequence: blocking here would otherwise eat the
     * user's next ordinary keystroke while waiting for a follow-up byte
     * that a lone Esc press was never going to send. */
    if (!uart_ready()) return KEY_ESC_ALONE;
    char c1 = uart_getc();
    if (c1 != '[' && c1 != 'O') return KEY_ESC_ALONE;
    char c2 = read_char(idle);
    if (c1 == 'O') return KEY_NONE; /* SS3 (e.g. F1-F4 on some terminals) - unused here */

    switch (c2) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
    }
    if (c2 >= '0' && c2 <= '9') {
        int num = c2 - '0';
        char c3;
        for (;;) {
            c3 = read_char(idle);
            if (c3 >= '0' && c3 <= '9') { num = num * 10 + (c3 - '0'); continue; }
            break;
        }
        if (c3 == '~') {
            switch (num) {
                case 1: return KEY_HOME;
                case 2: return KEY_INSERT;
                case 3: return KEY_DELETE;
                case 4: return KEY_END;
                case 18: return KEY_F7;
                case 19: return KEY_F8;
                case 20: return KEY_F9;
            }
        }
    }
    return KEY_NONE;
}

/* Single active edit session at a time (this kernel is single-threaded),
 * so plain module statics are simpler than threading state through every
 * helper below. */
static char ed_buf[DOSKEY_LINE_MAX];
static int  ed_len, ed_cursor;
static int  ed_browse;                    /* 0 = not browsing history; N = Nth-most-recent shown */
static char ed_f8_prefix[DOSKEY_LINE_MAX];
static int  ed_f8_active, ed_f8_offset;

/* Repaints the input field after ed_buf/ed_len/ed_cursor changed, given
 * the on-screen state (old_len, old_cursor) from just before the change. */
static void ed_redraw(int old_len, int old_cursor)
{
    for (int i = 0; i < old_cursor; i++) uart_putc('\b');
    for (int i = 0; i < ed_len; i++) uart_putc(ed_buf[i]);
    int pad = old_len - ed_len;
    for (int i = 0; i < pad; i++) uart_putc(' ');
    for (int i = 0; i < pad; i++) uart_putc('\b');
    for (int i = ed_len; i > ed_cursor; i--) uart_putc('\b');
}

static void ed_load_history(int browse)
{
    int old_len = ed_len, old_cursor = ed_cursor;
    ed_browse = browse;
    if (browse == 0) {
        ed_len = 0;
    } else {
        strncpy(ed_buf, history[history_slot(history_count - browse + 1)], DOSKEY_LINE_MAX - 1);
        ed_buf[DOSKEY_LINE_MAX - 1] = '\0';
        ed_len = (int)strlen(ed_buf);
    }
    ed_cursor = ed_len;
    ed_redraw(old_len, old_cursor);
}

int dos_read_line(const char *prompt, char *buffer, int max_len, dos_idle_fn idle)
{
    uart_puts(prompt);

    if (!doskey_installed) {
        /* Plain input, matching the byte-at-a-time loop both shells used
         * to have inline, before DOSKEY has ever been run. */
        int pos = 0;
        for (;;) {
            char c = read_char(idle);
            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                buffer[pos] = '\0';
                return pos;
            } else if (c == 0x7F || c == 0x08) {
                if (pos > 0) { pos--; uart_puts("\b \b"); }
            } else if (c == 0x03) {
                uart_puts("^C\r\n");
                pos = 0;
                uart_puts(prompt);
            } else if (c >= 32 && c < 127 && pos < max_len - 1) {
                buffer[pos++] = c;
                uart_putc(c);
            }
        }
    }

    ed_len = 0;
    ed_cursor = 0;
    ed_browse = 0;
    ed_f8_active = 0;

    for (;;) {
        char c = read_char(idle);

        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            ed_buf[ed_len] = '\0';
            history_add(ed_buf);

            char expanded[DOSKEY_LINE_MAX];
            const char *result = doskey_expand_macro(ed_buf, expanded, sizeof(expanded)) ? expanded : ed_buf;
            strncpy(buffer, result, max_len - 1);
            buffer[max_len - 1] = '\0';
            return (int)strlen(buffer);
        }
        else if (c == 0x1B) {
            switch (read_escape_sequence(idle)) {
                case KEY_UP:
                    if (ed_browse < history_count) ed_load_history(ed_browse + 1);
                    break;
                case KEY_DOWN:
                    if (ed_browse > 0) ed_load_history(ed_browse - 1);
                    break;
                case KEY_LEFT:
                    if (ed_cursor > 0) { int oc = ed_cursor--; ed_redraw(ed_len, oc); }
                    break;
                case KEY_RIGHT:
                    if (ed_cursor < ed_len) { int oc = ed_cursor++; ed_redraw(ed_len, oc); }
                    break;
                case KEY_HOME:
                    if (ed_cursor != 0) { int oc = ed_cursor; ed_cursor = 0; ed_redraw(ed_len, oc); }
                    break;
                case KEY_END:
                    if (ed_cursor != ed_len) { int oc = ed_cursor; ed_cursor = ed_len; ed_redraw(ed_len, oc); }
                    break;
                case KEY_DELETE:
                    if (ed_cursor < ed_len) {
                        int ol = ed_len, oc = ed_cursor;
                        memmove(&ed_buf[ed_cursor], &ed_buf[ed_cursor + 1], ed_len - ed_cursor - 1);
                        ed_len--;
                        ed_redraw(ol, oc);
                    }
                    ed_f8_active = 0;
                    break;
                case KEY_INSERT:
                    doskey_insert_mode = !doskey_insert_mode;
                    break;
                case KEY_F7: {
                    uart_puts("\r\n");
                    for (int n = 1; n <= history_count; n++) {
                        kprintf("%d", n);
                        uart_puts(": ");
                        uart_puts(history[history_slot(n)]);
                        uart_puts("\r\n");
                    }
                    uart_puts(prompt);
                    for (int i = 0; i < ed_len; i++) uart_putc(ed_buf[i]);
                    for (int i = ed_len; i > ed_cursor; i--) uart_putc('\b');
                    break;
                }
                case KEY_F8: {
                    if (!ed_f8_active) {
                        memcpy(ed_f8_prefix, ed_buf, ed_len);
                        ed_f8_prefix[ed_len] = '\0';
                        ed_f8_active = 1;
                        ed_f8_offset = 0;
                    }
                    int plen = (int)strlen(ed_f8_prefix);
                    for (int t = ed_f8_offset + 1; t <= history_count; t++) {
                        int slot = history_slot(history_count - t + 1);
                        if (strncmp(history[slot], ed_f8_prefix, plen) == 0) {
                            int old_len = ed_len, old_cursor = ed_cursor;
                            strncpy(ed_buf, history[slot], DOSKEY_LINE_MAX - 1);
                            ed_buf[DOSKEY_LINE_MAX - 1] = '\0';
                            ed_len = (int)strlen(ed_buf);
                            ed_cursor = ed_len;
                            ed_f8_offset = t;
                            ed_redraw(old_len, old_cursor);
                            break;
                        }
                    }
                    break;
                }
                case KEY_F9: {
                    uart_puts("\r\nLine number: ");
                    char numbuf[8];
                    int npos = 0;
                    for (;;) {
                        char nc = read_char(idle);
                        if (nc == '\r' || nc == '\n') break;
                        if ((nc == 0x7F || nc == 0x08) && npos > 0) { npos--; uart_puts("\b \b"); continue; }
                        if (nc >= '0' && nc <= '9' && npos < 7) { numbuf[npos++] = nc; uart_putc(nc); }
                    }
                    numbuf[npos] = '\0';
                    int n = 0;
                    for (int i = 0; i < npos; i++) n = n * 10 + (numbuf[i] - '0');
                    uart_puts("\r\n");
                    if (n >= 1 && n <= history_count) {
                        strncpy(ed_buf, history[history_slot(n)], DOSKEY_LINE_MAX - 1);
                        ed_buf[DOSKEY_LINE_MAX - 1] = '\0';
                        ed_len = (int)strlen(ed_buf);
                        ed_cursor = ed_len;
                    }
                    uart_puts(prompt);
                    for (int i = 0; i < ed_len; i++) uart_putc(ed_buf[i]);
                    break;
                }
                case KEY_ESC_ALONE:
                    if (ed_len != 0) { int ol = ed_len, oc = ed_cursor; ed_len = 0; ed_cursor = 0; ed_redraw(ol, oc); }
                    ed_f8_active = 0;
                    break;
                default:
                    break;
            }
        }
        else if (c == 0x7F || c == 0x08) { /* Backspace: delete char before cursor */
            if (ed_cursor > 0) {
                int ol = ed_len, oc = ed_cursor;
                memmove(&ed_buf[ed_cursor - 1], &ed_buf[ed_cursor], ed_len - ed_cursor);
                ed_len--;
                ed_cursor--;
                ed_redraw(ol, oc);
            }
            ed_f8_active = 0;
        }
        else if (c == 0x03) { /* Ctrl+C: abort this line, keep editing a fresh one */
            uart_puts("^C\r\n");
            ed_len = 0;
            ed_cursor = 0;
            ed_browse = 0;
            ed_f8_active = 0;
            uart_puts(prompt);
        }
        else if (c == 0x15) { /* Ctrl+U: delete from cursor to line start (real DOSKEY's Ctrl+Home) */
            if (ed_cursor > 0) {
                int ol = ed_len, oc = ed_cursor;
                memmove(&ed_buf[0], &ed_buf[ed_cursor], ed_len - ed_cursor);
                ed_len -= ed_cursor;
                ed_cursor = 0;
                ed_redraw(ol, oc);
            }
            ed_f8_active = 0;
        }
        else if (c == 0x0B) { /* Ctrl+K: delete from cursor to line end (real DOSKEY's Ctrl+End) */
            if (ed_cursor < ed_len) {
                int ol = ed_len, oc = ed_cursor;
                ed_len = ed_cursor;
                ed_redraw(ol, oc);
            }
            ed_f8_active = 0;
        }
        else if (c >= 32 && c < 127) {
            if (ed_len < DOSKEY_LINE_MAX - 1) {
                int ol = ed_len, oc = ed_cursor;
                if (doskey_insert_mode) {
                    memmove(&ed_buf[ed_cursor + 1], &ed_buf[ed_cursor], ed_len - ed_cursor);
                    ed_buf[ed_cursor] = c;
                    ed_len++;
                } else {
                    ed_buf[ed_cursor] = c;
                    if (ed_cursor == ed_len) ed_len++;
                }
                ed_cursor++;
                ed_redraw(ol, oc);
            }
            ed_f8_active = 0;
        }
    }
}

/* ── EXIT ─────────────────────────────────────────────────────────────────── */

/* This flat-address-space kernel has no process boundary to return
 * across (see cmd_run()'s comment in kernel/src/main.c: CMD.EXE is
 * treated as "a shell that never returns"), so EXIT halts the CPU here
 * the same way it does in the in-kernel shell - there's nowhere else to
 * go back to, whether this runs as the built-in shell or as CMD.EXE. */
void dos_exit(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("\r\n[KERNEL] Exiting to idle loop...\r\n\r\n");
    while (1) {
        __asm__ volatile("wfi");
    }
}


/* ── DIR ─────────────────────────────────────────────────────────────────── */

void dos_dir(int argc, char **argv)
{
    const char *arg = (argc > 1) ? argv[1] : "*";

    char resolved[VFS_PATH_MAX];
    DosResolvePath(arg, resolved, sizeof(resolved));

    /* If the argument names an actual directory (no wildcard given),
     * list its contents rather than searching for a file literally
     * named that - matching real DOS's "DIR <dir>" behavior. */
    FILESTATUS probe;
    if (DosQueryPathInfo(resolved, 1, &probe, sizeof(probe)) == NO_ERROR &&
        (probe.attributes & VFS_ATTR_DIR)) {
        uint32_t len = (uint32_t)strlen(resolved);
        if (len == 0 || resolved[len - 1] != '/') {
            if (len < sizeof(resolved) - 2) resolved[len++] = '/';
        }
        if (len < sizeof(resolved) - 2) {
            resolved[len++] = '*';
            resolved[len] = '\0';
        }
    }

    uart_puts("\r\n Directory of ");
    uart_puts(resolved);
    uart_puts("\r\n\r\n");

    HDIR hdir;
    FILEFINDBUF ffb;
    uint32_t count;
    APIRET rc = DosFindFirst(resolved, &hdir, 0, &ffb, sizeof(ffb), &count);

    if (rc != NO_ERROR) {
        if (rc == ERROR_NO_MORE_FILES || rc == ERROR_FILE_NOT_FOUND) {
            uart_puts("File not found\r\n\r\n");
        } else {
            print_dos_error("DIR", rc);
        }
        return;
    }

    uint32_t file_count = 0, dir_count = 0;
    do {
        uart_puts("  ");
        uart_puts(ffb.name);

        if (ffb.attributes & VFS_ATTR_DIR) {
            uart_puts("\t<DIR>");
            dir_count++;
        } else {
            uart_puts("\t");
            kprintf("%u", ffb.file_size);
            uart_puts(" bytes");
            file_count++;
        }
        uart_puts("\r\n");
    } while (DosFindNext(hdir, &ffb, sizeof(ffb), &count) == NO_ERROR);

    DosFindClose(hdir);

    uart_puts("\r\n");
    kprintf("%u", file_count);
    uart_puts(" file(s), ");
    kprintf("%u", dir_count);
    uart_puts(" dir(s)\r\n\r\n");
}

/* ── TYPE ────────────────────────────────────────────────────────────────── */

void dos_type(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: TYPE filename\r\n");
        return;
    }

    HFILE h;
    uint32_t action;
    APIRET rc = DosOpen(argv[1], &h, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        print_dos_error("TYPE", rc);
        return;
    }

    uart_puts("\r\n");
    char buf[512];
    uint32_t got;
    while (DosRead(h, buf, sizeof(buf) - 1, &got) == NO_ERROR && got > 0) {
        buf[got] = '\0';
        uart_puts(buf);
    }
    uart_puts("\r\n");
    DosClose(h);
}

/* ── COPY ────────────────────────────────────────────────────────────────── */

void dos_copy(int argc, char **argv)
{
    if (argc < 3) {
        uart_puts("\r\nUsage: COPY source dest\r\n");
        return;
    }

    HFILE src, dst;
    uint32_t action;

    APIRET rc = DosOpen(argv[1], &src, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        print_dos_error("COPY", rc);
        return;
    }

    rc = DosOpen(argv[2], &dst, &action, 0, 0, 0, O_WRONLY | O_CREAT | O_TRUNC);
    if (rc != NO_ERROR) {
        DosClose(src);
        print_dos_error("COPY", rc);
        return;
    }

    char buf[512];
    uint32_t total = 0;
    uint32_t got;
    while (DosRead(src, buf, sizeof(buf), &got) == NO_ERROR && got > 0) {
        uint32_t written;
        rc = DosWrite(dst, buf, got, &written);
        if (rc != NO_ERROR) {
            print_dos_error("COPY", rc);
            break;
        }
        total += written;
    }

    DosClose(src);
    DosClose(dst);

    if (rc == NO_ERROR) {
        uart_puts("\r\n");
        kprintf("%u", total);
        uart_puts(" bytes copied\r\n");
    }
}

/* ── DEL ─────────────────────────────────────────────────────────────────── */

void dos_del(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: DEL filename\r\n");
        return;
    }

    APIRET rc = DosDelete(argv[1]);
    if (rc != NO_ERROR) {
        print_dos_error("DEL", rc);
    } else {
        uart_puts("\r\n");
        uart_puts(argv[1]);
        uart_puts(" deleted\r\n");
    }
}

/* ── REN ─────────────────────────────────────────────────────────────────── */

void dos_ren(int argc, char **argv)
{
    if (argc < 3) {
        uart_puts("\r\nUsage: REN oldname newname\r\n");
        return;
    }

    APIRET rc = DosMove(argv[1], argv[2]);
    if (rc != NO_ERROR) {
        print_dos_error("REN", rc);
    } else {
        uart_puts("\r\nFile renamed\r\n");
    }
}

/* ── MD ──────────────────────────────────────────────────────────────────── */

void dos_md(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: MD dirname\r\n");
        return;
    }

    APIRET rc = DosCreateDir(argv[1]);
    if (rc != NO_ERROR) {
        print_dos_error("MD", rc);
    } else {
        uart_puts("\r\nDirectory created\r\n");
    }
}

/* ── RD ──────────────────────────────────────────────────────────────────── */

void dos_rd(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: RD dirname\r\n");
        return;
    }

    APIRET rc = DosDeleteDir(argv[1]);
    if (rc != NO_ERROR) {
        print_dos_error("RD", rc);
    } else {
        uart_puts("\r\nDirectory removed\r\n");
    }
}

/* ── CD ──────────────────────────────────────────────────────────────────── */

void dos_cd(int argc, char **argv)
{
    if (argc < 2) {
        char buf[VFS_PATH_MAX];
        APIRET rc = DosQueryCurrentDir(buf, sizeof(buf));
        uart_puts("\r\n");
        uart_puts(rc == NO_ERROR ? buf : "(unknown)");
        uart_puts("\r\n");
        return;
    }

    APIRET rc = DosSetCurrentDir(argv[1]);
    if (rc != NO_ERROR) {
        print_dos_error("CD", rc);
    }
}

/* ── VOL ─────────────────────────────────────────────────────────────────── */

void dos_vol(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("\r\n Volume in drive C is OS2WARP\r\n");
    uart_puts(" Volume Serial Number is 0000-0001\r\n\r\n");
}

/* ── ATTRIB ──────────────────────────────────────────────────────────────── */

void dos_attrib(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("\r\nUsage: ATTRIB filename\r\n");
        return;
    }

    FILESTATUS st;
    APIRET rc = DosQueryPathInfo(argv[1], 1, &st, sizeof(st));
    if (rc != NO_ERROR) {
        print_dos_error("ATTRIB", rc);
        return;
    }

    uart_puts("\r\n");
    uart_puts((st.attributes & VFS_ATTR_DIR)    ? "D" : " ");
    uart_puts((st.attributes & VFS_ATTR_RDONLY) ? "R" : " ");
    uart_puts((st.attributes & VFS_ATTR_HIDDEN) ? "H" : " ");
    uart_puts((st.attributes & VFS_ATTR_SYSTEM) ? "S" : " ");
    uart_puts("  ");
    uart_puts(argv[1]);
    uart_puts("\r\n");
}

/* ── CHKDSK / FORMAT ─────────────────────────────────────────────────────── */

void dos_chkdsk(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("\r\nCHKDSK: not supported - filesystem drivers in this build are read-only\r\n");
}

void dos_format(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("\r\nFORMAT: not supported - filesystem drivers in this build are read-only\r\n");
}

/* ── Shell dispatcher ────────────────────────────────────────────────────── */

static void dos_str_toupper(char *s)
{
    while (*s) {
        if (*s >= 'a' && *s <= 'z')
            *s = *s - 'a' + 'A';
        s++;
    }
}

int dos_shell_dispatch(int argc, char **argv)
{
    if (argc == 0)
        return 0;

    dos_str_toupper(argv[0]);

    if (strcmp(argv[0], "DIR") == 0) {
        dos_dir(argc, argv);
    }
    else if (strcmp(argv[0], "ECHO") == 0) {
        dos_echo(argc, argv);
    }
    else if (strcmp(argv[0], "CLS") == 0) {
        dos_cls(argc, argv);
    }
    else if (strcmp(argv[0], "EXIT") == 0) {
        dos_exit(argc, argv);
    }
    else if (strcmp(argv[0], "DOSKEY") == 0) {
        dos_doskey(argc, argv);
    }
    else if (strcmp(argv[0], "DEBUG") == 0) {
        dos_debug(argc, argv);
    }
    else if (strcmp(argv[0], "TYPE") == 0) {
        dos_type(argc, argv);
    }
    else if (strcmp(argv[0], "COPY") == 0) {
        dos_copy(argc, argv);
    }
    else if (strcmp(argv[0], "COMP") == 0) {
        dos_comp(argc, argv);
    }
    else if (strcmp(argv[0], "FC") == 0) {
        dos_fc(argc, argv);
    }
    else if (strcmp(argv[0], "DEL") == 0 || strcmp(argv[0], "ERASE") == 0) {
        dos_del(argc, argv);
    }
    else if (strcmp(argv[0], "REN") == 0 || strcmp(argv[0], "RENAME") == 0) {
        dos_ren(argc, argv);
    }
    else if (strcmp(argv[0], "MD") == 0 || strcmp(argv[0], "MKDIR") == 0) {
        dos_md(argc, argv);
    }
    else if (strcmp(argv[0], "RD") == 0 || strcmp(argv[0], "RMDIR") == 0) {
        dos_rd(argc, argv);
    }
    else if (strcmp(argv[0], "CD") == 0 || strcmp(argv[0], "CHDIR") == 0) {
        dos_cd(argc, argv);
    }
    else if (strcmp(argv[0], "VOL") == 0) {
        dos_vol(argc, argv);
    }
    else if (strcmp(argv[0], "ATTRIB") == 0) {
        dos_attrib(argc, argv);
    }
    else if (strcmp(argv[0], "CHKDSK") == 0) {
        dos_chkdsk(argc, argv);
    }
    else if (strcmp(argv[0], "FORMAT") == 0) {
        dos_format(argc, argv);
    }
    else {
        return 0;
    }

    return 1;
}
