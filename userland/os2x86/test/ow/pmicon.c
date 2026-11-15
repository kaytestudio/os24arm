/* userland/os2x86/test/ow/pmicon.c - PM's icons: WinLoadPointer (an
 * array, colour, masks), WinDrawPointer (normal, DP_MINI), a system icon
 * (WinQuerySysPointer), an SS_ICON control, WinMessageBox's icon.
 * run-tests.sh checks the pixels of its window. */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>

static HPOINTER p1, p2, pinfo;
static HWND hstatic;

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        hstatic = WinCreateWindow(hwnd, WC_STATIC, "#2", WS_VISIBLE | SS_ICON, 250, 10, 32, 32, hwnd, HWND_TOP, 9, NULL, NULL);
        return 0;
    case WM_PAINT: {
        RECTL r;
        HPS hps = WinBeginPaint(hwnd, NULLHANDLE, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(hps, &r, CLR_PALEGRAY);
        WinDrawPointer(hps, 10, 10, p1, DP_NORMAL);
        WinDrawPointer(hps, 60, 10, p2, DP_NORMAL);
        WinDrawPointer(hps, 110, 10, p1, DP_MINI);
        WinDrawPointer(hps, 150, 10, pinfo, DP_NORMAL);
        WinEndPaint(hps);
        return 0;
    }
    case WM_CHAR:
        if (!(SHORT1FROMMP(mp1) & KC_KEYUP) && SHORT1FROMMP(mp2) == 'm')
            printf("WinMessageBox %lu\n", WinMessageBox(HWND_DESKTOP, hwnd, "Careful!", "Icons", 0, MB_OK | MB_ICONEXCLAMATION));
        return (MRESULT)TRUE;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    p1 = WinLoadPointer(HWND_DESKTOP, NULLHANDLE, 1);
    p2 = WinLoadPointer(HWND_DESKTOP, NULLHANDLE, 2);
    pinfo = WinQuerySysPointer(HWND_DESKTOP, SPTR_ICONINFORMATION, FALSE);
    printf("pointers %s %s %s\n", p1 ? "yes" : "no", p2 ? "yes" : "no", pinfo ? "yes" : "no");
    WinRegisterClass(hab, "PmIcon", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmIcon", "PM Icons", 0, NULLHANDLE, 0, &client);
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyPointer(p1);
    WinDestroyPointer(p2);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
