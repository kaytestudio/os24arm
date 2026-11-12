/* userland/tests/files.c - file I/O through the kernel's VFS.
 * Reads /BOOT/BOOTINFO.TXT (the FAT boot partition); if a writable volume
 * is mounted at /H it also creates, lists, renames and deletes there. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char buf[256];
    int fd = open("/BOOT/BOOTINFO.TXT", O_RDONLY);
    if (fd < 0) { perror("open /BOOT/BOOTINFO.TXT"); return 1; }
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    printf("read %zd bytes: %.40s...\n", n, buf);

    if (mkdir("/H/ELFTEST", 0755) != 0) { perror("mkdir /H/ELFTEST (MOUNT a volume at \\H)"); return 2; }
    FILE *f = fopen("/H/ELFTEST/hello.txt", "w");
    if (!f) { perror("fopen"); return 3; }
    for (int i = 0; i < 100; i++) fprintf(f, "line %d written by a Linux program\n", i);
    fclose(f);
    struct stat st;
    if (stat("/H/ELFTEST/hello.txt", &st) != 0) { perror("stat"); return 4; }
    printf("stat: %lld bytes, regular=%d\n", (long long)st.st_size, S_ISREG(st.st_mode));
    if (rename("/H/ELFTEST/hello.txt", "/H/ELFTEST/renamed.txt") != 0) { perror("rename"); return 5; }
    DIR *d = opendir("/H/ELFTEST");
    struct dirent *e;
    int entries = 0;
    while (d && (e = readdir(d))) { printf("  entry: %s (%s)\n", e->d_name, e->d_type == DT_DIR ? "dir" : "file"); entries++; }
    if (d) closedir(d);
    f = fopen("/H/ELFTEST/renamed.txt", "r");
    int lines = 0;
    while (f && fgets(buf, sizeof buf, f)) lines++;
    if (f) fclose(f);
    printf("read back %d lines\n", lines);
    if (chdir("/H/ELFTEST") != 0 || !getcwd(buf, sizeof buf)) { perror("chdir/getcwd"); return 6; }
    printf("cwd: %s\n", buf);
    if (unlink("renamed.txt") != 0) { perror("unlink"); return 7; }
    chdir("/");
    if (rmdir("/H/ELFTEST") != 0) { perror("rmdir"); return 8; }
    printf("files: %s\n", (entries == 3 && lines == 100) ? "ok" : "FAIL");
    return 0;
}
