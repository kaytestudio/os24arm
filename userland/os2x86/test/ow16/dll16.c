/* userland/os2x86/test/ow16/dll16.c - a 16-bit OS/2 DLL: its own data
 * segment (__loadds: DS its own, the caller's put back), far Pascal
 * exports; its runtime's initialisation runs when it's loaded. Build:
 * wcl -bt=os2 -bd -ml -zu -zdp -l=os2_dll (-zu: SS isn't its data;
 * -zdp: exports save and restore the caller's DS) */
#define INCL_DOS
#include <os2.h>

static int counter = 1000;

int _export far pascal __loadds Add16(int a, int b) { return a + b; }
int _export far pascal __loadds Next16(void) { return ++counter; }
int _export far pascal __loadds Inited16(void) { return counter >= 1000; }
