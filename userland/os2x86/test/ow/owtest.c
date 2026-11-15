/* userland/os2x86/test/ow/owtest.c - the OpenWatcom C runtime through os2x86:
 * files, directories, maths, printf, the heap, signals (OS/2 exceptions),
 * _beginthread, spawnl. Built by ../run-tests.sh when OpenWatcom is there. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <setjmp.h>
#include <signal.h>
#include <process.h>
#include <math.h>
#include <ctype.h>
#define INCL_DOS
#include <os2.h>

static int fails;
#define CHECK(c, what) do { int _c = (c); printf("%s %s\n", _c ? "ok" : "FAIL", what); if (!_c) fails++; } while (0)

static jmp_buf jb;
static volatile int sigs;
static void on_fpe(int s) { (void)s; sigs++; longjmp(jb, 1); }
static void on_segv(int s) { (void)s; sigs += 10; longjmp(jb, 2); }

static volatile int tcount;
static void thr(void *arg) { int n = (int)arg; for (int i = 0; i < n; i++) tcount++; }

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) { printf("  child here\n"); return 3; }

    /* files */
    FILE *f = fopen("owtest.tmp", "w");
    CHECK(f != NULL, "fopen w");
    for (int i = 0; i < 100; i++) fprintf(f, "line %d %f\n", i, i * 0.5);
    fclose(f);
    f = fopen("OWTEST.TMP", "r");
    char line[80]; int n = 0;
    while (fgets(line, sizeof line, f)) n++;
    fclose(f);
    CHECK(n == 100, "fgets 100 lines (any case)");
    struct stat st;
    CHECK(stat("owtest.tmp", &st) == 0 && st.st_size > 1000, "stat");
    int fd = open("owtest.tmp", O_RDONLY | O_BINARY);
    CHECK(fd >= 0 && lseek(fd, 0, SEEK_END) == st.st_size, "open/lseek");
    close(fd);
    CHECK(rename("owtest.tmp", "owtest2.tmp") == 0 && access("owtest2.tmp", 0) == 0, "rename");
    CHECK(remove("owtest2.tmp") == 0, "remove");

    /* directories */
    char cwd[260];
    CHECK(getcwd(cwd, sizeof cwd) != NULL, "getcwd");
    printf("  cwd %s\n", cwd);
    CHECK(mkdir("owdir") == 0 && chdir("owdir") == 0, "mkdir, chdir");
    fclose(fopen("a.txt", "w"));
    fclose(fopen("b.txt", "w"));
    CHECK(chdir("..") == 0, "chdir ..");
    DIR *d = opendir("owdir");
    int ents = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) ents++;
    if (d) closedir(d);
    CHECK(ents == 4, "opendir/readdir (., .., a.txt, b.txt)");
    CHECK(remove("owdir\\a.txt") == 0 && remove("owdir/b.txt") == 0 && rmdir("owdir") == 0, "remove, rmdir");

    /* maths, strings, sorting, time */
    CHECK(fabs(sin(1.0) - 0.841470985) < 1e-8 && fabs(sqrt(2.0) - 1.414213562) < 1e-8, "sin, sqrt");
    CHECK(fabs(pow(2.0, 10.5) - 1448.154688) < 1e-5 && fabs(log(10.0) - 2.302585093) < 1e-8, "pow, log");
    CHECK(fabs(atan2(1.0, 1.0) * 4 - 3.14159265358979) < 1e-12 && fabs(exp(1.0) - 2.718281828) < 1e-8, "atan2, exp");
    char b[64];
    sprintf(b, "%.2e|%8.3f|%-5d|%x", 12345.678, -3.14159, 42, 0xBEEF);
    CHECK(!strcmp(b, "1.23e+04|  -3.142|42   |beef"), "sprintf");
    double v = strtod("  -1.5e3x", NULL);
    CHECK(v == -1500.0 && atoi("77") == 77, "strtod, atoi");
    int a[] = { 5, 3, 9, 1, 7 };
    qsort(a, 5, sizeof a[0], cmp);
    CHECK(a[0] == 1 && a[4] == 9, "qsort");
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    CHECK(t > 1600000000 && tm && tm->tm_year > 120, "time, localtime");
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", tm);
    printf("  now %s\n", b);
    long long big = 1234567890123LL;
    CHECK(big / 1000 == 1234567890LL && big % 1000 == 123, "64-bit division");

    /* memory */
    char *p = malloc(1 << 20);
    CHECK(p != NULL, "malloc 1 MB");
    memset(p, 'x', 1 << 20);
    p = realloc(p, 4 << 20);
    CHECK(p && p[1000] == 'x', "realloc 4 MB");
    free(p);

    /* signals: SIGFPE, SIGSEGV through OS/2 exceptions */
    signal(SIGIDIVZ, on_fpe);
    signal(SIGSEGV, on_segv);
    volatile int zero = 0;
    if (!setjmp(jb)) { volatile int x = 5 / zero; (void)x; }
    CHECK(sigs == 1, "SIGIDIVZ (divide by zero)");
    if (!setjmp(jb)) { *(volatile int *)0 = 1; }
    CHECK(sigs == 11, "SIGSEGV (NULL)");

    /* threads */
    int t1 = _beginthread(thr, NULL, 65536, (void *)1000);
    int t2 = _beginthread(thr, NULL, 65536, (void *)1000);
    CHECK(t1 > 0 && t2 > 0, "_beginthread");
    TID w1 = t1, w2 = t2;
    DosWaitThread(&w1, DCWW_WAIT);
    DosWaitThread(&w2, DCWW_WAIT);
    CHECK(tcount == 2000, "threads ran");

    /* children */
    int r = spawnl(P_WAIT, argv[0], argv[0], "child", NULL);
    CHECK(r == 3, "spawnl P_WAIT");
    printf("  (spawn result %d)\n", r);

    printf(fails ? "some tests failed\n" : "all passed\n");
    return fails;
}
