/* userland/os2x86/test/ow/pmcnr.c - PM's container: records of its own
 * (RECORDCORE and data after it), the details view (string, number, date
 * and icon columns, titles), the icon view, selection (CN_EMPHASIS),
 * double-click (CN_ENTER), sorting with a compare function, removing. It
 * prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

typedef struct { RECORDCORE rc; PSZ name; ULONG size; CDATE date; } FILEREC;
static HWND cnr;
static const char *names[] = { "readme.txt", "config.sys", "autoexec.bat", "startup.cmd", "os2.ini" };
static const ULONG sizes[] = { 1200, 4100, 300, 64, 22000 };

static SHORT APIENTRY ByName(PRECORDCORE a, PRECORDCORE b, PVOID p)
{
    (void)p;
    return (SHORT)strcmp(((FILEREC *)a)->name, ((FILEREC *)b)->name);
}

static void list(const char *what)
{
    printf("%s:", what);
    for (PRECORDCORE r = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, 0, MPFROM2SHORT(CMA_FIRST, CMA_ITEMORDER)); r;
         r = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, MPFROMP(r), MPFROM2SHORT(CMA_NEXT, CMA_ITEMORDER)))
        printf(" %s", ((FILEREC *)r)->name);
    printf("\n");
}

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE: {
        cnr = WinCreateWindow(hwnd, WC_CONTAINER, "", WS_VISIBLE | CCS_SINGLESEL, 0, 0, 100, 100, hwnd, HWND_TOP, 7, NULL, NULL);
        /* the columns */
        PFIELDINFO fi = (PFIELDINFO)WinSendMsg(cnr, CM_ALLOCDETAILFIELDINFO, MPFROMLONG(4), 0), f = fi;
        f->flData = CFA_BITMAPORICON | CFA_HORZSEPARATOR | CFA_SEPARATOR | CFA_CENTER; f->flTitle = CFA_CENTER;
        f->pTitleData = "Icon"; f->offStruct = FIELDOFFSET(RECORDCORE, hptrIcon); f = f->pNextFieldInfo;
        f->flData = CFA_STRING | CFA_HORZSEPARATOR | CFA_SEPARATOR | CFA_LEFT; f->pTitleData = "Name";
        f->offStruct = FIELDOFFSET(FILEREC, name); f = f->pNextFieldInfo;
        f->flData = CFA_ULONG | CFA_HORZSEPARATOR | CFA_SEPARATOR | CFA_RIGHT; f->pTitleData = "Size";
        f->offStruct = FIELDOFFSET(FILEREC, size); f = f->pNextFieldInfo;
        f->flData = CFA_DATE | CFA_HORZSEPARATOR | CFA_LEFT; f->pTitleData = "Date";
        f->offStruct = FIELDOFFSET(FILEREC, date);
        FIELDINFOINSERT fii = { sizeof fii, (PFIELDINFO)CMA_END, TRUE, 4 };
        LONG nf = (LONG)WinSendMsg(cnr, CM_INSERTDETAILFIELDINFO, MPFROMP(fi), MPFROMP(&fii));
        /* the records */
        FILEREC *r = (FILEREC *)WinSendMsg(cnr, CM_ALLOCRECORD, MPFROMLONG(sizeof(FILEREC) - sizeof(RECORDCORE)), MPFROMLONG(5)), *first = r;
        HPOINTER file = WinQuerySysPointer(HWND_DESKTOP, SPTR_FILE, FALSE);
        for (int i = 0; i < 5; i++, r = (FILEREC *)r->rc.preccNextRecord) {
            r->name = (PSZ)names[i]; r->size = sizes[i];
            r->date.day = (UCHAR)(i + 1); r->date.month = 11; r->date.year = 2026;
            r->rc.pszIcon = r->rc.pszName = r->rc.pszText = (PSZ)names[i];
            r->rc.hptrIcon = file;
        }
        RECORDINSERT ri = { sizeof ri, (PRECORDCORE)CMA_END, NULL, TRUE, CMA_TOP, 5 };
        LONG nr = (LONG)WinSendMsg(cnr, CM_INSERTRECORD, MPFROMP(first), MPFROMP(&ri));
        CNRINFO ci;
        memset(&ci, 0, sizeof ci);
        ci.cb = sizeof ci; ci.flWindowAttr = CV_DETAIL | CA_DETAILSVIEWTITLES | CA_CONTAINERTITLE | CA_TITLESEPARATOR;
        ci.pszCnrTitle = "Files";
        WinSendMsg(cnr, CM_SETCNRINFO, MPFROMP(&ci), MPFROMLONG(CMA_FLWINDOWATTR | CMA_CNRTITLE));
        printf("fields %ld, records %ld\n", nf, nr);
        list("inserted");
        return 0;
    }
    case WM_SIZE:
        WinSetWindowPos(cnr, HWND_TOP, 0, 0, SHORT1FROMMP(mp2), SHORT2FROMMP(mp2), SWP_SIZE | SWP_MOVE | SWP_SHOW);
        return 0;
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == 7 && SHORT2FROMMP(mp1) == CN_EMPHASIS) {
            PNOTIFYRECORDEMPHASIS n = (PNOTIFYRECORDEMPHASIS)mp2;
            if ((n->fEmphasisMask & CRA_SELECTED) && (n->pRecord->flRecordAttr & CRA_SELECTED))
                printf("CN_EMPHASIS %s selected\n", ((FILEREC *)n->pRecord)->name);
        }
        if (SHORT1FROMMP(mp1) == 7 && SHORT2FROMMP(mp1) == CN_ENTER) {
            PNOTIFYRECORDENTER n = (PNOTIFYRECORDENTER)mp2;
            printf("CN_ENTER %s\n", n->pRecord ? ((FILEREC *)n->pRecord)->name : "(none)");
        }
        return 0;
    case WM_CHAR:
        if (SHORT1FROMMP(mp1) & KC_KEYUP) return (MRESULT)TRUE;
        switch (SHORT1FROMMP(mp2)) {
        case 's':
            WinSendMsg(cnr, CM_SORTRECORD, MPFROMP(ByName), 0);
            list("sorted");
            break;
        case 'i': {
            CNRINFO ci;
            memset(&ci, 0, sizeof ci);
            ci.cb = sizeof ci; ci.flWindowAttr = CV_ICON | CA_CONTAINERTITLE | CA_TITLESEPARATOR;
            WinSendMsg(cnr, CM_SETCNRINFO, MPFROMP(&ci), MPFROMLONG(CMA_FLWINDOWATTR));
            break;
        }
        case 'd': {
            PRECORDCORE sel = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORDEMPHASIS, MPFROMLONG(CMA_FIRST), MPFROMSHORT(CRA_SELECTED));
            if (sel) {
                LONG left = (LONG)WinSendMsg(cnr, CM_REMOVERECORD, MPFROMP(&sel), MPFROM2SHORT(1, CMA_FREE | CMA_INVALIDATE));
                printf("removed, %ld left\n", left);
                list("now");
            }
            break;
        }
        }
        return (MRESULT)TRUE;
    case WM_CLOSE: {
        CNRINFO ci;
        WinSendMsg(cnr, CM_QUERYCNRINFO, MPFROMP(&ci), MPFROMLONG(sizeof ci));
        printf("done: %lu records, view %s\n", ci.cRecords, (ci.flWindowAttr & CV_ICON) ? "icon" : "detail");
        WinPostMsg(hwnd, WM_QUIT, 0, 0);
        return 0;
    }
    }
    return WinDefWindowProc(hwnd, msg, mp1, mp2);
}

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    WinRegisterClass(hab, "PmCnr", ClientProc, CS_SIZEREDRAW, 0);
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_SHELLPOSITION;
    HWND client, frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "PmCnr", "PM Container", 0, NULLHANDLE, 0, &client);
    QMSG q;
    while (WinGetMsg(hab, &q, NULLHANDLE, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
