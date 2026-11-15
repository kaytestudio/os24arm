/* userland/os2x86/test/ow/pmxform.c - GPI's page units and transforms: a
 * presentation space in PU_LOENGLISH (hundredths of an inch: 0.96 pels at
 * 96 per inch) on its window's DC; a box in it; a model transform made by
 * GpiTranslate and GpiScale; a rotation by GpiRotate; GpiConvert world to
 * device; GpiQueryPS. run-tests.sh checks the pixels. */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>

static HPS ps;

static void box(LONG x0, LONG y0, LONG x1, LONG y1, LONG c)
{
    POINTL p;
    GpiSetColor(ps, c);
    p.x = x0; p.y = y0; GpiMove(ps, &p);
    p.x = x1; p.y = y1; GpiBox(ps, DRO_FILL, &p, 0, 0);
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE: {
        SIZEL s = { 0, 0 };
        HDC dc = WinOpenWindowDC(hwnd);
        ps = GpiCreatePS(WinQueryAnchorBlock(hwnd), dc, &s, PU_LOENGLISH | GPIA_ASSOC);
        printf("units %s\n", GpiQueryPS(ps, &s) == PU_LOENGLISH ? "LOENGLISH" : "?");
        return 0;
    }
    case WM_PAINT: {
        RECTL r;
        MATRIXLF m;
        POINTL p[2];
        FIXED sc[2];
        WinBeginPaint(hwnd, ps, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(ps, &r, CLR_WHITE);
        box(0, 0, 100, 100, CLR_RED);                    /* (an inch: 0..96 pels) */
        GpiTranslate(ps, &m, TRANSFORM_REPLACE, (p[0].x = 200, p[0].y = 0, &p[0]));
        sc[0] = MAKEFIXED(2, 0); sc[1] = MAKEFIXED(2, 0);
        GpiScale(ps, &m, TRANSFORM_ADD, sc, NULL);       /* (then twice the size: x 400.. in units) */
        GpiSetModelTransformMatrix(ps, 9, &m, TRANSFORM_REPLACE);
        box(0, 0, 25, 25, CLR_BLUE);                     /* (units 400..450: pels 384..432) */
        GpiRotate(ps, &m, TRANSFORM_REPLACE, MAKEFIXED(90, 0), NULL);
        GpiSetModelTransformMatrix(ps, 9, &m, TRANSFORM_REPLACE);
        box(0, -60, 50, -10, CLR_GREEN);                 /* (turned a quarter: x 10..60, y 0..50 units) */
        GpiSetModelTransformMatrix(ps, 0, NULL, TRANSFORM_REPLACE);
        p[0].x = 100; p[0].y = 50; p[1].x = 250; p[1].y = 0;
        GpiConvert(ps, CVTC_WORLD, CVTC_DEVICE, 2, p);
        printf("device: %ld %ld, %ld %ld\n", p[0].x, p[0].y, p[1].x, p[1].y);
        WinEndPaint(ps);
        return 0;
    }
    case WM_CLOSE: WinPostMsg(hwnd, WM_QUIT, 0, 0); return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "xform", ClientProc, 0, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "xform", "Transforms", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 460, 160, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
