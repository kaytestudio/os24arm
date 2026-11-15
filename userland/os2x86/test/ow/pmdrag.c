/* userland/os2x86/test/ow/pmdrag.c - direct manipulation: two containers,
 * a record dragged (button 2) from one to the other - CN_INITDRAG, a
 * DRAGINFO with a DRAGITEM (string handles for its type, rendering and
 * name), DrgDrag; the other's CN_DRAGOVER (DrgVerifyType: DOR_DROP, DO_MOVE),
 * CN_DROP (DrgQueryStrName), the record moved - and one dropped where
 * it's refused (the container it came from). It prints what it gets, for
 * run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>

static HWND left, right;
static const char *names[] = { "alpha.txt", "beta.txt", "gamma.txt" };
static int overs;

static HWND mkcnr(HWND parent, int x, ULONG id)
{
    HWND c = WinCreateWindow(parent, WC_CONTAINER, "", WS_VISIBLE | CCS_SINGLESEL, x, 0, 140, 150, parent, HWND_TOP, id, NULL, NULL);
    CNRINFO ci;
    memset(&ci, 0, sizeof ci);
    ci.cb = sizeof ci; ci.flWindowAttr = CV_NAME;
    WinSendMsg(c, CM_SETCNRINFO, MPFROMP(&ci), MPFROMLONG(CMA_FLWINDOWATTR));
    return c;
}
static void add(HWND c, const char *name)
{
    PRECORDCORE r = (PRECORDCORE)WinSendMsg(c, CM_ALLOCRECORD, 0, MPFROMLONG(1));
    RECORDINSERT ri;
    r->pszIcon = r->pszName = r->pszText = (PSZ)name;
    r->hptrIcon = WinQuerySysPointer(HWND_DESKTOP, SPTR_FILE, FALSE);
    memset(&ri, 0, sizeof ri);
    ri.cb = sizeof ri; ri.pRecordOrder = (PRECORDCORE)CMA_END; ri.zOrder = CMA_TOP; ri.cRecordsInsert = 1; ri.fInvalidateRecord = TRUE;
    WinSendMsg(c, CM_INSERTRECORD, MPFROMP(r), MPFROMP(&ri));
}
static void list(const char *what, HWND c)
{
    printf("%s:", what);
    for (PRECORDCORE r = (PRECORDCORE)WinSendMsg(c, CM_QUERYRECORD, 0, MPFROM2SHORT(CMA_FIRST, CMA_ITEMORDER)); r;
         r = (PRECORDCORE)WinSendMsg(c, CM_QUERYRECORD, MPFROMP(r), MPFROM2SHORT(CMA_NEXT, CMA_ITEMORDER)))
        printf(" %s", r->pszName);
    printf("\n");
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE:
        left = mkcnr(hwnd, 0, 1);
        right = mkcnr(hwnd, 160, 2);
        for (int i = 0; i < 3; i++) add(left, names[i]);
        return 0;
    case WM_CONTROL: {
        USHORT id = SHORT1FROMMP(mp1), code = SHORT2FROMMP(mp1);
        if (code == CN_INITDRAG) {                                  /* the record under the pointer: off it goes */
            PCNRDRAGINIT di = (PCNRDRAGINIT)mp2;
            if (!di->pRecord) return 0;
            printf("CN_INITDRAG %s from %d\n", di->pRecord->pszName, id);
            PDRAGINFO pdinfo = DrgAllocDraginfo(1);
            DRAGITEM item;
            memset(&item, 0, sizeof item);
            item.hwndItem = di->hwndCnr; item.ulItemID = (ULONG)di->pRecord;
            item.hstrType = DrgAddStrHandle(DRT_TEXT);
            item.hstrRMF = DrgAddStrHandle("<DRM_OS2FILE,DRF_TEXT>");
            item.hstrSourceName = DrgAddStrHandle(di->pRecord->pszName);
            item.fsSupportedOps = DO_MOVEABLE | DO_COPYABLE;
            DrgSetDragitem(pdinfo, &item, sizeof item, 0);
            DRAGIMAGE img;
            memset(&img, 0, sizeof img);
            img.cb = sizeof img; img.hImage = di->pRecord->hptrIcon; img.fl = DRG_ICON;
            HWND t = DrgDrag(di->hwndCnr, pdinfo, &img, 1, VK_ENDDRAG, NULL);
            printf("DrgDrag: %s\n", t == right ? "the right one" : t == left ? "the left one" : t ? "somewhere" : "nowhere");
            DrgFreeDraginfo(pdinfo);
            return 0;
        }
        if (code == CN_DRAGOVER) {
            PCNRDRAGINFO ci = (PCNRDRAGINFO)mp2;
            PDRAGITEM it;
            overs++;
            DrgAccessDraginfo(ci->pDragInfo);
            it = DrgQueryDragitemPtr(ci->pDragInfo, 0);
            if (ci->pDragInfo->hwndSource == WinWindowFromID(hwnd, id)) return MRFROM2SHORT(DOR_NODROP, 0);   /* (not onto itself) */
            if (!DrgVerifyType(it, DRT_TEXT) || !DrgVerifyRMF(it, "DRM_OS2FILE", NULL)) return MRFROM2SHORT(DOR_NEVERDROP, 0);
            return MRFROM2SHORT(DOR_DROP, DO_MOVE);
        }
        if (code == CN_DROP) {
            PCNRDRAGINFO ci = (PCNRDRAGINFO)mp2;
            DRAGITEM it;
            char name[64];
            DrgAccessDraginfo(ci->pDragInfo);
            DrgQueryDragitem(ci->pDragInfo, sizeof it, &it, 0);
            DrgQueryStrName(it.hstrSourceName, sizeof name, name);
            printf("CN_DROP %s on %d, %s, %d items\n", name, id, ci->pDragInfo->usOperation == DO_MOVE ? "move" : "other",
                   (int)DrgQueryDragitemCount(ci->pDragInfo));
            for (int i = 0; i < 3; i++) if (!strcmp(names[i], name)) add(WinWindowFromID(hwnd, id), names[i]);
            PRECORDCORE r = (PRECORDCORE)it.ulItemID;
            WinSendMsg(it.hwndItem, CM_REMOVERECORD, MPFROMP(&r), MPFROM2SHORT(1, CMA_INVALIDATE));
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        printf("drag-overs %s\n", overs > 1 ? "yes" : "no");
        list("left", left); list("right", right);
        WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "drag", ClientProc, CS_SIZEREDRAW, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "drag", "Drag", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 300, 150, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
