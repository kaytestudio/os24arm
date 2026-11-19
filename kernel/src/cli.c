#include "keyboard.h"
#include "cli_fs.h"
#include "basic.h"
#include "vfs.h"

#define CLI_MAX_LINE  256
#define CLI_HIST_SIZE 16
extern void rexx_interactive(void);
extern void rexx_run_script(const char* path);

/* UART output */
static void putc_out(char c)
{
    volatile unsigned long *fr = (volatile unsigned long *)(0x09000000UL + 0x018);
    volatile unsigned long *dr = (volatile unsigned long *)(0x09000000UL + 0x000);
    while (*fr & (1u << 5))
        ;
    *dr = (unsigned long)(unsigned char)c;
}

static void puts_out(const char *s)
{
    while (*s) {
        if (*s == '\n') putc_out('\r');
        putc_out(*s++);
    }
}

static void putn_out(unsigned long n)
{
    char buf[12];
    int  i = 0;
    if (n == 0) { putc_out('0'); return; }
    while (n) { buf[i++] = '0' + (char)(n % 10); n /= 10; }
    while (i--) putc_out(buf[i]);
}

/* ANSI helpers */
static void cursor_left (unsigned long n) { if (n) { puts_out("\x1b["); putn_out(n); putc_out('D'); } }
static void cursor_right(unsigned long n) { if (n) { puts_out("\x1b["); putn_out(n); putc_out('C'); } }
static void erase_to_eol(void)            { puts_out("\x1b[K"); }

/* String helpers */
static int kstrlen(const char *s) { int i = 0; while (s[i]) i++; return i; }

static void kmemcpy(char *d, const char *s, int n) { while (n--) *d++ = *s++; }

