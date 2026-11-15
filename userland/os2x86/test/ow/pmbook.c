/* userland/os2x86/test/ow/pmbook.c - PM's notebook (pages from dialog
 * resources, tabs, turning pages, BKN_PAGESELECTED) and slider (ticks,
 * scale text, the arm by the mouse and the keys, SLN_CHANGE). It prints
 * what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include "pmbook.h"

static HWND book, page1, page2;
static ULONG id1, id2;
static int selects;

static MRESULT EXPENTRY PageProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    if (msg == WM_CONTROL && SHORT1FROMMP(mp1) == IDC_SLIDER && (SHORT2FROMMP(mp1) == SLN_CHANGE || SHORT2FROMMP(mp1) == SLN_SLIDERTRACK)) {
        LONG v = (LONG)WinSendDlgItemMsg(hwnd, IDC_SLIDER, SLM_QUERYSLIDERINFO, MPFROM2SHORT(SMA_SLIDERARMPOSITION, SMA_INCREMENTVALUE), 0);
        char b[16];
        sprintf(b, "%ld", v);
        WinSetDlgItemText(hwnd, IDC_VALUE, b);
        if (SHORT2FROMMP(mp1) == SLN_CHANGE) printf("SLN_CHANGE %ld\n", v);
        return 0;
    }
    if (msg == WM_COMMAND) return 0;                  /* (pages aren't dismissed) */
    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        book = WinCreateWindow(hwnd, WC_NOTEBOOK, "", WS_VISIBLE | BKS_TABBEDDIALOG | BKS_MAJORTABTOP, 0, 0, 100, 100,
                               hwnd, HWND_TOP, 1, NULL, NULL);
        id1 = (ULONG)WinSendMsg(book, BKM_INSERTPAGE, 0, MPFROM2SHORT(BKA_MAJOR | BKA_STATUSTEXTON | BKA_AUTOPAGESIZE, BKA_LAST));
        id2 = (ULONG)WinSendMsg(book, BKM_INSERTPAGE, 0, MPFROM2SHORT(BKA_MAJOR | BKA_STATUSTEXTON | BKA_AUTOPAGESIZE, BKA_LAST));
        WinSendMsg(book, BKM_SETTABTEXT, MPFROMLONG(id1), MPFROMP("~Sound"));
        WinSendMsg(book, BKM_SETTABTEXT, MPFROMLONG(id2), MPFROMP("~Owner"));
        WinSendMsg(book, BKM_SETSTATUSLINETEXT, MPFROMLONG(id1), MPFROMP("Page 1 of 2"));
        WinSendMsg(book, BKM_SETSTATUSLINETEXT, MPFROMLONG(id2), MPFROMP("Page 2 of 2"));
        page1 = WinLoadDlg(book, book, PageProc, NULLHANDLE, ID_PAGE1, NULL);
        page2 = WinLoadDlg(book, book, PageProc, NULLHANDLE, ID_PAGE2, NULL);
        WinSendMsg(book, BKM_SETPAGEWINDOWHWND, MPFROMLONG(id1), MPFROMHWND(page1));
        WinSendMsg(book, BKM_SETPAGEWINDOWHWND, MPFROMLONG(id2), MPFROMHWND(page2));
        for (int i = 0; i <= 10; i += 5) {
            char t[4];
            sprintf(t, "%d", i);
            WinSendDlgItemMsg(page1, IDC_SLIDER, SLM_SETTICKSIZE, MPFROM2SHORT(i, 6), 0);
            WinSendDlgItemMsg(page1, IDC_SLIDER, SLM_SETSCALETEXT, MPFROMSHORT(i), MPFROMP(t));
        }
        WinSendDlgItemMsg(page1, IDC_SLIDER, SLM_SETSLIDERINFO, MPFROM2SHORT(SMA_SLIDERARMPOSITION, SMA_INCREMENTVALUE), MPFROMLONG(3));
        printf("pages %ld, first %s\n", (LONG)WinSendMsg(book, BKM_QUERYPAGECOUNT, 0, MPFROMSHORT(BKA_END)),
               (ULONG)WinSendMsg(book, BKM_QUERYPAGEID, 0, MPFROM2SHORT(BKA_FIRST, 0)) == id1 ? "page 1" : "?");
        return 0;
    case WM_SIZE:
        WinSetWindowPos(book, HWND_TOP, 0, 0, SHORT1FROMMP(mp2), SHORT2FROMMP(mp2), SWP_SIZE | SWP_MOVE | SWP_SHOW);
        return 0;
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == 1 && SHORT2FROMMP(mp1) == BKN_PAGESELECTED) {
            PAGESELECTNOTIFY *n = (PAGESELECTNOTIFY *)mp2;
            selects++;
            printf("BKN_PAGESELECTED -> %s\n", n->ulPageIdNew == id2 ? "page 2" : n->ulPageIdNew == id1 ? "page 1" : "?");
        }
        return 0;
    case WM_CLOSE: {
        char name[32] = "";
        WinQueryDlgItemText(page2, IDC_NAME, sizeof name, name);
        LONG v = (LONG)WinSendDlgItemMsg(page1, IDC_SLIDER, SLM_QUERYSLIDERINFO, MPFROM2SHORT(SMA_SLIDERARMPOSITION, SMA_INCREMENTVALUE), 0);
        ULONG top = (ULONG)WinSendMsg(book, BKM_QUERYPAGEID, 0, MPFROM2SHORT(BKA_TOP, 0));
        printf("done: slider %ld, name \"%s\", on %s, %d selections\n", v, name, top == id2 ? "page 2" : "page 1", selects);
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
    WinRegisterClass(hab, "PmBook", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmBook", "PM Notebook", 0, NULLHANDLE, 0, &client);
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
