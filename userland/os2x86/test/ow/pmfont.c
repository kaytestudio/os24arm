/* userland/os2x86/test/ow/pmfont.c - PM's fonts: GpiQueryFonts, logical
 * fonts (a bitmap Courier by lMatch, an outline Helvetica Bold sized by
 * GpiSetCharBox), their metrics and text boxes, presentation parameters
 * (WinSetPresParam on a static, a dialog template's PRESPARAMS). It prints
 * what it gets, for run-tests.sh. */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>
#include <string.h>

static HWND hstatic;

static LONG width(HPS hps, const char *s)
{
    POINTL p[TXTBOX_COUNT];
    GpiQueryTextBox(hps, strlen(s), (PCH)s, TXTBOX_COUNT, p);
    return p[TXTBOX_CONCAT].x;
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    if (msg == WM_PAINT) {
        RECTL r;
        HPS hps = WinBeginPaint(hwnd, NULLHANDLE, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(hps, &r, CLR_WHITE);

        LONG n = 0;
        LONG more = GpiQueryFonts(hps, QF_PUBLIC, "Courier", &n, sizeof(FONTMETRICS), NULL);
        FONTMETRICS fm[16];
        n = more < 16 ? more : 16;
        GpiQueryFonts(hps, QF_PUBLIC, "Courier", &n, sizeof(FONTMETRICS), fm);
        int pick = -1;
        for (int i = 0; i < n; i++) if (!(fm[i].fsDefn & FM_DEFN_OUTLINE) && fm[i].sNominalPointSize == 120) pick = i;
        printf("Courier: %ld fonts, 12 point bitmap at %d\n", more, pick);

        FATTRS fa;
        memset(&fa, 0, sizeof fa);
        fa.usRecordLength = sizeof fa;
        strcpy(fa.szFacename, "Courier");
        fa.lMatch = pick >= 0 ? fm[pick].lMatch : 0;
        fa.lMaxBaselineExt = pick >= 0 ? fm[pick].lMaxBaselineExt : 18;
        fa.lAveCharWidth = pick >= 0 ? fm[pick].lAveCharWidth : 9;
        LONG m1 = GpiCreateLogFont(hps, NULL, 1, &fa);
        memset(&fa, 0, sizeof fa);
        fa.usRecordLength = sizeof fa;
        strcpy(fa.szFacename, "Helvetica Bold");
        fa.fsFontUse = FATTR_FONTUSE_OUTLINE;
        LONG m2 = GpiCreateLogFont(hps, NULL, 2, &fa);
        printf("GpiCreateLogFont %ld %ld\n", m1, m2);

        FONTMETRICS f;
        POINTL p;
        GpiSetCharSet(hps, 1);
        GpiQueryFontMetrics(hps, sizeof f, &f);
        printf("mono: fixed %d, WWWW %ld = iiii %ld\n", f.fsType & FM_TYPE_FIXED, width(hps, "WWWW"), width(hps, "iiii"));
        p.x = 10; p.y = r.yTop - 30;
        GpiCharStringAt(hps, &p, 14, "Courier 12 pt.");

        GpiSetCharSet(hps, 2);
        SIZEF box;
        box.cx = MAKEFIXED(26, 0); box.cy = MAKEFIXED(40, 0);
        GpiSetCharBox(hps, &box);
        GpiQueryFontMetrics(hps, sizeof f, &f);
        printf("outline bold: baseline extent %ld, bold %d\n", f.lMaxBaselineExt, (f.fsSelection & 0x20) != 0);
        p.x = 10; p.y = r.yTop - 90;
        GpiCharStringAt(hps, &p, 8, "Big bold");

        GpiSetCharSet(hps, LCID_DEFAULT);
        printf("default: WWWW %ld > iiii %ld\n", width(hps, "WWWW"), width(hps, "iiii"));
        p.x = 10; p.y = r.yTop - 120;
        GpiCharStringAt(hps, &p, 7, "Default");
        GpiDeleteSetId(hps, LCID_ALL);
        WinEndPaint(hps);
        return 0;
    }
    if (msg == WM_CREATE) {
        hstatic = WinCreateWindow(hwnd, WC_STATIC, "Red, 14 point", WS_VISIBLE | SS_TEXT, 10, 20, 300, 30, hwnd, HWND_TOP, 9, NULL, NULL);
        WinSetPresParam(hstatic, PP_FONTNAMESIZE, 8, "14.Helv");
        RGB2 red = { 0, 0, 255, 0 };
        WinSetPresParam(hstatic, PP_FOREGROUNDCOLOR, sizeof red, &red);
        char b[32] = "";
        ULONG id;
        WinQueryPresParam(hstatic, PP_FONTNAMESIZE, 0, &id, sizeof b, b, 0);
        printf("static font \"%s\"\n", b);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    WinRegisterClass(hab, "PmFont", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmFont", "PM Fonts", 0, NULLHANDLE, 0, &client);
    HWND dlg = WinLoadDlg(HWND_DESKTOP, frame, WinDefDlgProc, NULLHANDLE, 100, NULL);
    char b[32] = "";
    ULONG id;
    WinQueryPresParam(WinWindowFromID(dlg, 101), PP_FONTNAMESIZE, 0, &id, sizeof b, b, 0);
    printf("dialog item font \"%s\"\n", b);
    WinDestroyWindow(dlg);
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
