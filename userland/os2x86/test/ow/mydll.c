/* userland/os2x86/test/ow/mydll.c - a DLL (moved: it's linked where the program is), its initialisation */
#include <stdio.h>
static int inits;
int __dll_initialize(void) { inits++; return 1; }
int __export add3(int a, int b, int c) { return a + b + c + inits * 1000; }
const char * __export greet(void) { return "hello from MYDLL"; }
