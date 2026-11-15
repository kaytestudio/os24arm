/*
 * os2cmd - the OS/2 command prompt in a Linux terminal, and OS/2 commands
 * for Linux shells.
 *
 *   os2cmd                an interactive OS/2 prompt (the desktop's Alt+O)
 *   os2cmd COMMAND ...    run one OS/2 command, print its output, exit
 *                         (e.g. "/h/OS2/os2cmd dir \h" from a script)
 *
 * Every line goes to /dev/os2cmd (kernel/src/linux/linux.c), where the
 * kernel's own OS/2 command interpreter (kernel/src/main.c) runs it - DIR,
 * COPY, MOUNT, USB, MEM, TREE, ... with the same mounts and current
 * directory as the serial console's shell - and its output comes back,
 * followed by the next prompt.
 *
 * The prompt also runs Linux programs, as CMD.EXE runs .EXE files: a name
 * the OS/2 shell doesn't know is looked up as a file (C:\h\x, \h\x, or
 * relative to the current directory), then in $PATH, then as a busybox
 * applet (ls, ps, top, vi, ...), and run in this window in the current
 * directory. START prog opens it in a new terminal window instead, and
 * START /PM prog starts a graphical program with no terminal.
 *
 * Line editing is the terminal's (canonical mode). EXIT or Ctrl+D quits.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

#define OUT_MAX (80 * 1024)          /* the device returns at most 64 KB + a prompt */

static char out[OUT_MAX + 1];
static size_t out_len;
static char cwd[512] = "/";          /* the OS/2 session's directory (a Linux path) */
static int dev_fd = -1;              /* /dev/os2cmd */

/* A path as the OS/2 session sees it ("D:\\x", "\\x", "x") -> the Linux
 * path, from the kernel (/dev/os2cmd's resolve ioctl, which knows the
 * drive letters and this session's current directory). */
