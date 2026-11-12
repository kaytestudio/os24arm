/* cat.c - copy stdin (or files) to stdout; tests console line input,
 * Ctrl-D (end of file) and Ctrl-C (SIGINT). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static int copy(int fd)
{
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write(1, buf, n) != n) return 1;
    return n < 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return copy(0);
    int r = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { perror(argv[i]); r = 1; continue; }
        r |= copy(fd);
        close(fd);
    }
    return r;
}
