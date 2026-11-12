/* threads.c - pthreads on the Linux-ABI user mode: clone(CLONE_THREAD),
 * futex, TLS, join, exit/exit_group, execve and signals with threads. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern char **environ;
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter;
static __thread int tls_val;
static int tids[4], tls_ok[4];

static void *adder(void *arg)
{
    int i = (int)(long)arg;
    tls_val = 100 + i;
    tids[i] = (int)syscall(SYS_gettid);
    for (int k = 0; k < 10000; k++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
        if (k % 1000 == 0) sched_yield();
    }
    tls_ok[i] = tls_val == 100 + i;
    return (void *)(long)(i * 7);
}

static int queue[8], qn, done;
static void *producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 50; i++) {
        pthread_mutex_lock(&mu);
        while (qn == 8) pthread_cond_wait(&cv, &mu);
        queue[qn++] = i;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    done = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 0;
}

static volatile unsigned long spins[2];
static volatile int stop_spin;
static void *spinner(void *arg) { int i = (int)(long)arg; while (!stop_spin) spins[i]++; return 0; }

static void *blocker(void *arg) { (void)arg; for (;;) pause(); return 0; }
static void *exiter(void *arg) { (void)arg; usleep(20000); exit(5); return 0; }

static volatile int got_usr1;
static void on_usr1(int s) { (void)s; got_usr1++; }

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "exec-ok")) { printf("      exec'd image running, pid %d\n", getpid()); return 33; }

    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, adder, (void *)i);
    long sum = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); sum += (long)r; }
    CHECK(counter == 40000, "mutex: 4 threads x 10000 increments = %ld", counter);
    CHECK(sum == 42, "pthread_join return values (sum %ld)", sum);
    int distinct = tids[0] != tids[1] && tids[1] != tids[2] && tids[2] != tids[3] && tids[0] != getpid();
    CHECK(distinct, "distinct thread ids %d %d %d %d, pid %d", tids[0], tids[1], tids[2], tids[3], getpid());
    CHECK(tls_ok[0] && tls_ok[1] && tls_ok[2] && tls_ok[3], "__thread variables are per thread");

    pthread_t p;
    pthread_create(&p, 0, producer, 0);
    int got = 0, total = 0;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!qn && !done) pthread_cond_wait(&cv, &mu);
        if (!qn && done) break;
        for (int i = 0; i < qn; i++) { total += queue[i]; got++; }
        qn = 0;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    pthread_join(p, 0);
    CHECK(got == 50 && total == 1275, "condition variable: %d items, sum %d", got, total);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&mu);
    int r = pthread_cond_timedwait(&cv, &mu, &ts);
    pthread_mutex_unlock(&mu);
    CHECK(r == ETIMEDOUT, "pthread_cond_timedwait times out (%d)", r);

    pthread_t s1, s2;
    pthread_create(&s1, 0, spinner, (void *)0);
    pthread_create(&s2, 0, spinner, (void *)1);
    usleep(200000);
    stop_spin = 1;
    pthread_join(s1, 0); pthread_join(s2, 0);
    CHECK(spins[0] && spins[1], "two spinning threads both ran (%lu, %lu)", spins[0], spins[1]);

    pid_t c = fork();
    if (c == 0) {                       /* exit() in a thread ends the process */
        pthread_t b, e;
        pthread_create(&b, 0, blocker, 0);
        pthread_create(&e, 0, exiter, 0);
        pthread_join(b, 0);
        _exit(1);
    }
    int st;
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 5, "exit() from a thread: process status %d", WEXITSTATUS(st));

    c = fork();
    if (c == 0) {                       /* execve with other threads around */
        pthread_t b;
        pthread_create(&b, 0, blocker, 0);
        pthread_create(&b, 0, blocker, 0);
        usleep(10000);
        char *a[] = { argv[0], "exec-ok", 0 };
        execve(argv[0], a, environ);
        _exit(2);
    }
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 33, "execve from a multithreaded process: status %d", WEXITSTATUS(st));

    signal(SIGUSR1, on_usr1);
    pthread_t b;
    pthread_create(&b, 0, blocker, 0);
    kill(getpid(), SIGUSR1);
    usleep(10000);
    CHECK(got_usr1 == 1, "signal to a multithreaded process handled once (%d)", got_usr1);
    c = fork();
    if (c == 0) { pthread_t x; pthread_create(&x, 0, blocker, 0); pause(); _exit(0); }
    usleep(20000);
    kill(c, SIGTERM);
    waitpid(c, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "SIGTERM ends a multithreaded process");

    printf("threads: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;                       /* the blocker thread goes with the process */
}
