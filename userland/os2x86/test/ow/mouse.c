/* userland/os2x86/test/ow/mouse.c - MouOpen, MouReadEventQue & co. (16-bit,
 * through clib's thunk), reading the terminal's mouse reports that
 * run-tests.sh feeds it, with a key in between for getch */
#define INCL_MOU
#define INCL_KBD
#include <os2.h>
#include <conio.h>
#include <stdio.h>
int main(void)
{
    HMOU h;
    USHORT n, wait = MOU_WAIT, r;
    MOUEVENTINFO e;
    MOUQUEINFO q;
    int fails = 0;
    r = MouOpen(0, &h);
    MouGetNumButtons(&n, h);
    printf("MouOpen %u, %u buttons\n", r, n);
    if (r || n != 3) fails++;
    MouReadEventQue(&e, &wait, h);
    printf("event fs=%x row=%d col=%d\n", e.fs, e.row, e.col);
    if (e.fs != MOUSE_BN1_DOWN || e.row != 4 || e.col != 9) fails++;
    int k = getch();
    printf("key %c\n", k);
    if (k != 'k') fails++;
    MouReadEventQue(&e, &wait, h);
    printf("event fs=%x row=%d col=%d\n", e.fs, e.row, e.col);
    if (e.fs != MOUSE_MOTION_WITH_BN1_DOWN || e.row != 5 || e.col != 11) fails++;
    MouGetNumQueEl(&q, h);
    printf("%u more queued\n", q.cEvents);
    if (q.cEvents != 2) fails++;
    MouReadEventQue(&e, &wait, h);
    if (e.fs != 0) fails++;
    MouReadEventQue(&e, &wait, h);
    printf("event fs=%x row=%d col=%d\n", e.fs, e.row, e.col);
    if (e.fs != MOUSE_BN2_DOWN || e.row != 2 || e.col != 2) fails++;
    USHORT nowait = MOU_NOWAIT;
    MouReadEventQue(&e, &nowait, h);
    if (e.fs != 0 || e.time != 0) fails++;
    MouClose(h);
    printf(fails ? "mouse: FAIL\n" : "mouse: ok\n");
    return fails;
}
