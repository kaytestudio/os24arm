/* userland/os2x86/test/ow/pmclip.c - PM's clipboard: CF_TEXT in giveable
 * shared memory (WinOpenClipbrd, WinEmptyClipbrd, WinSetClipbrdData,
 * WinQueryClipbrdData, WinQueryClipbrdFmtInfo, WinEnumClipbrdFmts), an
 * entry field's and an MLE's Ctrl+C/Ctrl+V through it; the pointer
 * (WinSetPointer, WinQueryPointer). It prints what it gets, for
 * run-tests.sh. */
#define INCL_WIN
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
#include <string.h>

ULONG APIENTRY WinEnumClipbrdFmts(HAB hab, ULONG fmt);   /* (not in OpenWatcom's headers) */
static HAB hab;
static HWND ef, mle;

static void put(const char *s)
{
    PVOID p;
    if (DosAllocSharedMem(&p, NULL, strlen(s) + 1, PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_GIVEABLE)) { printf("no memory\n"); return; }
    strcpy(p, s);
    WinOpenClipbrd(hab);
    WinEmptyClipbrd(hab);
    printf("set: %d\n", (int)WinSetClipbrdData(hab, (ULONG)p, CF_TEXT, CFI_POINTER));
    WinCloseClipbrd(hab);
}
static void show(const char *what)
{
    ULONG fi = 0;
    WinOpenClipbrd(hab);
    PSZ t = (PSZ)WinQueryClipbrdData(hab, CF_TEXT);
    BOOL have = WinQueryClipbrdFmtInfo(hab, CF_TEXT, &fi);
    printf("%s: \"%s\" (have %d, info %lx, first format %lu)\n", what, t ? (char *)t : "(none)", (int)have, fi, WinEnumClipbrdFmts(hab, 0));
    WinCloseClipbrd(hab);
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        ef = WinCreateWindow(hwnd, WC_ENTRYFIELD, "", WS_VISIBLE | ES_MARGIN, 10, 150, 200, 20, hwnd, HWND_TOP, 1, NULL, NULL);
        mle = WinCreateWindow(hwnd, WC_MLE, "", WS_VISIBLE | MLS_BORDER, 10, 10, 200, 120, hwnd, HWND_TOP, 2, NULL, NULL);
        put("Hello, clipboard");
        show("put");
        {
            HPOINTER w = WinQuerySysPointer(HWND_DESKTOP, SPTR_WAIT, FALSE);
            BOOL ok = WinSetPointer(HWND_DESKTOP, w);
            printf("pointer: set %d, same %d\n", (int)ok, WinQueryPointer(HWND_DESKTOP) == w);
        }
        return 0;
    case WM_CLOSE: {
        char a[100], b[200];
        WinQueryWindowText(ef, sizeof a, a);
        WinQueryWindowText(mle, sizeof b, b);
        for (char *p = b; *p; p++) if (*p == '\r' || *p == '\n') *p = '|';
        printf("entry \"%s\", mle \"%s\"\n", a, b);
        show("done");
        WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "clip", ClientProc, CS_SIZEREDRAW, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "clip", "Clipboard", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 240, 220, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
