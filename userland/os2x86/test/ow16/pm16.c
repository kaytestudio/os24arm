/* userland/os2x86/test/ow16/pm16.c - a 16-bit OS/2 1.x Presentation
 * Manager program: a frame and client window (its 16-bit window
 * procedure), painting (WinBeginPaint, WinFillRect, GpiCharStringAt,
 * GpiBox), a timer, a key and a click, WinMessageBox's answer, the
 * messages it gets printed for run-tests.sh. */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>

/* (not in OpenWatcom's 16-bit headers) */
USHORT APIENTRY WinStartTimer(HAB, HWND, USHORT, USHORT);
BOOL APIENTRY WinStopTimer(HAB, HWND, USHORT);

static HAB hab;
static int timers;

MRESULT EXPENTRY ClientProc(HWND hwnd, USHORT msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        printf("WM_CREATE\n");
        WinStartTimer(hab, hwnd, 1, 50);
        return 0;
    case WM_SIZE:
        printf("WM_SIZE %d x %d\n", SHORT1FROMMP(mp2), SHORT2FROMMP(mp2));
        return 0;
    case WM_PAINT: {
        RECTL r;
        POINTL p;
        HPS ps = WinBeginPaint(hwnd, NULL, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(ps, &r, CLR_WHITE);
        GpiSetColor(ps, CLR_RED);
        p.x = 20; p.y = 20; GpiMove(ps, &p);
        p.x = 120; p.y = 60; GpiBox(ps, DRO_FILL, &p, 0L, 0L);
        GpiSetColor(ps, CLR_BLUE);
        p.x = 20; p.y = 90;
        GpiCharStringAt(ps, &p, 16L, "16-bit PM, here!");
        WinEndPaint(ps);
        printf("WM_PAINT\n");
        return 0;
    }
    case WM_TIMER:
        if (++timers == 3) { WinStopTimer(hab, hwnd, 1); printf("WM_TIMER x3\n"); }
        return 0;
    case WM_CHAR:
        if (!(SHORT1FROMMP(mp1) & KC_KEYUP) && (SHORT1FROMMP(mp1) & KC_CHAR)) printf("WM_CHAR %c\n", (char)SHORT1FROMMP(mp2));
        return (MRESULT)TRUE;
    case WM_BUTTON1DOWN:
        printf("WM_BUTTON1DOWN %d %d\n", SHORT1FROMMP(mp1), SHORT2FROMMP(mp1));
        return (MRESULT)TRUE;
    case WM_CLOSE:
        printf("WM_CLOSE, message box %d\n", WinMessageBox(HWND_DESKTOP, hwnd, "Quit?", "16-bit", 0, MB_YESNO));
        WinPostMsg(hwnd, WM_QUIT, 0L, 0L);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HMQ hmq;
    HWND frame, client;
    QMSG q;
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_TASKLIST;
    hab = WinInitialize(0);
    hmq = WinCreateMsgQueue(hab, 0);
    WinRegisterClass(hab, "pm16", ClientProc, CS_SIZEREDRAW, 0);
    frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "pm16", "16-bit PM", 0L, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 300, 160, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    printf("done\n");
    return 0;
}
