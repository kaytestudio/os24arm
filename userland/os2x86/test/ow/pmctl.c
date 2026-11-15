/* userland/os2x86/test/ow/pmctl.c - PM's resources and controls: a menu
 * (FCF_MENU, WM_COMMAND), a dialog from a template (WinDlgBox) with an
 * entry field, a check box, radio buttons, a list box and push buttons,
 * a string table. It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include "pmctl.h"

static char name[64] = "";
static int checked, radio, sel = -1;

static MRESULT EXPENTRY DlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_INITDLG: {
        static const char *items[] = { "Apple", "Banana", "Cherry", "Date" };
        for (int i = 0; i < 4; i++)
            WinSendDlgItemMsg(hwnd, IDC_LIST, LM_INSERTITEM, MPFROMSHORT(LIT_END), MPFROMP(items[i]));
        WinSetDlgItemText(hwnd, IDC_NAME, "Warp");
        WinSendDlgItemMsg(hwnd, IDC_RADIO1, BM_SETCHECK, MPFROMSHORT(1), 0);
        printf("WM_INITDLG: %d items\n", (int)(LONG)WinSendDlgItemMsg(hwnd, IDC_LIST, LM_QUERYITEMCOUNT, 0, 0));
        return 0;
    }
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == IDC_LIST && SHORT2FROMMP(mp1) == LN_SELECT) printf("LN_SELECT\n");
        return 0;
    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK) {
            WinQueryDlgItemText(hwnd, IDC_NAME, sizeof name, name);
            checked = (int)(LONG)WinSendDlgItemMsg(hwnd, IDC_CHECK, BM_QUERYCHECK, 0, 0);
            radio = WinSendDlgItemMsg(hwnd, IDC_RADIO2, BM_QUERYCHECK, 0, 0) ? 2 : 1;
            sel = (int)(LONG)WinSendDlgItemMsg(hwnd, IDC_LIST, LM_QUERYSELECTION, MPFROMSHORT(LIT_FIRST), 0);
        }
        WinDismissDlg(hwnd, SHORT1FROMMP(mp1));
        return 0;
    }
    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_PAINT: {
        RECTL r;
        HPS hps = WinBeginPaint(hwnd, NULLHANDLE, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(hps, &r, CLR_WHITE);
        char b[128];
        sprintf(b, "name \"%s\", check %d, radio %d, item %d", name, checked, radio, sel);
        WinDrawText(hps, -1, b, &r, CLR_BLACK, CLR_WHITE, DT_CENTER | DT_VCENTER);
        WinEndPaint(hps);
        return 0;
    }
    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1)) {
        case IDM_DIALOG: {
            ULONG r = WinDlgBox(HWND_DESKTOP, hwnd, DlgProc, NULLHANDLE, ID_DLG, NULL);
            printf("WinDlgBox: %lu, name \"%s\", check %d, radio %d, item %d\n", r, name, checked, radio, sel);
            WinInvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case IDM_ABOUT: {
            char s[64];
            WinLoadString(WinQueryAnchorBlock(hwnd), NULLHANDLE, IDS_HELLO, sizeof s, s);
            WinMessageBox(HWND_DESKTOP, hwnd, s, "About", 0, MB_OK);
            return 0;
        }
        case IDM_EXIT:
            printf("IDM_EXIT\n");
            WinPostMsg(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    char s[64] = "";
    WinLoadString(hab, NULLHANDLE, IDS_HELLO, sizeof s, s);
    printf("string: %s\n", s);
    WinRegisterClass(hab, "PmCtl", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_MENU | FCF_SHELLPOSITION | FCF_TASKLIST;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmCtl", "PM Controls", 0, NULLHANDLE, ID_MAIN, &client);
    printf("menu %s\n", WinWindowFromID(frame, FID_MENU) ? "yes" : "no");
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    printf("done\n");
    return 0;
}
