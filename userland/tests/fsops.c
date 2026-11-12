/* fsops.c - directory operations on any volume: fsops [dir] (default /tmp) */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "/tmp";
    char d[256], f[256];
    snprintf(d, sizeof d, "%s/fsopsdir", base);
    snprintf(f, sizeof f, "%s/file.txt", d);
    int fails = 0;
    if (mkdir(d, 0755)) { printf("FAIL mkdir %s: %d\n", d, errno); fails++; }
    int fd = open(f, O_CREAT | O_WRONLY, 0644);
    if (fd < 0 || write(fd, "abc", 3) != 3) { printf("FAIL create %s: %d\n", f, errno); fails++; }
    close(fd);
    if (rmdir(d) == 0 || errno != ENOTEMPTY) { printf("FAIL rmdir non-empty: %d\n", errno); fails++; }
    if (unlink(f)) { printf("FAIL unlink: %d\n", errno); fails++; }
    struct stat st;
    if (stat(f, &st) == 0) { printf("FAIL file still there\n"); fails++; }
    if (rmdir(d)) { printf("FAIL rmdir empty: %d\n", errno); fails++; }
    printf("fsops %s: %s\n", base, fails ? "FAILED" : "ok");
    return fails;
}
