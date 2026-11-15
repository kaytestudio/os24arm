/* userland/os2x86/test/ow/pmgeo.c - GPI's geometry: a full arc (filled),
 * an area (a triangle, its boundary), a path filled with a pattern, XOR
 * mix, polygons (a pentagram, alternate: its middle left empty), a dotted
 * line, a partial arc, a point arc, a spline, a fillet, markers, a box
 * with rounded corners, a clip rectangle. run-tests.sh checks the pixels. */
#define INCL_WIN
#define INCL_GPI
#include <os2.h>
#include <stdio.h>
#ifndef MARKSYM_SOLIDSQUARE
#define MARKSYM_SOLIDSQUARE 8   /* (the toolkit's; not in OpenWatcom's headers) */
#endif

static void at(HPS ps, LONG x, LONG y) { POINTL p; p.x = x; p.y = y; GpiMove(ps, &p); }
static void to(HPS ps, LONG x, LONG y) { POINTL p; p.x = x; p.y = y; GpiLine(ps, &p); }

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    if (msg == WM_PAINT) {
        RECTL r;
        HPS ps = WinBeginPaint(hwnd, 0, &r);
        POINTL p, pts[6];
        WinQueryWindowRect(hwnd, &r);
        WinFillRect(ps, &r, CLR_WHITE);
        /* a filled circle: centre (60, 60), radius 40 */
        GpiSetColor(ps, CLR_RED);
        at(ps, 60, 60);
        GpiFullArc(ps, DRO_FILL, MAKEFIXED(40, 0));
        /* an area: a triangle, blue, its boundary too */
        GpiSetColor(ps, CLR_BLUE);
        GpiBeginArea(ps, BA_BOUNDARY | BA_ALTERNATE);
        at(ps, 130, 20); to(ps, 190, 20); to(ps, 160, 100);
        GpiEndArea(ps);
        /* a path, a square, filled with the halftone pattern */
        GpiSetColor(ps, CLR_GREEN);
        GpiSetPattern(ps, PATSYM_HALFTONE);
        GpiBeginPath(ps, 1);
        at(ps, 220, 20);
        pts[0].x = 280; pts[0].y = 20; pts[1].x = 280; pts[1].y = 80; pts[2].x = 220; pts[2].y = 80;
        GpiPolyLine(ps, 3, pts);
        GpiCloseFigure(ps);
        GpiEndPath(ps);
        GpiFillPath(ps, 1, FPATH_ALTERNATE);
        GpiSetPattern(ps, PATSYM_DEFAULT);
        /* XOR: red over white is cyan */
        GpiSetMix(ps, FM_XOR);
        GpiSetColor(ps, CLR_RED);
        at(ps, 300, 20); p.x = 340; p.y = 60; GpiBox(ps, DRO_FILL, &p, 0, 0);
        GpiSetMix(ps, FM_OVERPAINT);
        /* a pentagram, alternate: the middle stays white */
        {
            POLYGON pg;
            static POINTL star[4] = { { 395, 20 }, { 330, 140 }, { 300, 80 }, { 405, 140 } };
            at(ps, 300, 140);                                   /* (the first point: the current position - 5 points) */
            star[0].x = 395; star[0].y = 80;  star[1].x = 315; star[1].y = 30;
            star[2].x = 345; star[2].y = 120; star[3].x = 370; star[3].y = 30;
            at(ps, 290, 80);
            pg.ulPoints = 4; pg.aPointl = star;
            GpiSetColor(ps, CLR_DARKGREEN);
            GpiPolygons(ps, 1, &pg, POLYGON_NOBOUNDARY | POLYGON_ALTERNATE, POLYGON_INCL);
        }
        /* a dotted line */
        GpiSetColor(ps, CLR_BLACK);
        GpiSetLineType(ps, LINETYPE_DOT);
        at(ps, 20, 150); to(ps, 120, 150);
        GpiSetLineType(ps, LINETYPE_DEFAULT);
        /* a partial arc: centre (60, 200), radius 30, a quarter from 0 degrees */
        GpiSetColor(ps, CLR_PINK);
        at(ps, 90, 200);
        p.x = 60; p.y = 200;
        GpiPartialArc(ps, &p, MAKEFIXED(30, 0), MAKEFIXED(0, 0), MAKEFIXED(90, 0));
        /* a point arc, a spline, a fillet: only their ends checked */
        GpiSetColor(ps, CLR_BROWN);
        at(ps, 130, 200); pts[0].x = 160; pts[0].y = 230; pts[1].x = 190; pts[1].y = 200;
        GpiPointArc(ps, pts);
        at(ps, 200, 160); pts[0].x = 220; pts[0].y = 220; pts[1].x = 250; pts[1].y = 140; pts[2].x = 270; pts[2].y = 200;
        GpiPolySpline(ps, 3, pts);
        GpiQueryCurrentPosition(ps, &p);
        printf("spline ends at %ld %ld\n", p.x, p.y);
        at(ps, 280, 160); pts[0].x = 300; pts[0].y = 220; pts[1].x = 320; pts[1].y = 160;
        GpiPolyFillet(ps, 2, pts);
        /* a marker, a solid square */
        GpiSetColor(ps, CLR_DARKBLUE);
        GpiSetMarker(ps, MARKSYM_SOLIDSQUARE);
        p.x = 350; p.y = 200; GpiMarker(ps, &p);
        /* a box with rounded corners: its corner left white */
        GpiSetColor(ps, CLR_DARKCYAN);
        at(ps, 20, 240); p.x = 120; p.y = 280; GpiBox(ps, DRO_FILL, &p, 30, 30);
        /* a clip rectangle: a box drawn half outside it */
        {
            RECTL c = { 150, 240, 200, 280 };
            GpiIntersectClipRectangle(ps, &c);
            GpiSetColor(ps, CLR_YELLOW);
            at(ps, 130, 240); p.x = 230; p.y = 279; GpiBox(ps, DRO_FILL | DRO_OUTLINE, &p, 0, 0);
        }
        WinEndPaint(ps);
        return 0;
    }
    if (msg == WM_CLOSE) { WinPostMsg(hwnd, WM_QUIT, 0, 0); return 0; }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "geo", ClientProc, CS_SIZEREDRAW, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "geo", "Geometry", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 420, 300, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
