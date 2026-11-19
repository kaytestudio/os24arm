/* userland/os2x86/test/ow/pmhelp.c - the help manager: a help table (a
 * HELPTABLE resource), F1 on a control (its panel), F1 on the client (not
 * in the table: HM_HELPSUBITEM_NOT_FOUND, then the extended help panel),
 * the program's own HM_ messages (contents, index, a panel by name, one
 * that isn't there), an inform link (HM_INFORM back). Its help, pmhelp.hlp,
 * from pmhelp.ipf (wipfc). It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#define INCL_WINHELP
#include <os2.h>
#include <stdio.h>

static HWND help;
static MRESULT EXPENTRY proc(HWND w, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        WinCreateWindow(w, WC_BUTTON, "Button", WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 100, 30, w, HWND_TOP, 10, 0, 0);
        return 0;
    case WM_PAINT: {
        RECTL r;
        HPS ps = WinBeginPaint(w, 0, &r);
        WinFillRect(ps, &r, CLR_WHITE);
        WinEndPaint(ps);
        return 0;
    }
    case WM_CHAR:
        if (!(SHORT1FROMMP(mp1) & KC_KEYUP) && (SHORT1FROMMP(mp1) & KC_CHAR)) {
            ULONG r;
            switch (SHORT1FROMMP(mp2)) {
            case 'c': r = (ULONG)WinSendMsg(help, HM_HELP_CONTENTS, 0, 0); printf("contents: %lu\n", r); break;
            case 'i': r = (ULONG)WinSendMsg(help, HM_HELP_INDEX, 0, 0); printf("index: %lu\n", r); break;
            case 'n': r = (ULONG)WinSendMsg(help, HM_DISPLAY_HELP, MPFROMP("second"), MPFROMSHORT(HM_PANELNAME)); printf("by name: %lu\n", r); break;
            case 'r': r = (ULONG)WinSendMsg(help, HM_DISPLAY_HELP, MPFROMSHORT(999), MPFROMSHORT(HM_RESOURCEID)); printf("panel 999: %lx\n", r); break;
            case 'q': WinPostMsg(w, WM_QUIT, 0, 0); break;
            }
            return (MRESULT)TRUE;
        }
        break;
    case HM_HELPSUBITEM_NOT_FOUND:
        printf("not found: window %u, item %u\n", SHORT1FROMMP(mp2), SHORT2FROMMP(mp2));
        return (MRESULT)FALSE;
    case HM_INFORM: printf("inform %u\n", SHORT1FROMMP(mp1)); return 0;
    case HM_ERROR: printf("error %lx\n", (ULONG)mp1); return 0;
    }
    return WinDefWindowProc(w, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ mq = WinCreateMsgQueue(hab, 0);
    ULONG fcf = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER;
    HWND frame, client;
    HELPINIT hi = { 0 };
    QMSG m;
    WinRegisterClass(hab, "pmhelp", proc, CS_SIZEREDRAW, 0);
    frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fcf, "pmhelp", "Help test", 0, 0, 1, &client);
    hi.cb = sizeof hi;
    hi.phtHelpTable = (PHELPTABLE)MAKELONG(1000, 0xFFFF);
    hi.pszHelpWindowTitle = "Help test";
    hi.pszHelpLibraryName = "pmhelp.hlp";
    help = WinCreateHelpInstance(hab, &hi);
    printf("instance %s, associated %d\n", help ? "made" : "not made", (int)WinAssociateHelpInstance(help, frame));
    fflush(stdout);
    while (WinGetMsg(hab, &m, 0, 0, 0)) WinDispatchMsg(hab, &m);
    WinDestroyHelpInstance(help);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(mq);
    WinTerminate(hab);
    return 0;
}
