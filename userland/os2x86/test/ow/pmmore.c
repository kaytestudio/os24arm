/* userland/os2x86/test/ow/pmmore.c - PM's scroll bars (the frame's, a
 * dialog's), a combo box, accelerators (F5, Ctrl+D) and the menu by the
 * keyboard (Alt+F, mnemonics). It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include "pmmore.h"

static int top, hpos;
static char fruit[32] = "";
static HWND hwndVert;

static MRESULT EXPENTRY DlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_INITDLG: {
        static const char *items[] = { "Apple", "Banana", "Cherry", "Date", "Elderberry" };
        for (int i = 0; i < 5; i++)
            WinSendDlgItemMsg(hwnd, IDC_COMBO, LM_INSERTITEM, MPFROMSHORT(LIT_END), MPFROMP(items[i]));
        WinSendDlgItemMsg(hwnd, IDC_COMBO, LM_SELECTITEM, MPFROMSHORT(0), MPFROMSHORT(TRUE));
        WinSendDlgItemMsg(hwnd, IDC_HSB, SBM_SETSCROLLBAR, MPFROMSHORT(5), MPFROM2SHORT(0, 10));
        return 0;
    }
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == IDC_COMBO && SHORT2FROMMP(mp1) == CBN_LBSELECT) printf("CBN_LBSELECT\n");
        return 0;
    case WM_HSCROLL: {
        USHORT cmd = SHORT2FROMMP(mp2);
        if (cmd == SB_LINERIGHT) hpos++;
        else if (cmd == SB_LINELEFT) hpos--;
        else if (cmd == SB_PAGERIGHT) hpos += 3;
        else if (cmd == SB_PAGELEFT) hpos -= 3;
        else return 0;
        WinSendDlgItemMsg(hwnd, IDC_HSB, SBM_SETPOS, MPFROMSHORT(5 + hpos), 0);
        printf("WM_HSCROLL %d -> %d\n", cmd, (int)(SHORT)WinSendDlgItemMsg(hwnd, IDC_HSB, SBM_QUERYPOS, 0, 0));
        return 0;
    }
    case WM_COMMAND:
        if (SHORT1FROMMP(mp1) == DID_OK) {
            WinQueryDlgItemText(hwnd, IDC_COMBO, sizeof fruit, fruit);
            printf("combo \"%s\", item %d\n", fruit,
                   (int)(SHORT)WinSendDlgItemMsg(hwnd, IDC_COMBO, LM_QUERYSELECTION, MPFROMSHORT(LIT_FIRST), 0));
        }
        WinDismissDlg(hwnd, SHORT1FROMMP(mp1));
        return 0;
    }
    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_SIZE:
        WinSendMsg(hwndVert, SBM_SETSCROLLBAR, MPFROMSHORT(top), MPFROM2SHORT(0, 40));
        WinSendMsg(hwndVert, SBM_SETTHUMBSIZE, MPFROM2SHORT(10, 50), 0);
        return 0;
    case WM_PAINT: {
        RECTL r;
        HPS hps = WinBeginPaint(hwnd, NULLHANDLE, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(hps, &r, CLR_WHITE);
        for (int i = 0; i < 20; i++) {
            char b[32];
            POINTL p;
            p.x = 10; p.y = r.yTop - 18 * (i + 1);
            if (p.y < 0) break;
            sprintf(b, "Line %d", top + i + 1);
            GpiCharStringAt(hps, &p, strlen(b), b);
        }
        WinEndPaint(hps);
        return 0;
    }
    case WM_VSCROLL: {
        USHORT cmd = SHORT2FROMMP(mp2);
        int o = top;
        if (cmd == SB_LINEDOWN) top++;
        else if (cmd == SB_LINEUP) top--;
        else if (cmd == SB_PAGEDOWN) top += 10;
        else if (cmd == SB_PAGEUP) top -= 10;
        else if (cmd == SB_SLIDERPOSITION) top = SHORT1FROMMP(mp2);
        if (top < 0) top = 0;
        if (top > 40) top = 40;
        if (top != o) {
            WinSendMsg(hwndVert, SBM_SETPOS, MPFROMSHORT(top), 0);
            printf("top %d\n", top);
            WinInvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (SHORT1FROMMP(mp1)) {
        case IDM_REFRESH: printf("IDM_REFRESH (source %d)\n", SHORT1FROMMP(mp2)); return 0;
        case IDM_DIALOG: {
            ULONG r = WinDlgBox(HWND_DESKTOP, hwnd, DlgProc, NULLHANDLE, ID_DLG, NULL);
            printf("WinDlgBox %lu\n", r);
            return 0;
        }
        case IDM_EXIT: printf("IDM_EXIT\n"); WinPostMsg(hwnd, WM_QUIT, 0, 0); return 0;
        }
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    WinRegisterClass(hab, "PmMore", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_MENU | FCF_ACCELTABLE | FCF_VERTSCROLL | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, 0, &fl, "PmMore", "PM More", 0, NULLHANDLE, ID_MAIN, &client);
    hwndVert = WinWindowFromID(frame, FID_VERTSCROLL);
    printf("scroll bar %s\n", hwndVert ? "yes" : "no");
    WinShowWindow(frame, TRUE);
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    printf("done: top %d, fruit \"%s\", hpos %d\n", top, fruit, hpos);
    return 0;
}
