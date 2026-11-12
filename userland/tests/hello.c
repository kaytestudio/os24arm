/* userland/tests/hello.c - printf, argv, environment, uname, isatty */
#include <stdio.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct utsname u;
    printf("hello from EL0: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    printf("  TERM=%s  pi=%.5f  2^40=%lld\n", getenv("TERM"), 3.14159265, 1LL << 40);
    if (uname(&u) == 0) printf("  uname: %s %s %s\n", u.sysname, u.release, u.machine);
    printf("  isatty(1)=%d  pid=%d\n", isatty(1), (int)getpid());
    return 7;
}
