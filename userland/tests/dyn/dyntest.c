/* userland/tests/dyn/dyntest.c - a dynamically linked program: the kernel
 * loads musl's dynamic linker (PT_INTERP) beside it, which loads libc.so
 * (itself) and libdyn.so; then dlopen/dlsym of libdyn2.so, TLS in a shared
 * library with threads, and execve of a dynamic program from one. Built
 * twice: at a fixed address and position-independent (dyntest-pie). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/wait.h>

int dyn_inited(void);
int dyn_add(int a, int b);
int *dyn_tls_addr(void);
extern int dyn_counter;

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

static int thread_saw;
static void *t_main(void *a)
{
    (void)a;
    thread_saw = *dyn_tls_addr();
    *dyn_tls_addr() = 99;
    return 0;
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) return dyn_add(2, 3) == 5 ? 5 : 1;
    CHECK(getauxval(AT_BASE) != 0, "a dynamic linker (AT_BASE %#lx), the program at %p", getauxval(AT_BASE), (void *)main);
    CHECK(dyn_inited(), "libdyn.so's constructor ran");
    int a = dyn_add(1, 2), b = dyn_add(1, 2);
    CHECK(a == 3 && b == 4 && dyn_counter == 42, "calls into libdyn.so, its data (%d %d %d)", a, b, dyn_counter);
    pthread_t t;
    *dyn_tls_addr() = 8;
    pthread_create(&t, 0, t_main, 0);
    pthread_join(t, 0);
    CHECK(thread_saw == 7 && *dyn_tls_addr() == 8, "TLS in a shared library: each thread its own (%d, %d)", thread_saw, *dyn_tls_addr());
    void *h = dlopen("libdyn2.so", RTLD_NOW);
    CHECK(h != 0, "dlopen libdyn2.so%s%s", h ? "" : ": ", h ? "" : dlerror());
    if (h) {
        int (*mul)(int, int) = (int (*)(int, int))dlsym(h, "dyn2_mul");
        int (*via)(int, int) = (int (*)(int, int))dlsym(h, "dyn2_add_via");
        CHECK(mul && via && mul(6, 7) == 42 && via(10, 0) == 12, "dlsym, and a call back into libdyn.so");
        CHECK(!dlsym(h, "no_such_symbol"), "dlsym of a missing symbol: %s", dlerror());
        dlclose(h);
    }
    int v[] = { 5, 3, 9, 1 };
    qsort(v, 4, sizeof v[0], cmp);
    CHECK(v[0] == 1 && v[3] == 9, "libc.so calling back into the program (qsort)");
    pid_t c = fork();
    if (c == 0) { execl("/proc/self/exe", argv[0], "child", (char *)0); _exit(1); }
    int st = 0;
    waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 5, "fork, and execve of a dynamic program (exit %d)", WEXITSTATUS(st));
    printf(fails ? "dyntest: %d FAILED\n" : "dyntest: all ok\n", fails);
    return fails != 0;
}
