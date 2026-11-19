#include "os2.h"

/* Legacy OS/2 API placeholders. The current DOSCALLS implementation is in
 * os2api/os2_api.c. */

APIRET DosOpen(PSZ pszFileName, HFILE *phFile, ULONG *pulAction,
               ULONG cbFile, ULONG ulAttribute, ULONG fsOpenFlags,
               ULONG fsOpenMode, PVOID peaop2)
{
    (void)pszFileName; (void)cbFile; (void)ulAttribute;
    (void)fsOpenFlags; (void)fsOpenMode; (void)peaop2;
    if (phFile)    *phFile    = 0;
    if (pulAction) *pulAction = 1;
    return NO_ERROR;
}

APIRET DosRead(HFILE hFile, PVOID pBuffer, ULONG cbRead, ULONG *pcbActual)
{
    (void)hFile; (void)pBuffer; (void)cbRead;
    if (pcbActual) *pcbActual = 0;
    return NO_ERROR;
}

APIRET DosWrite(HFILE hFile, PVOID pBuffer, ULONG cbWrite, ULONG *pcbActual)
{
    (void)hFile; (void)pBuffer; (void)cbWrite;
    if (pcbActual) *pcbActual = 0;
    return NO_ERROR;
}

APIRET DosClose(HFILE hFile)       { (void)hFile; return NO_ERROR; }
APIRET DosDelete(PSZ pszFile)      { (void)pszFile; return NO_ERROR; }
APIRET DosMove(PSZ pszOld, PSZ pszNew) { (void)pszOld; (void)pszNew; return NO_ERROR; }

APIRET DosQueryFileInfo(HFILE hFile, ULONG ulInfoLevel, PVOID pInfo, ULONG cbInfo)
{
    (void)hFile; (void)ulInfoLevel; (void)pInfo; (void)cbInfo;
    return NO_ERROR;
}

APIRET DosAllocMem(PVOID *ppb, ULONG cb, ULONG flags)
{
    (void)cb; (void)flags;
    if (ppb) *ppb = NULL;
    return NO_ERROR;
}

APIRET DosFreeMem(PVOID pb)                          { (void)pb; return NO_ERROR; }
APIRET DosSetMem(PVOID pb, ULONG cb, ULONG flags)    { (void)pb; (void)cb; (void)flags; return NO_ERROR; }

APIRET DosQueryMem(PVOID pb, PULONG pcb, PULONG pFlags)
{
    (void)pb;
    if (pcb)    *pcb    = 0;
    if (pFlags) *pFlags = 0;
    return NO_ERROR;
}

APIRET DosExecPgm(PCHAR pObjName, LONG cbObjName, ULONG execFlag,
                  PSZ pArg, PSZ pEnv, PVOID pResult, PSZ pName)
{
    (void)pObjName; (void)cbObjName; (void)execFlag;
    (void)pArg; (void)pEnv; (void)pResult; (void)pName;
    return NO_ERROR;
}

APIRET DosExit(ULONG action, ULONG result)
{
    (void)action; (void)result;
    for (;;) __asm__ volatile("wfe");
    return NO_ERROR;
}

APIRET DosWaitChild(ULONG action, ULONG waitOption,
                    PVOID pResult, PULONG pPID, PID pid)
{
    (void)action; (void)waitOption; (void)pResult; (void)pPID; (void)pid;
    return NO_ERROR;
}

APIRET DosCreateThread(TID *ptid, PVOID pfnThread,
                       ULONG param, ULONG flags, ULONG stackSize)
{
    (void)pfnThread; (void)param; (void)flags; (void)stackSize;
    if (ptid) *ptid = 0;
    return NO_ERROR;
}

APIRET DosExitThread(ULONG result)
{
    (void)result;
    for (;;) __asm__ volatile("wfe");
    return NO_ERROR;
}

APIRET DosWaitThread(TID *ptid, ULONG option) { (void)ptid; (void)option; return NO_ERROR; }

APIRET DosSleep(ULONG ulMilliseconds)
{
    volatile ULONG ticks = ulMilliseconds * 100000UL;
    while (ticks--) __asm__ volatile("nop");
    return NO_ERROR;
}

APIRET DosCreateEventSem(PSZ pszName, HEV *phev, ULONG flAttr, BOOL fState)
{
    (void)pszName; (void)flAttr; (void)fState;
    if (phev) *phev = 0;
    return NO_ERROR;
}

APIRET DosPostEventSem(HEV hev)                      { (void)hev; return NO_ERROR; }
APIRET DosWaitEventSem(HEV hev, ULONG timeout)       { (void)hev; (void)timeout; return NO_ERROR; }
APIRET DosCloseEventSem(HEV hev)                     { (void)hev; return NO_ERROR; }

APIRET DosCreateMutexSem(PSZ pszName, HMTX *phmtx, ULONG flAttr, BOOL fInitial)
{
    (void)pszName; (void)flAttr; (void)fInitial;
    if (phmtx) *phmtx = 0;
    return NO_ERROR;
}

APIRET DosRequestMutexSem(HMTX hmtx, ULONG timeout) { (void)hmtx; (void)timeout; return NO_ERROR; }
APIRET DosReleaseMutexSem(HMTX hmtx)                { (void)hmtx; return NO_ERROR; }
APIRET DosCloseMutexSem(HMTX hmtx)                  { (void)hmtx; return NO_ERROR; }

APIRET DosLoadModule(PSZ pszFailName, ULONG cbFailName,
                     PSZ pszModuleName, HMODULE *phmod)
{
    (void)pszFailName; (void)cbFailName; (void)pszModuleName;
    if (phmod) *phmod = 0;
    return NO_ERROR;
}

APIRET DosFreeModule(HMODULE hmod)                   { (void)hmod; return NO_ERROR; }

APIRET DosQueryProcAddr(HMODULE hmod, ULONG ordinal,
                        PSZ pszProcName, PVOID *ppfn)
{
    (void)hmod; (void)ordinal; (void)pszProcName;
    if (ppfn) *ppfn = NULL;
    return NO_ERROR;
}

APIRET DosQuerySysInfo(ULONG iStart, ULONG iLast, PVOID pBuf, ULONG cbBuf)
{
    (void)iStart; (void)iLast; (void)pBuf; (void)cbBuf;
    return NO_ERROR;
}

APIRET DosGetDateTime(PVOID pdt) { (void)pdt; return NO_ERROR; }
APIRET DosSetDateTime(PVOID pdt) { (void)pdt; return NO_ERROR; }
