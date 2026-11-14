/* fsize.c - an open file's size, on any volume: fsize [dir] (default /tmp).
 * Writes in odd-sized pieces, as a download does, then checks that fstat
 * sees the size while the file is still open, that ftruncate to that same
 * size changes nothing (busybox wget does it at the end of every download:
 * on HPFS it used to write zeros over the file), and that the data reads
 * back as written. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "/tmp";
    char f[256];
    snprintf(f, sizeof f, "%s/fsize.tmp", base);
    static unsigned char buf[50000], back[50000];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)(i * 7 + 13);
    int fails = 0;
    int fd = open(f, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { printf("FAIL create %s: %d\n", f, errno); return 1; }
    unsigned done = 0;
    static const unsigned piece[] = { 1460, 2920, 333, 4096, 1, 1460 };
    for (int k = 0; done < sizeof buf; k++) {
        unsigned n = piece[k % 6];
        if (n > sizeof buf - done) n = sizeof buf - done;
        if (write(fd, buf + done, n) != (ssize_t)n) { printf("FAIL write at %u: %d\n", done, errno); fails++; break; }
        done += n;
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_size != (off_t)sizeof buf) {
        printf("FAIL fstat while open: %lld, not %u\n", (long long)st.st_size, (unsigned)sizeof buf);
        fails++;
    }
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (ftruncate(fd, pos)) { printf("FAIL ftruncate to %lld: %d\n", (long long)pos, errno); fails++; }
    close(fd);
    fd = open(f, O_RDONLY);
    ssize_t r = fd < 0 ? -1 : read(fd, back, sizeof back);
    if (fd >= 0) close(fd);
    if (r != (ssize_t)sizeof buf || memcmp(buf, back, sizeof buf)) {
        unsigned at = 0;
        while (r > 0 && at < (unsigned)r && buf[at] == back[at]) at++;
        printf("FAIL read back: %zd bytes, differs at %u\n", r, at);
        fails++;
    }
    unlink(f);
    printf("%s\n", fails ? "fsize: FAILED" : "fsize: PASS");
    return fails != 0;
}
