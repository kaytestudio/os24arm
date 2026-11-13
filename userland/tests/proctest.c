/* proctest.c - /proc: the files ps, top and free read, the directory
 * listing, the self link, cmdline, CPU time. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

static int slurp(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY), k = 0, r;
    if (fd < 0) return -1;
    while (k < n - 1 && (r = read(fd, buf + k, n - 1 - k)) > 0) k += r;
    buf[k] = 0;
    close(fd);
    return k;
}

int main(int argc, char **argv)
{
    (void)argc;
    char buf[4096], path[64], link[64];
    int pid = getpid();
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    CHECK(slurp(path, buf, sizeof buf) > 0, "%s readable", path);
    int spid = 0, ppid = -1; char comm[32] = "", st = 0;
    sscanf(buf, "%d (%31[^)]) %c %d", &spid, comm, &st, &ppid);
    CHECK(spid == pid && st == 'R' && ppid == getppid() && !strcmp(comm, "proctest"),
          "stat: pid %d (%s) %c ppid %d", spid, comm, st, ppid);
    ssize_t l = readlink("/proc/self", link, sizeof link - 1);
    link[l > 0 ? l : 0] = 0;
    CHECK(atoi(link) == pid, "/proc/self -> %s", link);
    l = readlink("/proc/self/exe", link, sizeof link - 1);
    link[l > 0 ? l : 0] = 0;
    CHECK(l > 0 && !strcmp(link, argv[0]), "/proc/self/exe -> %s", link);
    int n = slurp("/proc/self/cmdline", buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, argv[0]), "cmdline starts with argv[0] (%d bytes)", n);
    CHECK(slurp("/proc/meminfo", buf, sizeof buf) > 0 && strstr(buf, "MemTotal:") && strstr(buf, "MemFree:"),
          "meminfo");
    unsigned long long u = 0, ni = 0, sy = 0, id = 0;
    CHECK(slurp("/proc/stat", buf, sizeof buf) > 0 && sscanf(buf, "cpu %llu %llu %llu %llu", &u, &ni, &sy, &id) == 4,
          "stat: cpu %llu user, %llu idle", u, id);
    int found_self = 0, found_pid = 0;
    DIR *d = opendir("/proc");
    for (struct dirent *e; d && (e = readdir(d)); ) {
        if (!strcmp(e->d_name, "self")) found_self = 1;
        if (atoi(e->d_name) == pid) found_pid = 1;
    }
    if (d) closedir(d);
    CHECK(found_self && found_pid, "/proc lists self and %d", pid);
    int fds = 0;
    d = opendir("/proc/self/fd");
    for (struct dirent *e; d && (e = readdir(d)); ) if (e->d_name[0] != '.') fds++;
    if (d) closedir(d);
    CHECK(fds >= 3, "/proc/self/fd: %d entries", fds);
    struct sysinfo si;
    CHECK(sysinfo(&si) == 0 && si.totalram > si.freeram && si.procs >= 1, "sysinfo: %lu MB total, %lu MB free",
          si.totalram >> 20, si.freeram >> 20);
    volatile unsigned long spin = 0;
    for (unsigned long i = 0; i < 200000000UL; i++) spin += i;
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    slurp(path, buf, sizeof buf);
    char *p = strrchr(buf, ')');
    unsigned long ut = 0;
    if (p) sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ut);
    CHECK(ut > 0, "CPU time charged after spinning: %lu jiffies", ut);
    CHECK(access("/proc/nonexistent", F_OK) == -1 && chdir("/proc") == 0 && access("meminfo", R_OK) == 0,
          "access/chdir in /proc");
    printf("proctest: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
