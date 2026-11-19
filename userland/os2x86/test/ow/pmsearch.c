/* userland/os2x86/test/ow/pmsearch.c - a list box's LM_SEARCHSTRING: the
 * whole item (case or not, LSS_CASESENSITIVE), the item's start
 * (LSS_PREFIX), anywhere in it (LSS_SUBSTRING), after a given item, and no
 * match (LIT_NONE). It prints the items found, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>

static const char *const items[] = { "Apple", "apricot", "Banana", "pineapple", "APPLE" };

static SHORT find(HWND lb, USHORT fl, SHORT after, const char *s)
{
    return SHORT1FROMMR(WinSendMsg(lb, LM_SEARCHSTRING, MPFROM2SHORT(fl, after), MPFROMP(s)));
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    if (msg == WM_CREATE) {
        HWND lb = WinCreateWindow(hwnd, WC_LISTBOX, "", WS_VISIBLE, 10, 10, 150, 100, hwnd, HWND_TOP, 1, NULL, NULL);
        for (int i = 0; i < 5; i++) WinSendMsg(lb, LM_INSERTITEM, MPFROMSHORT(LIT_END), MPFROMP(items[i]));
        printf("%d %d %d %d %d %d %d %d\n",
               find(lb, 0, LIT_FIRST, "apple"),                                   /* the whole item: Apple */
               find(lb, LSS_CASESENSITIVE, LIT_FIRST, "APPLE"),                   /* as typed: APPLE */
               find(lb, LSS_PREFIX, LIT_FIRST, "ap"),                             /* Apple */
               find(lb, LSS_PREFIX | LSS_CASESENSITIVE, LIT_FIRST, "ap"),         /* apricot */
               find(lb, LSS_SUBSTRING, LIT_FIRST, "apple"),                       /* Apple */
               find(lb, LSS_SUBSTRING, 0, "apple"),                               /* after Apple: pineapple */
               find(lb, LSS_SUBSTRING | LSS_CASESENSITIVE, LIT_FIRST, "APP"),     /* APPLE */
               find(lb, 0, LIT_FIRST, "app"));                                    /* none: LIT_NONE */
        WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU;
    HWND client;
    WinRegisterClass(hab, "search", ClientProc, 0, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "search", "Search", 0, 0, 0, &client);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
