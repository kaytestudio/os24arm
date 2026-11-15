/* userland/os2x86/test/ow/pmprint.c - printing: the printer's queue
 * (SplEnumQueue), its DC (DevOpenDC OD_QUEUED; DevQueryCaps,
 * DevQueryHardcopyCaps), a presentation space on it in PU_LOENGLISH, two
 * pages (DevEscape STARTDOC, NEWFRAME, ENDDOC): a box and text on each.
 * It prints what it gets; run-tests.sh checks the PDF it makes. */
#define INCL_WIN
#define INCL_GPI
#define INCL_DEV
#define INCL_SPL
#define INCL_SPLDOSPRINT
#include <os2.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG ret = 0, total = 0, need = 0;
    char buf[512];
    DEVOPENSTRUC dop;
    HDC dc;
    HPS ps;
    SIZEL sz = { 0, 0 };
    LONG caps[2];
    HCINFO hc;
    POINTL p;
    USHORT job = 0;
    LONG cb = 2;
    if (SplEnumQueue(NULL, 3, buf, sizeof buf, &ret, &total, &need, NULL) == 0 && ret)
        printf("queue: %s\n", ((PPRQINFO3)buf)->pszName);
    memset(&dop, 0, sizeof dop);
    dop.pszLogAddress = "PDF"; dop.pszDriverName = "PDF";
    dc = DevOpenDC(hab, OD_QUEUED, "*", 2, (PDEVOPENDATA)&dop, NULLHANDLE);
    DevQueryCaps(dc, CAPS_WIDTH, 2, caps);
    DevQueryHardcopyCaps(dc, 0, 1, &hc);
    printf("page: %ld x %ld pels, form %s %ld x %ld mm\n", caps[0], caps[1], hc.szFormname, hc.cx, hc.cy);
    ps = GpiCreatePS(hab, dc, &sz, PU_LOENGLISH | GPIA_ASSOC);
    DevEscape(dc, DEVESC_STARTDOC, 9, "test page", NULL, NULL);
    GpiSetColor(ps, CLR_BLUE);
    p.x = 100; p.y = 100; GpiMove(ps, &p);                  /* (an inch in from the bottom left: a 2-inch box) */
    p.x = 300; p.y = 300; GpiBox(ps, DRO_FILL, &p, 0, 0);
    GpiSetColor(ps, CLR_BLACK);
    p.x = 100; p.y = 1000; GpiCharStringAt(ps, &p, 6, "Page 1");
    DevEscape(dc, DEVESC_NEWFRAME, 0, NULL, NULL, NULL);
    GpiSetColor(ps, CLR_RED);
    p.x = 400; p.y = 100; GpiMove(ps, &p);
    p.x = 500; p.y = 200; GpiBox(ps, DRO_FILL, &p, 0, 0);
    DevEscape(dc, DEVESC_ENDDOC, 0, NULL, &cb, (PBYTE)&job);
    printf("job %u\n", job);
    GpiAssociate(ps, NULLHANDLE);
    GpiDestroyPS(ps);
    DevCloseDC(dc);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
