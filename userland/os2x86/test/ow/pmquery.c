/* userland/os2x86/test/ow/pmquery.c - the pointer and points between
 * windows: WinGetKeyState (a button down, Shift), WinQueryPointerPos,
 * WinMapWindowPoints both ways, WinWindowFromPoint (a child button),
 * WinQueryClassName, WinIsChild, WinSetCapture/WinQueryCapture,
 * WinQueryWindowProcess. It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>

static HWND client, button;

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        button = WinCreateWindow(hwnd, WC_BUTTON, "Button", WS_VISIBLE | BS_PUSHBUTTON, 100, 20, 80, 30, hwnd, HWND_TOP, 5, NULL, NULL);
        return 0;
    case WM_BUTTON1DOWN: {
        POINTL p, d, back, b;
        char cls[16];
        PID pid; TID tid;
        p.x = SHORT1FROMMP(mp1); p.y = SHORT2FROMMP(mp1);
        WinQueryPointerPos(HWND_DESKTOP, &d);
        back = d;
        WinMapWindowPoints(HWND_DESKTOP, hwnd, &back, 1);
        printf("down %d, shift %d; at %ld %ld, mapped back %ld %ld\n", (WinGetKeyState(HWND_DESKTOP, VK_BUTTON1) & 0x8000) != 0,
               (WinGetKeyState(HWND_DESKTOP, VK_SHIFT) & 0x8000) != 0, p.x, p.y, back.x, back.y);
        b.x = 120; b.y = 30;
        {
            HWND h = WinWindowFromPoint(hwnd, &b, TRUE);
            HWND e = WinWindowFromPoint(hwnd, &p, TRUE);
            WinQueryClassName(button, sizeof cls, cls);
            printf("from point: %s, %s; class %s; child %d\n", h == button ? "the button" : "?", e == hwnd ? "the client" : "?", cls, WinIsChild(button, hwnd));
        }
        WinSetCapture(HWND_DESKTOP, hwnd);
        printf("capture %s\n", WinQueryCapture(HWND_DESKTOP) == hwnd ? "ours" : "?");
        WinSetCapture(HWND_DESKTOP, NULLHANDLE);
        WinQueryWindowProcess(hwnd, &pid, &tid);
        printf("thread %lu\n", tid);
        return (MRESULT)TRUE;
    }
    case WM_CLOSE: WinPostMsg(hwnd, WM_QUIT, 0, 0); return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_TASKLIST;
    WinRegisterClass(hab, "query", ClientProc, 0, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "query", "Query", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 240, 120, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
