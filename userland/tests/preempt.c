/* preempt.c - stage 6 test: preemptive scheduling. Two children spin
 * without making system calls; the parent must still wake from its sleep
 * (the timer tick takes the CPU away from them), see both make progress,
 * and be able to kill them. */
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

int main(void)
{
    volatile unsigned long *count = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pid_t kid[2];
    for (int i = 0; i < 2; i++)
        if ((kid[i] = fork()) == 0) for (;;) count[i]++;        /* no syscalls, ever */
    struct timespec t0, t1, d = { 0, 300000000 };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    nanosleep(&d, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    unsigned long a = count[0], b = count[1];
    int fails = 0;
    printf("%s  parent woke after %.3f s (asked 0.300)\n", dt < 0.6 ? "ok   " : "FAIL ", dt);
    fails += dt >= 0.6;
    printf("%s  both spinners ran: %lu and %lu iterations\n", a && b ? "ok   " : "FAIL ", a, b);
    fails += !(a && b);
    for (int i = 0; i < 2; i++) kill(kid[i], SIGKILL);
    int st, killed = 0;
    for (int i = 0; i < 2; i++) if (waitpid(kid[i], &st, 0) == kid[i] && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL) killed++;
    printf("%s  spinners killed (%d)\n", killed == 2 ? "ok   " : "FAIL ", killed);
    fails += killed != 2;
    printf("preempt: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
