/* smp.c - several CPUs (kernel/src/smp.c): how many there are, threads
 * running on different ones at once (the time of N spinning threads close
 * to one's, not N times it), and the kernel's shared paths hit from all of
 * them together - mmap/munmap (TLB shootdowns while other threads run),
 * a mutex-protected counter (futex), fork + wait. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define MAXT 8
static volatile unsigned long sink[MAXT * 16];
static unsigned long spin_n;
static int cpu_seen[MAXT];

static void *spinner(void *arg)
{
    int i = (int)(long)arg;
    unsigned long x = (unsigned long)i + 1;
    for (unsigned long k = 0; k < spin_n; k++) x = x * 6364136223846793005UL + 1442695040888963407UL;
    sink[i * 16] = x;
    cpu_seen[i] = sched_getcpu();
    return 0;
}

static double run_spinners(int n)
{
    pthread_t t[MAXT];
    double t0 = now();
    for (int i = 0; i < n; i++) pthread_create(&t[i], 0, spinner, (void *)(long)i);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    return now() - t0;
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static int map_bad;

static void *churner(void *arg)
{
    int i = (int)(long)arg;
    for (int k = 0; k < 300; k++) {
        char *p = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { map_bad++; break; }
        memset(p, i + k, 3 * 4096);
        if (p[4096 + 7] != (char)(i + k)) map_bad++;
        mprotect(p, 4096, PROT_READ);
        if (p[0] != (char)(i + k)) map_bad++;
        munmap(p, 3 * 4096);
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    return 0;
}

static int forks_ok[MAXT];
static void *forker(void *arg)
{
    int i = (int)(long)arg, ok = 1;
    for (int k = 0; k < 10; k++) {
        pid_t c = fork();
        if (c == 0) _exit(i * 10 + k);
        int st;
        if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st) != i * 10 + k) ok = 0;
    }
    forks_ok[i] = ok;
    return 0;
}

int main(int argc, char **argv)
{
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    printf("CPUs: %d\n", n);
    if (n > MAXT) n = MAXT;
    CHECK(n >= 1, "sysconf(_SC_NPROCESSORS_ONLN) = %d", n);
    spin_n = argc > 1 ? strtoul(argv[1], 0, 0) : 500000000UL;

    double one = run_spinners(1), all = run_spinners(n);
    int distinct = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (cpu_seen[j] == cpu_seen[i]) dup = 1;
        if (!dup && cpu_seen[i] >= 0) distinct++;
    }
    double speedup = one * n / all;
    printf("one thread %.2f s, %d threads %.2f s: speedup %.1f, on %d different CPUs\n", one, n, all, speedup, distinct);
    /* (the host's other work shares its cores with QEMU's: clearly more than one at a time is the test) */
    if (n > 1) CHECK(speedup > 1.5, "%d threads ran in parallel (speedup %.1f of %d)", n, speedup, n);

    pthread_t t[MAXT];
    for (int i = 0; i < n; i++) pthread_create(&t[i], 0, churner, (void *)(long)i);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    CHECK(!map_bad && counter == 300L * n, "mmap/mprotect/munmap and a mutex from %d threads (%ld)", n, counter);

    for (int i = 0; i < n; i++) pthread_create(&t[i], 0, forker, (void *)(long)i);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    int fk = 1;
    for (int i = 0; i < n; i++) fk &= forks_ok[i];
    CHECK(fk, "fork + waitpid from %d threads at once", n);

    printf(fails ? "smp: %d FAILED\n" : "smp: all ok\n", fails);
    return fails != 0;
}
