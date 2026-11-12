/* userland/tests/timetest.c - clock_gettime, nanosleep */
#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec a, b, rt;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec s = { 0, 250 * 1000 * 1000 };
    nanosleep(&s, 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    clock_gettime(CLOCK_REALTIME, &rt);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", gmtime(&rt.tv_sec));
    printf("slept %ld ms (asked 250), UTC now %s\n", ms, when);
    return (ms >= 245 && ms <= 400) ? 0 : 1;
}
