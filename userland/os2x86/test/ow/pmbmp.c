/* userland/os2x86/test/ow/pmbmp.c - PM's bitmaps: from resources (24-bit,
 * 4-bit with a palette, an OS/2 1.x 8-bit), WinDrawBitmap plain and
 * stretched, a memory DC and PS with a bitmap of its own (GpiCreateBitmap,
 * GpiSetBitmap) drawn into, GpiBitBlt to the window, GpiQueryBitmapBits.
 * run-tests.sh checks the pixels of its window. */
#define INCL_WIN
#define INCL_GPI
#define INCL_DEV
#include <os2.h>
#include <stdio.h>
#include <string.h>

static HBITMAP b1, b2, b3, bmem;
static HDC hdcMem;
static HPS hpsMem;

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    if (msg == WM_PAINT) {
        RECTL r;
        HPS hps = WinBeginPaint(hwnd, NULLHANDLE, &r);
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(hps, &r, CLR_WHITE);
        POINTL p;
        p.x = 10; p.y = 10;
        WinDrawBitmap(hps, b1, NULL, &p, 0, 0, DBM_NORMAL);
        RECTL t;
        t.xLeft = 100; t.yBottom = 10; t.xRight = 164; t.yTop = 74;
        WinDrawBitmap(hps, b2, NULL, (PPOINTL)&t, 0, 0, DBM_STRETCH);
        p.x = 200; p.y = 10;
        WinDrawBitmap(hps, b3, NULL, &p, 0, 0, DBM_NORMAL);
        POINTL a[3];
        a[0].x = 250; a[0].y = 10; a[1].x = 330; a[1].y = 50; a[2].x = 0; a[2].y = 0;
        GpiBitBlt(hps, hpsMem, 3, a, ROP_SRCCOPY, BBO_IGNORE);
        WinEndPaint(hps);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    HPS hps = WinGetScreenPS(HWND_DESKTOP);
    b1 = GpiLoadBitmap(hps, NULLHANDLE, 1, 0, 0);
    b2 = GpiLoadBitmap(hps, NULLHANDLE, 2, 0, 0);
    b3 = GpiLoadBitmap(hps, NULLHANDLE, 3, 0, 0);
    BITMAPINFOHEADER bi;
    GpiQueryBitmapParameters(b1, &bi);
    printf("b1 %dx%d", bi.cx, bi.cy);
    GpiQueryBitmapParameters(b2, &bi);
    printf(", b2 %dx%d", bi.cx, bi.cy);
    GpiQueryBitmapParameters(b3, &bi);
    printf(", b3 %dx%d\n", bi.cx, bi.cy);

    /* a memory PS, a bitmap of its own, drawn into */
    DEVOPENSTRUC dop = { 0 };
    SIZEL sz = { 0, 0 };
    hdcMem = DevOpenDC(hab, OD_MEMORY, "*", 5, (PDEVOPENDATA)&dop, NULLHANDLE);
    hpsMem = GpiCreatePS(hab, hdcMem, &sz, PU_PELS | GPIT_MICRO | GPIA_ASSOC);
    BITMAPINFOHEADER2 h2;
    memset(&h2, 0, sizeof h2);
    h2.cbFix = sizeof h2; h2.cx = 80; h2.cy = 40; h2.cPlanes = 1; h2.cBitCount = 24;
    bmem = GpiCreateBitmap(hpsMem, &h2, 0, NULL, NULL);
    GpiSetBitmap(hpsMem, bmem);
    RECTL r;
    r.xLeft = 0; r.yBottom = 0; r.xRight = 80; r.yTop = 40;
    WinFillRect(hpsMem, &r, CLR_YELLOW);
    POINTL p;
    p.x = 0; p.y = 0;
    GpiMove(hpsMem, &p);
    p.x = 79; p.y = 39;
    GpiSetColor(hpsMem, CLR_BLACK);
    GpiBox(hpsMem, DRO_OUTLINE, &p, 0, 0);
    struct { BITMAPINFOHEADER2 h; } q;
    memset(&q, 0, sizeof q);
    q.h.cbFix = sizeof q.h; q.h.cPlanes = 1; q.h.cBitCount = 24;
    static BYTE bits[240 * 40];
    LONG n = GpiQueryBitmapBits(hpsMem, 0, 40, bits, (PBITMAPINFO2)&q);
    printf("GpiQueryBitmapBits %ld rows: (5,5) %02x%02x%02x, (0,0) %02x%02x%02x\n", n,
           bits[5 * 240 + 17], bits[5 * 240 + 16], bits[5 * 240 + 15], bits[2], bits[1], bits[0]);

    WinRegisterClass(hab, "PmBmp", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmBmp", "PM Bitmaps", 0, NULLHANDLE, 0, &client);
    QMSG qm;
    while (WinGetMsg(hab, &qm, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &qm);
    GpiSetBitmap(hpsMem, NULLHANDLE);
    GpiDeleteBitmap(bmem);
    GpiDestroyPS(hpsMem);
    DevCloseDC(hdcMem);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    printf("done\n");
    return 0;
}
