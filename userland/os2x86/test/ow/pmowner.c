/* userland/os2x86/test/ow/pmowner.c - owner drawing: a list box with
 * LS_OWNERDRAW (WM_MEASUREITEM for its items' height, WM_DRAWITEM for
 * each, the selection left to the list box), a value set item with
 * VIA_OWNERDRAW, menu items with MIS_OWNERDRAW (measured, drawn, lit). It
 * prints what it gets, for run-tests.sh, which also checks the pixels. */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>
#include <string.h>

static HWND lb, vs;
static const char *names[] = { "Red", "Green", "Blue", "Yellow" };
static const LONG cols[] = { CLR_RED, CLR_GREEN, CLR_BLUE, CLR_YELLOW };
static int measured, drawn_list, drawn_vs, drawn_menu, lit_menu;

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE: {
        lb = WinCreateWindow(hwnd, WC_LISTBOX, "", WS_VISIBLE | LS_OWNERDRAW, 10, 10, 120, 110, hwnd, HWND_TOP, 1, NULL, NULL);
        for (int i = 0; i < 4; i++) WinSendMsg(lb, LM_INSERTITEM, MPFROMSHORT(LIT_END), MPFROMP(names[i]));
        VSCDATA vd = { sizeof vd, 1, 2 };
        vs = WinCreateWindow(hwnd, WC_VALUESET, "", WS_VISIBLE | VS_RGB, 140, 60, 90, 40, hwnd, HWND_TOP, 2, &vd, NULL);
        WinSendMsg(vs, VM_SETITEM, MPFROM2SHORT(1, 1), MPFROMLONG(0x808080));
        WinSendMsg(vs, VM_SETITEMATTR, MPFROM2SHORT(1, 2), MPFROM2SHORT(VIA_OWNERDRAW, TRUE));
        return 0;
    }
    case WM_MEASUREITEM:
        measured++;
        if (SHORT1FROMMP(mp1) == 1) return MRFROM2SHORT(24, 0);           /* the list box's items: 24 high */
        if (SHORT1FROMMP(mp1) == 10) {                                    /* the menu's: 110 x 22 */
            POWNERITEM oi = (POWNERITEM)mp2;
            oi->rclItem.xRight = oi->rclItem.xLeft + 110; oi->rclItem.yTop = oi->rclItem.yBottom + 22;
            return MRFROMSHORT(22);
        }
        return 0;
    case WM_DRAWITEM: {
        POWNERITEM oi = (POWNERITEM)mp2;
        RECTL r = oi->rclItem;
        if (SHORT1FROMMP(mp1) == 1) {                                     /* a colour swatch and its name */
            drawn_list++;
            WinFillRect(oi->hps, &r, CLR_WHITE);
            RECTL sw;
            sw.xLeft = r.xLeft + 2; sw.yBottom = r.yBottom + 3; sw.xRight = r.xLeft + 20; sw.yTop = r.yTop - 3;
            WinFillRect(oi->hps, &sw, cols[oi->idItem]);
            POINTL p;
            p.x = r.xLeft + 26; p.y = r.yBottom + 6;
            GpiSetColor(oi->hps, CLR_BLACK);
            GpiCharStringAt(oi->hps, &p, (LONG)strlen(names[oi->idItem]), (PCH)names[oi->idItem]);
            return (MRESULT)TRUE;                                         /* (fsState left: the list box inverts the selected one) */
        }
        if (SHORT1FROMMP(mp1) == 2) { drawn_vs++; WinFillRect(oi->hps, &r, CLR_DARKBLUE); return (MRESULT)TRUE; }
        if (SHORT1FROMMP(mp1) == 10) {                                    /* the menu: our own highlight */
            drawn_menu++;
            BOOL lit = (oi->fsAttribute & MIA_HILITED) != 0;
            if (lit) lit_menu++;
            WinFillRect(oi->hps, &r, lit ? CLR_DARKGREEN : CLR_PALEGRAY);
            RECTL sw;
            sw.xLeft = r.xLeft + 4; sw.yBottom = r.yBottom + 4; sw.xRight = r.xLeft + 18; sw.yTop = r.yTop - 4;
            WinFillRect(oi->hps, &sw, oi->idItem == 11 ? CLR_RED : CLR_GREEN);
            oi->fsAttributeOld = oi->fsAttribute;                         /* (drawn: nothing left for the menu) */
            return (MRESULT)TRUE;
        }
        return 0;
    }
    case WM_COMMAND: printf("WM_COMMAND %d\n", SHORT1FROMMP(mp1)); return 0;
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == 1 && SHORT2FROMMP(mp1) == LN_SELECT) printf("LN_SELECT %d\n", (int)(SHORT)WinSendMsg(lb, LM_QUERYSELECTION, MPFROMSHORT(LIT_FIRST), 0));
        return 0;
    case WM_PAINT: { RECTL r; HPS ps = WinBeginPaint(hwnd, 0, &r); WinFillRect(ps, &r, CLR_PALEGRAY); WinEndPaint(ps); return 0; }
    case WM_CLOSE:
        printf("done: measured %d, list drawn %s, value set drawn %s, menu drawn %s, lit %s\n", measured,
               drawn_list >= 4 ? "yes" : "no", drawn_vs ? "yes" : "no", drawn_menu >= 2 ? "yes" : "no", lit_menu ? "yes" : "no");
        WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_MENU | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "owner", ClientProc, CS_SIZEREDRAW, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "owner", "Owner drawn", 0, 0, 1, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 240, 180, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
