/* userland/os2x86/test/ow/usedll.c - MYDLL.DLL at load time, then DosLoadModule and DosQueryProcAddr */
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
int add3(int, int, int);
const char *greet(void);
int main(void)
{
    printf("%s, add3 = %d\n", greet(), add3(1, 2, 3));
    HMODULE h; PFN fn; char bad[64];
    APIRET r = DosLoadModule(bad, sizeof bad, "MYDLL", &h);
    APIRET r2 = r ? r : DosQueryProcAddr(h, 0, "add3_", &fn);
    printf("DosLoadModule %lu, DosQueryProcAddr %lu\n", r, r2);
    if (!r2) printf("through the pointer: %d\n", ((int (*)(int, int, int))fn)(10, 20, 30));
    return 0;
}
