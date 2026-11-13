/*
 * os2cmd - the OS/2 command prompt in a Linux terminal.
 *
 * Relays between this terminal and /dev/os2cmd (kernel/src/linux/linux.c):
 * every line typed here is run by the kernel's own OS/2 command
 * interpreter (kernel/src/main.c) - DIR, COPY, MOUNT, USB, MEM, TREE, ...
 * with the same mounts and current directory as the serial console's
 * shell - and what it prints comes back, followed by the next prompt.
 * Line editing is the terminal's (canonical mode). EXIT or Ctrl+D quits.
 *
 * On the desktop: "OS/2 Command Prompt" in fuzzel (Alt+P), which opens
 * foot running this (userland/rootfs/applications/os2cmd.desktop).
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void drain(int fd)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t done = 0; done < n; ) {
            ssize_t w = write(1, buf + done, (size_t)(n - done));
            if (w <= 0) return;
            done += w;
        }
    }
}

/* EXIT (any case, surrounding blanks allowed)? */
static int is_exit(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    const char *e = "EXIT";
    for (; *e; s++, e++)
        if ((*s & ~0x20) != *e) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return !*s;
}

int main(void)
{
    int fd = open("/dev/os2cmd", O_RDWR);
    if (fd < 0) {
        perror("os2cmd: /dev/os2cmd");
        return 1;
    }
    fputs("\033]0;OS/2 Command Prompt\007", stdout);   /* window title */
    fflush(stdout);

    char line[256];
    for (;;) {
        drain(fd);                                    /* output, then the prompt */
        size_t n = 0;
        int eof = 0;
        for (;;) {                                    /* one line from the terminal */
            char c;
            ssize_t r = read(0, &c, 1);
            if (r <= 0) { eof = 1; break; }
            if (c == '\n') break;
            if (n < sizeof line - 1) line[n++] = c;
        }
        line[n] = '\0';
        if (eof && !n) { fputs("\n", stdout); return 0; }
        if (is_exit(line)) return 0;
        if (write(fd, line, n ? n : 1) < 0) {        /* an empty line: just a new prompt */
            perror("os2cmd: write");
            return 1;
        }
    }
}
