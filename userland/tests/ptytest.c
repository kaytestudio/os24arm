/* ptytest.c - pseudo-terminals as a terminal emulator uses them: an
 * interactive shell on a pty's slave, driven through the master.
 *   ptytest /path/to/busybox */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

static int fails, master;
static char out[8192];
static size_t outn;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

/* Read from the master until want shows up (or timeout seconds). */
static int expect(const char *want, int timeout)
{
    time_t end = time(0) + timeout;
    while (time(0) <= end) {
        out[outn] = 0;
        if (strstr(out, want)) return 1;
        struct pollfd p = { master, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t n = read(master, out + outn, sizeof out - 1 - outn);
        if (n <= 0) break;
        outn += (size_t)n;
    }
    out[outn] = 0;
    return strstr(out, want) != 0;
}
static void say(const char *s) { write(master, s, strlen(s)); }
static void reset(void) { outn = 0; out[0] = 0; }

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    const char *bb = argc > 1 ? argv[1] : "/h/busybox";
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, "posix_openpt/grantpt/unlockpt");
    char *name = ptsname(master);
    CHECK(name && !strncmp(name, "/dev/pts/", 9), "ptsname: %s", name ? name : "?");
    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(name, O_RDWR);              /* becomes the controlling terminal */
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        char *a[] = { (char *)bb, "sh", "-i", 0 };
        char *e[] = { "PS1=pty$ ", "TERM=xterm", "HOME=/tmp", 0 };
        execve(bb, a, e);
        _exit(99);
    }
    CHECK(expect("pty$ ", 10), "shell prompt on the master");
    reset(); say("echo sum=$((2+3)); tty\r");
    CHECK(expect("sum=5", 10) && expect("/dev/pts/", 5), "command output through the pty");
    reset(); say("sleep 20\r");
    usleep(500000);
    say("\003");                                 /* Ctrl-C: SIGINT to the foreground job */
    say("echo rc=$?\r");
    CHECK(expect("rc=130", 10), "Ctrl-C interrupts the foreground job");
    reset();
    ws.ws_row = 40; ws.ws_col = 100;
    ioctl(master, TIOCSWINSZ, &ws);              /* SIGWINCH to the foreground group */
    say("stty size\r");
    CHECK(expect("40 100", 10), "window size seen by the shell (stty size)");
    reset(); say("exit 7\r");
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "shell exit status %d", WEXITSTATUS(st));
    char b[64];
    ssize_t n;
    while ((n = read(master, b, sizeof b)) > 0) { }
    CHECK(n < 0 && errno == EIO, "master reads EIO once the slave is gone");
    close(master);
    printf("ptytest: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    if (fails) printf("--- last output ---\n%s\n", out);
    return fails;
}
