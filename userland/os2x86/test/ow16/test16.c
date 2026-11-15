/* userland/os2x86/test/ow16/test16.c - a 16-bit OS/2 1.x program (NE,
 * OpenWatcom's 16-bit C runtime): files (write, seek, read, size),
 * directories (make, change, list with DosFindFirst/Next, remove), the
 * time, the environment, memory (malloc, a segment of its own),
 * floating point, its arguments and exit code. It prints what it gets;
 * "passed" at the end when each check holds. */
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <direct.h>

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad++; } }

int main(int argc, char **argv)
{
    FILE *f;
    char buf[128];
    long n;
    int i;
    /* files */
    f = fopen("t16.tmp", "wb");
    check(f != NULL, "fopen for writing");
    for (i = 0; i < 100; i++) fprintf(f, "line %03d\n", i);
    fclose(f);
    f = fopen("t16.tmp", "rb");
    check(f != NULL, "fopen for reading");
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    check(n == 900, "file size");
    fseek(f, 9 * 42, SEEK_SET);
    fgets(buf, sizeof buf, f);
    check(!strcmp(buf, "line 042\n"), "seek and read");
    fclose(f);
    check(remove("t16.tmp") == 0, "remove");
    /* directories, the 16-bit DosFindFirst/Next */
    check(mkdir("t16dir") == 0, "mkdir");
    check(chdir("t16dir") == 0, "chdir");
    f = fopen("a.txt", "w"); fclose(f);
    f = fopen("b.txt", "w"); fclose(f);
    {
        HDIR h = HDIR_CREATE;
        FILEFINDBUF fb;
        USHORT cnt = 1, found = 0;
        if (!DosFindFirst("*.txt", &h, FILE_NORMAL, &fb, sizeof fb, &cnt, 0L)) {
            do found++; while (cnt = 1, !DosFindNext(h, &fb, sizeof fb, &cnt));
            DosFindClose(h);
        }
        check(found == 2, "DosFindFirst/Next");
    }
    remove("a.txt"); remove("b.txt");
    chdir("..");
    check(rmdir("t16dir") == 0, "rmdir");
    /* the rest */
    check(time(NULL) > 1700000000L, "time");
    check(getenv("PATH") != NULL, "getenv");
    {
        char *p = malloc(20000);
        SEL sel;
        check(p != NULL, "malloc");
        if (p) { memset(p, 'x', 20000); free(p); }
        check(DosAllocSeg(4096, &sel, 0) == 0, "DosAllocSeg");
        if (!bad) { char far *q = MAKEP(sel, 0); q[100] = 'z'; check(q[100] == 'z', "a segment of its own"); DosFreeSeg(sel); }
    }
    {
        double d = 0;
        for (i = 1; i <= 10; i++) d += 1.0 / i;
        sprintf(buf, "%.6f", d);
        check(!strcmp(buf, "2.928968"), "floating point");
    }
    check(argc == 3 && !strcmp(argv[2], "two words"), "arguments");
    printf("%s\n", bad ? "failed" : "passed");
    return bad ? 1 : 42;
}
