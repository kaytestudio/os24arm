/* userland/os2x86/test/ow/pmedit.c - PM's multi-line edit control (typing,
 * the keys, import and export, line queries) and spin buttons (numeric
 * and from an array). It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#define ID_DLG 100
#define IDC_MLE 101
#define IDC_SPIN 102
#define IDC_DAYS 103

static char text[512], buf[512], day[16];
static LONG spin;
static int lines, changes;

static MRESULT EXPENTRY DlgProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    static const char *days[] = { "Mon", "Tue", "Wed", "Thu", "Fri" };
    switch (msg) {
    case WM_INITDLG:
        WinSetDlgItemText(hwnd, IDC_MLE, "Hello\nWorld");
        WinSendDlgItemMsg(hwnd, IDC_SPIN, SPBM_SETLIMITS, MPFROMLONG(10), MPFROMLONG(1));
        WinSendDlgItemMsg(hwnd, IDC_SPIN, SPBM_SETCURRENTVALUE, MPFROMLONG(3), 0);
        WinSendDlgItemMsg(hwnd, IDC_DAYS, SPBM_SETARRAY, MPFROMP(days), MPFROMSHORT(5));
        printf("lines at first: %ld\n", (LONG)WinSendDlgItemMsg(hwnd, IDC_MLE, MLM_QUERYLINECOUNT, 0, 0));
        return 0;
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == IDC_MLE && SHORT2FROMMP(mp1) == MLN_CHANGE) changes++;
        return 0;
    case WM_COMMAND: {
        HWND mle = WinWindowFromID(hwnd, IDC_MLE);
        WinQueryWindowText(mle, sizeof text, text);
        lines = (int)(LONG)WinSendMsg(mle, MLM_QUERYLINECOUNT, 0, 0);
        IPT ipt = 0;
        ULONG cb = sizeof buf - 1;
        WinSendMsg(mle, MLM_SETIMPORTEXPORT, MPFROMP(buf), MPFROMLONG(sizeof buf));
        ULONG n = (ULONG)WinSendMsg(mle, MLM_EXPORT, MPFROMP(&ipt), MPFROMP(&cb));
        buf[n] = 0;
        printf("exported %lu bytes, line 2 is %ld long\n", n,
               (LONG)WinSendMsg(mle, MLM_QUERYLINELENGTH, WinSendMsg(mle, MLM_CHARFROMLINE, MPFROMLONG(1), 0), 0));
        WinSendDlgItemMsg(hwnd, IDC_SPIN, SPBM_QUERYVALUE, MPFROMP(&spin), MPFROM2SHORT(0, SPBQ_DONOTUPDATE));
        WinSendDlgItemMsg(hwnd, IDC_DAYS, SPBM_QUERYVALUE, MPFROMP(day), MPFROM2SHORT(sizeof day, SPBQ_DONOTUPDATE));
        WinDismissDlg(hwnd, SHORT1FROMMP(mp1));
        return 0;
    }
    }
    return WinDefDlgProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG r = WinDlgBox(HWND_DESKTOP, HWND_DESKTOP, DlgProc, NULLHANDLE, ID_DLG, NULL);
    for (char *p = text; *p; p++) if (*p == '\r') *p = 'R'; else if (*p == '\n') *p = 'N';
    for (char *p = buf; *p; p++) if (*p == '\r') *p = 'R'; else if (*p == '\n') *p = 'N';
    printf("WinDlgBox %lu: text \"%s\", export \"%s\", %d lines, %s changes, spin %ld, day %s\n",
           r, text, buf, lines, changes ? "some" : "no", spin, day);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
