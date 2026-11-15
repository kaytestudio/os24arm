/* userland/os2x86/test/ow/pmhello.c - a Presentation Manager program: a
 * standard window (frame + client), painting (WinFillRect, GpiCharStringAt,
 * GpiBox, WinDrawText), the mouse and keys, a timer, WinMessageBox. It
 * says what it gets on stdout, for run-tests.sh (os2x86's PM, headless). */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>
#include <string.h>

static int clicks, ticks, keys;
static char last[32] = "";

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        printf("WM_CREATE\n");
        WinStartTimer(WinQueryAnchorBlock(hwnd), hwnd, 1, 100);
        return 0;
    case WM_PAINT: {
        RECTL r;
        HPS hps = WinBeginPaint(hwnd, NULLHANDLE, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(hps, &r, CLR_WHITE);
        POINTL p;
        p.x = 20; p.y = r.yTop - 40;
        GpiSetColor(hps, CLR_BLUE);
        GpiCharStringAt(hps, &p, 21, "Hello, Presentation M");
        p.x = 20; p.y = 20;
        GpiMove(hps, &p);
        p.x = 120; p.y = 60;
        GpiSetColor(hps, CLR_RED);
        GpiBox(hps, DRO_FILL, &p, 0, 0);
        RECTL t;
        t.xLeft = 0; t.yBottom = 70; t.xRight = r.xRight; t.yTop = 100;
        WinDrawText(hps, -1, last, &t, CLR_BLACK, CLR_WHITE, DT_CENTER | DT_VCENTER);
        WinEndPaint(hps);
        printf("WM_PAINT %ldx%ld\n", r.xRight, r.yTop);
        return 0;
    }
    case WM_BUTTON1DOWN:
        clicks++;
        printf("WM_BUTTON1DOWN at %d,%d\n", SHORT1FROMMP(mp1), SHORT2FROMMP(mp1));
        sprintf(last, "click %d", clicks);
        WinInvalidateRect(hwnd, NULL, FALSE);
        return (MRESULT)TRUE;
    case WM_CHAR:
        if (!(SHORT1FROMMP(mp1) & KC_KEYUP) && (SHORT1FROMMP(mp1) & KC_CHAR)) {
            keys++;
            printf("WM_CHAR '%c'\n", SHORT1FROMMP(mp2));
            if (SHORT1FROMMP(mp2) == 'q') WinPostMsg(hwnd, WM_CLOSE, 0, 0);
        }
        return (MRESULT)TRUE;
    case WM_TIMER:
        if (++ticks == 3) {
            WinStopTimer(WinQueryAnchorBlock(hwnd), hwnd, 1);
            printf("WM_TIMER x3\n");
        }
        return 0;
    case WM_CLOSE:
        if (WinMessageBox(HWND_DESKTOP, hwnd, "Really quit?", "PM Hello", 0, MB_YESNO | MB_QUERY) == MBID_YES)
            WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    if (!hab || !hmq) { printf("no PM\n"); return 1; }
    WinRegisterClass(hab, "PmHello", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_MINMAX | FCF_TASKLIST | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmHello", "PM Hello", 0, NULLHANDLE, 0, &client);
    printf("frame %s, client %s\n", frame ? "yes" : "no", client ? "yes" : "no");
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    printf("done: %d clicks, %d keys, %d ticks\n", clicks, keys, ticks);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
