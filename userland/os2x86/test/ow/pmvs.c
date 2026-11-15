/* userland/os2x86/test/ow/pmvs.c - PM's value set (colours and text,
 * VM_SETITEM, VM_SETITEMATTR, VM_SELECTITEM, VN_SELECT, VN_ENTER) and
 * circular slider (CSM_SETRANGE, CSM_SETVALUE, CSN_CHANGED, CSN_TRACKING).
 * It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>

static HWND vs, cs;
static const char *names[] = { "Red", "Green", "Blue" };
static const ULONG rgbs[] = { 0xFF0000, 0x00C000, 0x0000FF };

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE: {
        VSCDATA vd = { sizeof vd, 2, 3 };
        vs = WinCreateWindow(hwnd, WC_VALUESET, "", WS_VISIBLE | VS_RGB | VS_BORDER | VS_ITEMBORDER, 10, 120, 220, 80, hwnd, HWND_TOP, 1, &vd, NULL);
        for (USHORT c = 1; c <= 3; c++) {
            WinSendMsg(vs, VM_SETITEM, MPFROM2SHORT(1, c), MPFROMLONG(rgbs[c - 1]));
            WinSendMsg(vs, VM_SETITEMATTR, MPFROM2SHORT(2, c), MPFROM2SHORT(VIA_TEXT, TRUE));
            WinSendMsg(vs, VM_SETITEM, MPFROM2SHORT(2, c), MPFROMP(names[c - 1]));
        }
        WinSendMsg(vs, VM_SELECTITEM, MPFROM2SHORT(1, 1), 0);
        char buf[20];
        VSTEXT vt;
        vt.pszItemText = buf; vt.ulBufLen = sizeof buf;
        ULONG n = (ULONG)WinSendMsg(vs, VM_QUERYITEM, MPFROM2SHORT(2, 2), MPFROMP(&vt));
        printf("item 2,2: \"%s\" (%lu), 1,3: %06lx, attr 2,1 %lx\n", buf, n, (ULONG)WinSendMsg(vs, VM_QUERYITEM, MPFROM2SHORT(1, 3), 0),
               (ULONG)WinSendMsg(vs, VM_QUERYITEMATTR, MPFROM2SHORT(2, 1), 0));
        cs = WinCreateWindow(hwnd, WC_CIRCULARSLIDER, "Volume", WS_VISIBLE, 60, 0, 120, 115, hwnd, HWND_TOP, 2, NULL, NULL);
        WinSendMsg(cs, CSM_SETRANGE, MPFROMSHORT(0), MPFROMSHORT(100));
        WinSendMsg(cs, CSM_SETINCREMENT, MPFROMSHORT(5), MPFROMSHORT(10));
        WinSendMsg(cs, CSM_SETVALUE, MPFROMSHORT(50), 0);
        return 0;
    }
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == 1) {
            MRESULT sel = WinSendMsg(vs, VM_QUERYSELECTEDITEM, 0, 0);
            printf("%s %d,%d (selected %d,%d)\n", SHORT2FROMMP(mp1) == VN_SELECT ? "VN_SELECT" : SHORT2FROMMP(mp1) == VN_ENTER ? "VN_ENTER" : "VN_?",
                   SHORT1FROMMP(mp2), SHORT2FROMMP(mp2), SHORT1FROMMR(sel), SHORT2FROMMR(sel));
        } else if (SHORT1FROMMP(mp1) == 2 && SHORT2FROMMP(mp1) == CSN_CHANGED) printf("CSN_CHANGED %d\n", (SHORT)SHORT1FROMMP(mp2));
        else if (SHORT1FROMMP(mp1) == 2 && SHORT2FROMMP(mp1) == CSN_TRACKING) printf("CSN_TRACKING %d\n", (SHORT)SHORT1FROMMP(mp2));
        return 0;
    case WM_PAINT: { RECTL r; HPS ps = WinBeginPaint(hwnd, 0, &r); WinFillRect(ps, &r, CLR_PALEGRAY); WinEndPaint(ps); return 0; }
    case WM_CLOSE: {
        SHORT v = 0;
        WinSendMsg(cs, CSM_QUERYVALUE, MPFROMP(&v), 0);
        MRESULT sel = WinSendMsg(vs, VM_QUERYSELECTEDITEM, 0, 0);
        printf("done: dial %d, selected %d,%d\n", v, SHORT1FROMMR(sel), SHORT2FROMMR(sel));
        WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "vs", ClientProc, CS_SIZEREDRAW, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "vs", "Value set", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 240, 210, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
