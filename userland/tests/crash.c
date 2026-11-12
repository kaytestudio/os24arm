/* userland/tests/crash.c - the kernel must survive a faulting program.
 * crash null : NULL dereference      crash code : write to own code */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    printf("about to fault (%s)...\n", argc > 1 ? argv[1] : "null");
    if (argc > 1 && strcmp(argv[1], "code") == 0) {
        volatile unsigned *code = (volatile unsigned *)(void *)main;
        *code = 0;                                  /* code pages are read-only */
    } else {
        volatile int *p = 0;
        *p = 1;
    }
    printf("not reached\n");
    return 0;
}
