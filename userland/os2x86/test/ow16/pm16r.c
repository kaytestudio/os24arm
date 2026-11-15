/* userland/os2x86/test/ow16/pm16r.c - a 16-bit PM program's resources
 * (pm16r.rc, in its NE): a menu (FCF_MENU), a string table (WinLoadString),
 * a dialog (WinDlgBox, its 16-bit procedure; WM_INITDLG's create
 * parameters; an entry field read back; list box items sent with 16:16
 * pointers); DosGetResource. It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
#include <string.h>
#ifndef RT_MENU
#define RT_MENU 3     /* (the toolkit's) */
#endif

static HAB hab;
static char typed[64];

MRESULT EXPENTRY DlgProc(HWND hwnd, USHORT msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_INITDLG:
        printf("WM_INITDLG \"%s\"\n", (char far *)mp2);
        WinSetDlgItemText(hwnd, 10, "Warp");
        WinSendDlgItemMsg(hwnd, 11, LM_INSERTITEM, MPFROMSHORT(LIT_END), MPFROMP("first"));
        WinSendDlgItemMsg(hwnd, 11, LM_INSERTITEM, MPFROMSHORT(LIT_END), MPFROMP("second"));
        {
            char b[32];
            WinSendDlgItemMsg(hwnd, 11, LM_QUERYITEMTEXT, MPFROM2SHORT(1, sizeof b), MPFROMP(b));
            printf("list: %d items, the second \"%s\"\n", (int)(SHORT)WinSendDlgItemMsg(hwnd, 11, LM_QUERYITEMCOUNT, 0L, 0L), b);
        }
        return 0;
    case WM_COMMAND:
        WinQueryDlgItemText(hwnd, 10, sizeof typed, typed);
        WinDismissDlg(hwnd, SHORT1FROMMP(mp1));
        return 0;
    }
    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

MRESULT EXPENTRY ClientProc(HWND hwnd, USHORT msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == 101) {
            static char param[] = "create params";
            USHORT r = WinDlgBox(HWND_DESKTOP, hwnd, DlgProc, 0, 2, param);
            printf("WinDlgBox %u, typed \"%s\"\n", r, typed);
        }
        if (SHORT1FROMMP(mp1) == 102) WinPostMsg(hwnd, WM_QUIT, 0L, 0L);
        return 0;
    case WM_PAINT: { RECTL r; HPS ps = WinBeginPaint(hwnd, NULL, &r); WinFillRect(ps, &r, CLR_WHITE); WinEndPaint(ps); return 0; }
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HMQ hmq;
    HWND frame, client;
    QMSG q;
    char s[64];
    SEL sel;
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_MENU | FCF_TASKLIST;
    hab = WinInitialize(0);
    hmq = WinCreateMsgQueue(hab, 0);
    WinLoadString(hab, 0, 201, sizeof s, s);
    printf("string: \"%s\"\n", s);
    printf("DosGetResource: %s\n", DosGetResource(0, RT_MENU, 1, &sel) ? "failed" : "a selector");
    WinRegisterClass(hab, "pm16r", ClientProc, CS_SIZEREDRAW, 0);
    frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "pm16r", "16-bit resources", 0L, 0, 1, &client);
    printf("menu: %s\n", WinWindowFromID(frame, FID_MENU) ? "there" : "missing");
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 300, 160, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    printf("done\n");
    return 0;
}
