/* userland/os2x86/test/ow/conio.c - conio: getch, kbhit, putch (16-bit KbdCharIn, KbdPeek, VioWrtTTY through clib's thunk) */
#include <conio.h>
#include <stdio.h>
int main(void)
{
    cputs("type two keys: ");
    int a = getch(), b = getch();
    cprintf("\r\ngot %d %d, kbhit now %d\r\n", a, b, kbhit());
    putch('!'); putch('\r'); putch('\n');
    return a == 'x' && b == 'y' ? 0 : 1;
}
