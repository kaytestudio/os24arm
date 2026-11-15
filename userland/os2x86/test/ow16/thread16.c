/* userland/os2x86/test/ow16/thread16.c - 16-bit threads (DosCreateThread
 * on stacks of the program's own; DosExit(EXIT_THREAD); a far return
 * ending one too), a RAM semaphore guarding a counter (DosSemRequest,
 * DosSemClear), a system semaphore (DosCreateSem, DosSemSet; the last
 * thread clears it; DosSemWait), DosMuxSemWait. It prints what it gets.
 * Built with -s: the C runtime's stack check knows only the first thread's. */
#define INCL_DOS
#include <os2.h>
#include <stdio.h>
#ifndef CSEM_PUBLIC
#define CSEM_PUBLIC 1    /* (the toolkit's; not in OpenWatcom's 16-bit headers) */
#endif

static ULONG lock;                       /* a RAM semaphore */
static HSYSSEM done;
static volatile int counter, finished;
static char stk1[4096], stk2[4096], stk3[2048];

static void far worker(void)
{
    int i;
    for (i = 0; i < 1000; i++) {
        int v;
        DosSemRequest((HSEM)&lock, SEM_INDEFINITE_WAIT);
        v = counter;
        if ((i & 63) == 0) DosSleep(1L);  /* (the other thread, while it's held) */
        counter = v + 1;
        DosSemClear((HSEM)&lock);
    }
    DosSemRequest((HSEM)&lock, SEM_INDEFINITE_WAIT);
    if (++finished == 2) DosSemClear((HSEM)done);
    DosSemClear((HSEM)&lock);
    DosExit(EXIT_THREAD, 0);
}
static void far quick(void) { counter += 0; }     /* (returns: the thread ends) */

int main(void)
{
    TID t1, t2, t3;
    USHORT which;
    struct { USHORT cnt; MUXSEM m[1]; } mux;
    if (DosCreateSem(CSEM_PUBLIC, &done, "\\SEM\\T16DONE")) { printf("DosCreateSem failed\n"); return 1; }
    DosSemSet((HSEM)done);
    if (DosCreateThread(worker, &t1, stk1 + sizeof stk1) || DosCreateThread(worker, &t2, stk2 + sizeof stk2) ||
        DosCreateThread(quick, &t3, stk3 + sizeof stk3)) { printf("DosCreateThread failed\n"); return 1; }
    printf("threads %u %u %u\n", t1, t2, t3);
    if (DosSemWait((HSEM)done, 20000L)) { printf("timed out, counter %d\n", counter); return 1; }
    mux.cnt = 1; mux.m[0].zero = 0; mux.m[0].hsem = (HSEM)done;
    printf("counter %d, finished %d, mux %u\n", counter, finished, DosMuxSemWait(&which, &mux, 0L) ? 99 : which);
    printf("lock %s\n", DosSemWait((HSEM)&lock, 0L) == 0 ? "free" : "held");
    DosCloseSem(done);
    return 0;
}
