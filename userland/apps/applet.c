/*
 * applet - a stand-in for a program that busybox provides: runs the
 * system's busybox (/h/OS2/busybox) as the applet named after itself.
 * On the stick it is UNIXROOT's /bin/sh (userland/make-stick.sh), which
 * make, system() and popen() look for before they run a command.
 */
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    const char *self = argv[0] ? argv[0] : "sh";
    const char *base = strrchr(self, '/');
    argv[0] = (char *)(base ? base + 1 : self);       /* "sh": busybox's applet */
    execv("/h/OS2/busybox", argv);
    static const char msg[] = "applet: /h/OS2/busybox is missing\n";
    write(2, msg, sizeof msg - 1);
    return 127;
}
