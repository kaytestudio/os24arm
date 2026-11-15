/* userland/os2x86/test/ow16/use16.c - a 16-bit program and its own 16-bit
 * DLL (dll16.c): imported calls, then DosLoadModule and DosGetProcAddr
 * (by name), DosGetModHandle and DosGetModName; DosExecPgm (a 16-bit child,
 * its exit code back) */
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
#include <string.h>

int far pascal Add16(int a, int b);
int far pascal Next16(void);
int far pascal Inited16(void);

int main(void)
{
    HMODULE h;
    PFN fn;
    char fail[32], name[260];
    int a, b, c;
    a = Add16(40, 2);
    b = Next16(); c = Next16();
    printf("imported: %d, %d %d, inited %d\n", a, b, c, Inited16());
    if (DosLoadModule(fail, sizeof fail, "DLL16", &h)) { printf("DosLoadModule failed: %s\n", fail); return 1; }
    if (DosGetProcAddr(h, "NEXT16", &fn)) { printf("DosGetProcAddr failed\n"); return 1; }
    a = ((int (far pascal *)(void))fn)();
    printf("by name: %d\n", a);
    if (!DosGetModHandle("DLL16", &h) && !DosGetModName(h, sizeof name, name))
        printf("module: %s\n", strstr(name, "DLL16") || strstr(name, "dll16") ? "DLL16.DLL" : name);
    if (DosLoadModule(fail, sizeof fail, "NOSUCH", &h)) printf("NOSUCH: not found (%s)\n", fail);
    {   /* a child: test16 with its two arguments; its exit code back */
        RESULTCODES rc;
        static char args[] = "test16\0one \"two words\"\0";
        if (!DosExecPgm(fail, sizeof fail, EXEC_SYNC, args, NULL, &rc, "test16.exe"))
            printf("child: %u %u\n", rc.codeTerminate, rc.codeResult);
        else printf("DosExecPgm failed: %s\n", fail);
    }
    return 0;
}
