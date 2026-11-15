/* userland/os2x86/test/ow/pmtree.c - PM's container in the tree view:
 * records under records (RECORDINSERT's pRecordParent), CM_QUERYRECORD
 * by CMA_FIRSTCHILD/NEXT/PARENT, CM_EXPANDTREE, the + and - buttons,
 * the keys, CN_EXPANDTREE/CN_COLLAPSETREE/CN_ENTER, editing a record's
 * name in place (CN_BEGINEDIT, CN_REALLOCPSZ, CN_ENDEDIT), removing a
 * record with its children. It prints what it gets, for run-tests.sh. */
#define INCL_WIN
#include <os2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static HWND cnr;

static PRECORDCORE add(PRECORDCORE parent, const char *name, BOOL folder)
{
    PRECORDCORE r = (PRECORDCORE)WinSendMsg(cnr, CM_ALLOCRECORD, MPFROMLONG(0), MPFROMLONG(1));
    r->pszIcon = r->pszName = r->pszText = (PSZ)name;
    r->hptrIcon = WinQuerySysPointer(HWND_DESKTOP, folder ? SPTR_FOLDER : SPTR_FILE, FALSE);
    RECORDINSERT ri;
    memset(&ri, 0, sizeof ri);
    ri.cb = sizeof ri; ri.pRecordOrder = (PRECORDCORE)CMA_END; ri.pRecordParent = parent;
    ri.fInvalidateRecord = TRUE; ri.zOrder = CMA_TOP; ri.cRecordsInsert = 1;
    WinSendMsg(cnr, CM_INSERTRECORD, MPFROMP(r), MPFROMP(&ri));
    return r;
}
static const char *nm(PRECORDCORE r) { return r ? (const char *)r->pszName : "(none)"; }

static MRESULT EXPENTRY ClientProc(HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2)
{
    switch (msg) {
    case WM_CREATE: {
        cnr = WinCreateWindow(hwnd, WC_CONTAINER, "", WS_VISIBLE | CCS_SINGLESEL, 0, 0, 100, 100, hwnd, HWND_TOP, 7, NULL, NULL);
        CNRINFO ci;
        memset(&ci, 0, sizeof ci);
        ci.cb = sizeof ci; ci.flWindowAttr = CV_TREE | CV_NAME | CA_TREELINE;
        WinSendMsg(cnr, CM_SETCNRINFO, MPFROMP(&ci), MPFROMLONG(CMA_FLWINDOWATTR));
        PRECORDCORE desk = add(NULL, "Desktop", TRUE);
        PRECORDCORE drives = add(desk, "Drives", TRUE);
        add(drives, "Drive C", FALSE);
        add(drives, "Drive D", FALSE);
        PRECORDCORE progs = add(desk, "Programs", TRUE);
        add(progs, "Editor", FALSE);
        PRECORDCORE games = add(progs, "Games", TRUE);
        add(games, "Chess", FALSE);
        add(NULL, "Printer", FALSE);
        PRECORDCORE fc = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, MPFROMP(desk), MPFROM2SHORT(CMA_FIRSTCHILD, CMA_ITEMORDER));
        PRECORDCORE nx = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, MPFROMP(fc), MPFROM2SHORT(CMA_NEXT, CMA_ITEMORDER));
        PRECORDCORE pa = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, MPFROMP(games), MPFROM2SHORT(CMA_PARENT, CMA_ITEMORDER));
        PRECORDCORE top2 = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, MPFROMP(desk), MPFROM2SHORT(CMA_NEXT, CMA_ITEMORDER));
        printf("first child %s, next %s, parent of Games %s, after Desktop %s\n", nm(fc), nm(nx), nm(pa), nm(top2));
        WinSendMsg(cnr, CM_EXPANDTREE, MPFROMP(desk), 0);
        return 0;
    }
    case WM_SIZE: WinSetWindowPos(cnr, 0, 0, 0, SHORT1FROMMP(mp2), SHORT2FROMMP(mp2), SWP_SIZE | SWP_MOVE); return 0;
    case WM_CONTROL:
        if (SHORT1FROMMP(mp1) == 7) {
            USHORT code = SHORT2FROMMP(mp1);
            if (code == CN_EXPANDTREE) printf("CN_EXPANDTREE %s\n", nm((PRECORDCORE)mp2));
            else if (code == CN_COLLAPSETREE) printf("CN_COLLAPSETREE %s\n", nm((PRECORDCORE)mp2));
            else if (code == CN_ENTER) printf("CN_ENTER %s\n", nm(((PNOTIFYRECORDENTER)mp2)->pRecord));
            else if (code == CN_BEGINEDIT) printf("EDIT begin %s\n", nm(((PCNREDITDATA)mp2)->pRecord));
            else if (code == CN_REALLOCPSZ) {                   /* room for the new text: the program's own memory */
                PCNREDITDATA ed = (PCNREDITDATA)mp2;
                PSZ p = (PSZ)malloc(ed->cbText);
                printf("EDIT realloc %lu\n", ed->cbText);
                if (!p) return (MRESULT)FALSE;
                *ed->ppszText = p;
                ((PRECORDCORE)ed->pRecord)->pszIcon = ((PRECORDCORE)ed->pRecord)->pszName = ((PRECORDCORE)ed->pRecord)->pszText = p;
                return (MRESULT)TRUE;
            } else if (code == CN_ENDEDIT) printf("EDIT end %s\n", nm(((PCNREDITDATA)mp2)->pRecord));
        }
        return 0;
    case WM_CLOSE: {
        PRECORDCORE progs = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORD, MPFROMP(
            WinSendMsg(cnr, CM_QUERYRECORD, 0, MPFROM2SHORT(CMA_FIRST, CMA_ITEMORDER))), MPFROM2SHORT(CMA_LASTCHILD, CMA_ITEMORDER));
        PRECORDCORE cur = (PRECORDCORE)WinSendMsg(cnr, CM_QUERYRECORDEMPHASIS, MPFROMLONG(CMA_FIRST), MPFROMSHORT(CRA_SELECTED));
        printf("selected %s; removing %s:", nm(cur), nm(progs));
        WinSendMsg(cnr, CM_REMOVERECORD, MPFROMP(&progs), MPFROM2SHORT(1, CMA_INVALIDATE));
        CNRINFO ci;
        WinSendMsg(cnr, CM_QUERYCNRINFO, MPFROMP(&ci), MPFROMLONG(sizeof ci));
        printf(" %lu left\n", ci.cRecords);
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
    ULONG fl = FCF_TITLEBAR | FCF_SYSMENU | FCF_SIZEBORDER | FCF_TASKLIST;
    HWND client;
    WinRegisterClass(hab, "tree", ClientProc, CS_SIZEREDRAW, 0);
    HWND frame = WinCreateStdWindow(HWND_DESKTOP, WS_VISIBLE, &fl, "tree", "Tree", 0, 0, 0, &client);
    WinSetWindowPos(frame, HWND_TOP, 50, 50, 240, 220, SWP_SIZE | SWP_MOVE | SWP_SHOW);
    QMSG q;
    while (WinGetMsg(hab, &q, 0, 0, 0)) WinDispatchMsg(hab, &q);
    WinDestroyWindow(frame);
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