static int kstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int kstrncmp(const char *a, const char *b, int n)
{
    while (n-- > 0 && *a && *a == *b) { a++; b++; }
    if (n < 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* Command history */
static char hist[CLI_HIST_SIZE][CLI_MAX_LINE];
static int  hist_count = 0;
static int  hist_head  = 0;

static void hist_push(const char *line)
{
    if (!line[0]) return;
    int prev = (hist_head + CLI_HIST_SIZE - 1) % CLI_HIST_SIZE;
    if (hist_count > 0) {
        int i = 0;
        while (hist[prev][i] && hist[prev][i] == line[i]) i++;
        if (!hist[prev][i] && !line[i]) return;
    }
    int j = 0;
    while (line[j] && j < CLI_MAX_LINE - 1) {
        hist[hist_head][j] = line[j];
        j++;
    }
    hist[hist_head][j] = '\0';
    hist_head = (hist_head + 1) % CLI_HIST_SIZE;
    if (hist_count < CLI_HIST_SIZE) hist_count++;
}

static const char *hist_get(int offset)
{
    if (offset < 1 || offset > hist_count) return NULL;
    int idx = (hist_head - offset + CLI_HIST_SIZE * 2) % CLI_HIST_SIZE;
    return hist[idx];
}

/* Line editor */
static int cli_readline(char *buf, int maxlen)
{
    int  len = 0, cur = 0, hoff = 0;
    char saved[CLI_MAX_LINE];
    saved[0] = '\0';

    for (;;) {
        unsigned long key = kbd_getc();

        if (key == KEY_CTRL_C) { puts_out("^C\r\n"); buf[0] = '\0'; return 0; }
        if (key == KEY_CTRL_D && len == 0) { puts_out("exit\r\n"); return -1; }
        if (key == KEY_CTRL_L) {
            puts_out("\x1b[2J\x1b[H");
            puts_out(cli_fs_get_prompt());
            buf[len] = '\0'; puts_out(buf);
            if (cur < len) cursor_left((unsigned long)(len - cur));
            continue;
        }

        if (key == KEY_ENTER || key == '\n') {
            buf[len] = '\0'; puts_out("\r\n"); hist_push(buf); return len;
        }

        if (key == KEY_BACKSPACE) {
            if (cur == 0) continue;
            kmemcpy(buf + cur - 1, buf + cur, len - cur);
            len--; cur--; buf[len] = '\0';
            cursor_left(1);
            for (int i = cur; i < len; i++) putc_out(buf[i]);
            erase_to_eol(); cursor_left((unsigned long)(len - cur));
            continue;
        }

        if (key == KEY_DEL) {
            if (cur == len) continue;
            kmemcpy(buf + cur, buf + cur + 1, len - cur - 1);
            len--; buf[len] = '\0';
            for (int i = cur; i < len; i++) putc_out(buf[i]);
            erase_to_eol(); cursor_left((unsigned long)(len - cur));
            continue;
        }

        if (key == KEY_LEFT)  { if (cur > 0)  { cur--; cursor_left(1);  } continue; }
        if (key == KEY_RIGHT) { if (cur < len) { cur++; cursor_right(1); } continue; }
        if (key == KEY_HOME)  { cursor_left((unsigned long)cur); cur = 0; continue; }
        if (key == KEY_END)   { cursor_right((unsigned long)(len - cur)); cur = len; continue; }

        if (key == KEY_UP) {
            if (hoff == 0) { buf[len] = '\0'; kmemcpy(saved, buf, len + 1); }
            const char *h = hist_get(hoff + 1);
            if (!h) continue;
            hoff++;
            cursor_left((unsigned long)cur); erase_to_eol();
            len = kstrlen(h); cur = len;
            kmemcpy(buf, h, len + 1); puts_out(buf);
            continue;
        }

        if (key == KEY_DOWN) {
            if (hoff == 0) continue;
            hoff--;
            cursor_left((unsigned long)cur); erase_to_eol();
            if (hoff == 0) {
                len = kstrlen(saved); kmemcpy(buf, saved, len + 1);
            } else {
                const char *h = hist_get(hoff);
                len = kstrlen(h); kmemcpy(buf, h, len + 1);
            }
            cur = len; puts_out(buf);
            continue;
        }

        if (key == KEY_TAB) continue;
        if (key > 0x7F)     continue;

        if (len >= maxlen - 1) continue;
        kmemcpy(buf + cur + 1, buf + cur, len - cur);
        buf[cur] = (char)key;
        len++; cur++; buf[len] = '\0';
        for (int i = cur - 1; i < len; i++) putc_out(buf[i]);
        if (cur < len) cursor_left((unsigned long)(len - cur));
    }
}

/* Help */
static void cmd_help(void)
{
    puts_out("\r\n  OS/2 Warp ARM64 -- Command Reference\r\n\r\n");
    puts_out("  General:\r\n");
    puts_out("    help                  This message\r\n");
    puts_out("    ver                   OS/2 version\r\n");
    puts_out("    cls                   Clear screen\r\n");
    puts_out("    echo [text|ON|OFF]    Print text or toggle echo\r\n");
    puts_out("    date / time           Show date / time\r\n");
    puts_out("    mem                   Memory usage\r\n");
    puts_out("    pause                 Wait for keypress\r\n");
    puts_out("    set [var[=val]]       Environment variables\r\n");
    puts_out("    path [path]           Search path\r\n");
    puts_out("    prompt [text]         Set prompt ($P$G etc.)\r\n");
    puts_out("    basic                 QuickBASIC interpreter\r\n");
    puts_out("    halt                  Halt the CPU\r\n");
    puts_out("\r\n  File System:\r\n");
    puts_out("    dir   [path][/W][/A]  List directory\r\n");
    puts_out("    cd    [path]          Change / show directory\r\n");
    puts_out("    md    path            Make directory  (mkdir)\r\n");
    puts_out("    rd    path            Remove directory (rmdir)\r\n");
    puts_out("    type  file            Print file contents\r\n");
    puts_out("    copy  src dst         Copy file\r\n");
    puts_out("    xcopy src dst [/S]    Copy tree\r\n");
    puts_out("    move  src dst         Move / rename file\r\n");
    puts_out("    del   file            Delete file  (erase)\r\n");
    puts_out("    ren   old new         Rename file  (rename)\r\n");
    puts_out("    attrib [+/-RHS] file  File attributes\r\n");
    puts_out("    tree  [path]          Directory tree\r\n");
    puts_out("\r\n  Volume:\r\n");
    puts_out("    vol    [drive:]       Volume label\r\n");
    puts_out("    chkdsk [drive:]       Volume statistics\r\n");
    puts_out("    format dev [/FS:type] [/V:label] [/Q]\r\n");
    puts_out("                          Format block device\r\n");
    puts_out("                          /FS:FAT32|EXT4|EXFAT\r\n");
    puts_out("                          /V:label (volume label)\r\n");
    puts_out("                          /Q (quick format)\r\n");
    puts_out("    mount  dev mpt [/FS:type]\r\n");
    puts_out("                          Mount filesystem\r\n");
    puts_out("                          dev = block device (vda1, vda2)\r\n");
    puts_out("                          mpt = mount point (/, C:)\r\n");
    puts_out("                          /FS:FAT32|EXT4 (auto-detect if omitted)\r\n");
    puts_out("    umount mpt            Unmount volume\r\n");
    puts_out("\r\n");
}

/* Command matching */
static const char *cmd_arg(const char *line, const char *cmd)
{
    int n = kstrlen(cmd);
    if (kstrncmp(line, cmd, n) != 0) return NULL;
    if (line[n] != '\0' && line[n] != ' ') return NULL;
    const char *arg = line + n;
    while (*arg == ' ') arg++;
    return arg;
}

/* Dispatcher */
static void dispatch(const char *line)
{
    const char *arg;
    if (!line[0]) return;

    if (kstrcmp(line, "help")  == 0) { cmd_help();  return; }
    if (kstrcmp(line, "basic") == 0) { basic_run(); return; }
    if (kstrcmp(line, "halt")  == 0) {
        puts_out("Halting.\r\n");
        for (;;) __asm__ volatile("wfe");
    }

    /* General */
    if ((arg = cmd_arg(line, "ver"))     != NULL) { cli_cmd_ver();       return; }
    if ((arg = cmd_arg(line, "version")) != NULL) { cli_cmd_ver();       return; }
    if ((arg = cmd_arg(line, "cls"))     != NULL) { cli_cmd_cls();       return; }
    if ((arg = cmd_arg(line, "clear"))   != NULL) { cli_cmd_cls();       return; }
    if ((arg = cmd_arg(line, "date"))    != NULL) { cli_cmd_date();      return; }
    if ((arg = cmd_arg(line, "time"))    != NULL) { cli_cmd_time();      return; }
    if ((arg = cmd_arg(line, "mem"))     != NULL) { cli_cmd_mem();       return; }
    if ((arg = cmd_arg(line, "pause"))   != NULL) { cli_cmd_pause();     return; }
    if ((arg = cmd_arg(line, "echo"))    != NULL) { cli_cmd_echo(arg);   return; }
    if ((arg = cmd_arg(line, "set"))     != NULL) { cli_cmd_set(arg);    return; }
    if ((arg = cmd_arg(line, "path"))    != NULL) { cli_cmd_path(arg);   return; }
    if ((arg = cmd_arg(line, "prompt"))  != NULL) { cli_cmd_prompt(arg); return; }

    /* File system */
    if ((arg = cmd_arg(line, "dir"))     != NULL) { cli_cmd_dir(arg);    return; }
    if ((arg = cmd_arg(line, "cd"))      != NULL) { cli_cmd_cd(arg);     return; }
    if ((arg = cmd_arg(line, "chdir"))   != NULL) { cli_cmd_cd(arg);     return; }
    if ((arg = cmd_arg(line, "md"))      != NULL) { cli_cmd_md(arg);     return; }
    if ((arg = cmd_arg(line, "mkdir"))   != NULL) { cli_cmd_md(arg);     return; }
    if ((arg = cmd_arg(line, "rd"))      != NULL) { cli_cmd_rd(arg);     return; }
    if ((arg = cmd_arg(line, "rmdir"))   != NULL) { cli_cmd_rd(arg);     return; }
    if ((arg = cmd_arg(line, "type"))    != NULL) { cli_cmd_type(arg);   return; }
    if ((arg = cmd_arg(line, "copy"))    != NULL) { cli_cmd_copy(arg);   return; }
    if ((arg = cmd_arg(line, "xcopy"))   != NULL) { cli_cmd_xcopy(arg);  return; }
    if ((arg = cmd_arg(line, "move"))    != NULL) { cli_cmd_move(arg);   return; }
    if ((arg = cmd_arg(line, "del"))     != NULL) { cli_cmd_del(arg);    return; }
    if ((arg = cmd_arg(line, "erase"))   != NULL) { cli_cmd_del(arg);    return; }
    if ((arg = cmd_arg(line, "ren"))     != NULL) { cli_cmd_ren(arg);    return; }
    if ((arg = cmd_arg(line, "rename"))  != NULL) { cli_cmd_ren(arg);    return; }
    if ((arg = cmd_arg(line, "attrib"))  != NULL) { cli_cmd_attrib(arg); return; }
    if ((arg = cmd_arg(line, "tree"))    != NULL) { cli_cmd_tree(arg);   return; }

    /* Volume */
    if ((arg = cmd_arg(line, "vol"))     != NULL) { cli_cmd_vol(arg);    return; }
    if ((arg = cmd_arg(line, "chkdsk"))  != NULL) { cli_cmd_chkdsk(arg); return; }
    if ((arg = cmd_arg(line, "format"))  != NULL) { cli_cmd_format(arg); return; }
    if ((arg = cmd_arg(line, "mount"))   != NULL) { cli_cmd_mount(arg);  return; }
    if ((arg = cmd_arg(line, "umount"))  != NULL) { cli_cmd_umount(arg); return; }

    puts_out("SYS1023: ");
    puts_out(line);
    puts_out(" is not recognized as an internal or external command.\r\n");
}

/* Entry point */
void cli_run(void)
{
    char buf[CLI_MAX_LINE];
    puts_out("\r\nOS/2 Warp ARM64 CLI  -  type 'help' for commands\r\n\r\n");
    for (;;) {
        puts_out(cli_fs_get_prompt());
        if (cli_readline(buf, CLI_MAX_LINE) < 0) break;
        dispatch(buf);
    }
}