#define OS2CMD_RESOLVE 0x4F53
#define VFS_PATH_MAX   256
static int os2_resolve(const char *dos, char *out, size_t size)
{
    char buf[VFS_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", dos);
    if (dev_fd < 0 || ioctl(dev_fd, OS2CMD_RESOLVE, buf) != 0) return -1;
    snprintf(out, size, "%s", buf);
    return 0;
}

static void put(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(1, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

/* Reads everything the device has for the last command into out[]. */
static void collect(int fd)
{
    ssize_t n;
    out_len = 0;
    while (out_len < OUT_MAX && (n = read(fd, out + out_len, OUT_MAX - out_len)) > 0)
        out_len += (size_t)n;
    out[out_len] = '\0';
}

/* Where the prompt starts (the text after the last newline); and the
 * session's current directory, refreshed. */
static size_t prompt_at(void)
{
    size_t p = out_len;
    while (p > 0 && out[p - 1] != '\n') p--;
    if (os2_resolve(".", cwd, sizeof cwd) != 0) strcpy(cwd, "/");
    return p;
}

/* "Bad command or file name: X" - the OS/2 shell doesn't know X. */
static int unknown_command(void)
{
    return strstr(out, "Bad command or file name: ") != NULL &&
           strstr(out, "Bad command or file name: ") < out + 8;
}

/* ── Linux programs ──────────────────────────────────────────────────── */

static int split(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {                              /* "quoted argument" */
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static int is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* name as the OS/2 shell would see it -> a Linux path in out (or 0). */
static int find_program(const char *name, char *path, size_t size)
{
    char p[512];
    int has_dir = strchr(name, '\\') || strchr(name, '/') || (name[0] && name[1] == ':');
    if (os2_resolve(name, p, sizeof p) == 0 && is_file(p)) { snprintf(path, size, "%s", p); return 1; }
    /* an OS/2 program by its name alone: HELLO is HELLO.EXE, else HELLO.CMD */
    const char *leaf = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    if (strlen(name) < 250) {
        /* ... in any case (the disks keep OS/2's upper-case names): HELLO,
         * hello, hello.exe all find HELLO.EXE */
        char up[256], exe[256], cmd[256];
        snprintf(up, sizeof up, "%s", name);
        char *l = up + (leaf - name);
        for (char *c = l; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        const char *tries[4] = { 0, 0, 0, 0 };
        int nt = 0;
        if (!strchr(leaf, '.')) { snprintf(exe, sizeof exe, "%s.EXE", up); tries[nt++] = exe; }
        if (!strchr(leaf, '.')) { snprintf(cmd, sizeof cmd, "%s.CMD", up); tries[nt++] = cmd; }   /* (a REXX program: the kernel runs it with rexx) */
        tries[nt++] = up;
        for (int i = 0; i < nt; i++)
            if (os2_resolve(tries[i], p, sizeof p) == 0 && is_file(p)) { snprintf(path, size, "%s", p); return 1; }
    }
    if (has_dir) return 0;

    /* $PATH, then the directory os2cmd itself is in (on the stick: /h) */
    const char *env = getenv("PATH");
    char self[512], dirs[1100];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    self[sl > 0 ? sl : 0] = '\0';
    char *slash = strrchr(self, '/');
    if (slash) *slash = '\0';
    snprintf(dirs, sizeof dirs, "%s:%s", env ? env : "", self[0] ? self : "/h");
    for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":")) {
        snprintf(p, sizeof p, "%s/%s", d, name);
        if (is_file(p)) { snprintf(path, size, "%s", p); return 1; }
    }
    return 0;
}

/* The busybox binary, if name is one of its applets. */
static const char *busybox_for(const char *name)
{
    static char bb[512];
    static char list[8192];
    static int listed;
    if (!bb[0] && !find_program("busybox", bb, sizeof bb)) return NULL;
    if (!listed) {
        listed = 1;
        int fds[2];
        if (pipe(fds) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                dup2(fds[1], 1);
                close(fds[0]);
                execl(bb, "busybox", "--list", (char *)NULL);
                _exit(127);
            }
            close(fds[1]);
            size_t n = 0;
            ssize_t r;
            while (n < sizeof list - 1 && (r = read(fds[0], list + n, sizeof list - 1 - n)) > 0)
                n += (size_t)r;
            list[n] = '\0';
            close(fds[0]);
            if (pid > 0) waitpid(pid, NULL, 0);
        }
    }
    size_t len = strlen(name);
    for (const char *l = list; *l; ) {
        const char *e = strchr(l, '\n');
        size_t ll = e ? (size_t)(e - l) : strlen(l);
        if (ll == len && !strncmp(l, name, len)) return bb;
        if (!e) break;
        l = e + 1;
    }
    return NULL;
}

/* Builds the argv to run: path (or a .sh script through busybox sh, or a
 * busybox applet) followed by args. Returns 0 if there's no such program. */
static int resolve(char **argv, int argc, char *path, size_t size, char **run, int max)
{
    int k = 0;
    if (find_program(argv[0], path, size)) {
        size_t n = strlen(path);
        if (n > 3 && !strcmp(path + n - 3, ".sh")) {   /* no #! in this kernel's execve */
            static char script[512];
            snprintf(script, sizeof script, "%s", path);
            if (!find_program("busybox", path, size)) return 0;
            run[k++] = "sh";
            run[k++] = script;
        } else {
            run[k++] = path;
        }
    } else {
        const char *bb = busybox_for(argv[0]);
        if (!bb) return 0;
        snprintf(path, size, "%s", bb);
        run[k++] = argv[0];                            /* busybox picks the applet by argv[0] */
    }
    /* path is what to execve; run[] its argv */
    /* Arguments that are OS/2 paths ("D:\\x.s", "src\\x.c", "\\x") become
     * the paths the program can open; others are passed as typed. */
    static char conv[64][512];
    for (int i = 1; i < argc && k < max - 1; i++) {
        const char *a = argv[i];
        int dos = (((a[0] | 0x20) >= 'a' && (a[0] | 0x20) <= 'z') && a[1] == ':') || strchr(a, '\\');
        if (dos && i < 64 && os2_resolve(a, conv[i], sizeof conv[i]) == 0) run[k++] = conv[i];
        else run[k++] = argv[i];
    }
    run[k] = NULL;
    return 1;
}

/* an OS/2 program (LX), not a Linux one? */
static int is_os2_program(const char *path)
{
    char m[2] = { 0 };
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int r = read(fd, m, 2) == 2 && m[0] == 'L' && m[1] == 'X';
    close(fd);
    return r;
}

/* Runs a program in this window, in the OS/2 directory, and waits. OS/2
 * programs (CMD.EXE, ...) read keys one at a time and echo them
 * themselves: the terminal is raw for them, as the console is. */
static void run_here(char **argv, int argc, int prompt)
{
    char path[512];
    char *run[64];
    if (!resolve(argv, argc, path, sizeof path, run, 64)) {
        if (prompt) put(out, out_len);                /* the shell's own message */
        else fprintf(stderr, "%s: no such program\n", argv[0]);
        return;
    }
    if (prompt) put("\r\n", 2);
    struct termios saved, raw;
    int os2 = is_os2_program(path) && tcgetattr(0, &saved) == 0;
    if (os2) {
        raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_iflag &= ~ICRNL;                        /* Enter is CR, as on the console */
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (chdir(cwd) != 0) perror(cwd);
        execve(path, run, environ);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid < 0) { perror("fork"); return; }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (os2) tcsetattr(0, TCSANOW, &saved);
    if (WIFSIGNALED(status) && WTERMSIG(status) != SIGINT)
        fprintf(stderr, "\n[%s: killed by signal %d]\n", argv[0], WTERMSIG(status));
    if (!prompt) return;
    size_t p = prompt_at();
    put(out + p, out_len - p);                        /* just the prompt again */
}

/* START [/PM] prog args: a new terminal window (or, /PM, no terminal). */
static void start(char **argv, int argc)
{
    int pm = argc > 1 && (!strcmp(argv[1], "/PM") || !strcmp(argv[1], "/pm"));
    argv += 1 + pm;
    argc -= 1 + pm;
    if (argc < 1) {
        fputs("\nUsage: START [/PM] program [arguments]\n"
              "  a new terminal window for it; /PM: a graphical program, no terminal\n", stdout);
        return;
    }
    char path[512];
    char *run[64];
    if (!resolve(argv, argc, path, sizeof path, run, 60)) {
        printf("\nSTART: %s: no such program\n", argv[0]);
        return;
    }
    char *full[72];
    const char *exe = path;
    int k = 0;
    static char foot[512];
    if (!pm) {                                        /* foot -c foot.ini path [name] args */
        if (!find_program("foot", foot, sizeof foot)) { puts("\nSTART: no terminal program on this system"); return; }
        exe = foot;
        full[k++] = foot;
        static char ini[512];                          /* foot.ini: next to foot */
        snprintf(ini, sizeof ini, "%.*s/foot.ini", (int)(strrchr(foot, '/') - foot), foot);
        if (is_file(ini)) { full[k++] = "-c"; full[k++] = ini; }
        full[k++] = path;
        if (strcmp(run[0], path)) full[k++] = run[0];  /* busybox: the applet, or sh */
        for (int i = 1; run[i] && k < 71; i++) full[k++] = run[i];
    } else {
        for (int i = 0; run[i] && k < 71; i++) full[k++] = run[i];
    }
    full[k] = NULL;
    pid_t pid = fork();
    if (pid == 0) {                                   /* detached: its own session */
        setsid();
        if (fork() > 0) _exit(0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }   /* not this window's */
        if (chdir(cwd) != 0) { /* stay where we are */ }
        execve(exe, full, environ);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
    printf("\n%s started\n", argv[0]);
}

/* ── main ────────────────────────────────────────────────────────────── */

/* EXIT (any case, surrounding blanks allowed)? */
static int is_word(const char *s, const char *w)
{
    while (*s == ' ' || *s == '\t') s++;
    for (; *w; s++, w++)
        if ((*s & ~0x20) != *w) return 0;
    return !*s || *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r';
}

/* One line: the OS/2 shell first; if it doesn't know the command, a
 * Linux program. interactive: show the prompt that follows. */
static void line_cmd(int fd, char *line, int interactive)
{
    if (is_word(line, "RUN")) {                       /* RUN X.EXE: an OS/2 program, here */
        char copy[256], *argv[64];
        snprintf(copy, sizeof copy, "%s", line);
        int argc = split(copy, argv, 64);
        if (argc < 2) { fputs("\nUsage: RUN program.EXE [arguments]\n", stdout); fflush(stdout); }
        else run_here(argv + 1, argc - 1, interactive);
        return;
    }
    if (is_word(line, "START")) {
        char *argv[64];
        int argc = split(line, argv, 64);
        start(argv, argc);
        if (interactive) {
            if (write(fd, "", 1) < 0) return;          /* a fresh prompt */
            collect(fd);
            put(out, out_len);
        }
        return;
    }
    size_t n = strlen(line);
    if (write(fd, line, n ? n : 1) < 0) { perror("os2cmd: write"); exit(1); }
    collect(fd);
    size_t p = prompt_at();
    if (unknown_command()) {
        char *argv[64];
        char copy[256];
        snprintf(copy, sizeof copy, "%s", line);
        int argc = split(copy, argv, 64);
        if (argc) {
            if (!interactive) {
                char path[512];
                char *run[64];
                if (!resolve(argv, argc, path, sizeof path, run, 64)) { put(out, p); exit(1); }
                if (chdir(cwd) != 0) { /* the caller's directory, then */ }
                execve(path, run, environ);
                perror(argv[0]);
                exit(127);
            }
            run_here(argv, argc, 1);
            return;
        }
    }
    if (interactive) { put(out, out_len); return; }
    /* one-shot: without the prompt, or the blank lines around the output */
    size_t a = 0;
    while (a < p && (out[a] == '\r' || out[a] == '\n')) a++;
    while (p > a && (out[p - 1] == '\r' || out[p - 1] == '\n' || out[p - 1] == ' ')) p--;
    put(out + a, p - a);
}

int main(int argc, char **argv)
{
    int fd = open("/dev/os2cmd", O_RDWR);
    if (fd < 0) {
        perror("os2cmd: /dev/os2cmd");
        return 1;
    }
    dev_fd = fd;
    collect(fd);                                      /* the banner and first prompt */
    prompt_at();

    if (argc > 1) {                                   /* os2cmd COMMAND ... (or os2cmd /C "a command line", as CMD.EXE) */
        char line[256];
        size_t n = 0;
        int verbatim = argc > 2 && (!strcmp(argv[1], "/C") || !strcmp(argv[1], "/c"));
        for (int i = verbatim ? 2 : 1; i < argc; i++) {
            if (verbatim) { n += (size_t)snprintf(line + n, sizeof line - n, "%s%s", i > 2 ? " " : "", argv[i]); if (n >= sizeof line) { fputs("os2cmd: command too long\n", stderr); return 1; } continue; }
            int quote = strchr(argv[i], ' ') != NULL;
            n += (size_t)snprintf(line + n, sizeof line - n, "%s%s%s%s", i > 1 ? " " : "",
                                  quote ? "\"" : "", argv[i], quote ? "\"" : "");
            if (n >= sizeof line) { fputs("os2cmd: command too long\n", stderr); return 1; }
        }
        line_cmd(fd, line, 0);
        put("\n", 1);
        return 0;
    }

    signal(SIGINT, SIG_IGN);                          /* Ctrl+C is for the programs it runs */
    fputs("\033]0;OS/2 Command Prompt\007", stdout);  /* window title */
    fflush(stdout);
    put(out, out_len);

    char line[256];
    for (;;) {
        size_t n = 0;
        int eof = 0;
        for (;;) {                                    /* one line from the terminal */
            char c;
            ssize_t r = read(0, &c, 1);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { eof = 1; break; }
            if (c == '\n') break;
            if (n < sizeof line - 1) line[n++] = c;
        }
        line[n] = '\0';
        if (eof && !n) { put("\n", 1); return 0; }
        if (is_word(line, "EXIT")) return 0;
        line_cmd(fd, line, 1);
        fflush(stdout);
    }
}
