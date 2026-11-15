/* userland/os2x86/test/ow/pmfdlg.c - the standard file dialog: an Open on
 * a filter (fdtest\*.TXT: the files that match, the subdirectories), a
 * file chosen; then a Save As with a name typed; then the font dialog. It prints what it gets,
 * for run-tests.sh (which makes fdtest first). */
#define INCL_WIN
#define INCL_WINSTDFILE
#include <os2.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    HAB hab = WinInitialize(0);
    HMQ hmq = WinCreateMsgQueue(hab, 0);
    FILEDLG fd;
    memset(&fd, 0, sizeof fd);
    fd.cbSize = sizeof fd;
    fd.fl = FDS_OPEN_DIALOG | FDS_CENTER;
    fd.pszTitle = "Open a text";
    strcpy(fd.szFullFile, "fdtest\\*.TXT");
    HWND h = WinFileDlg(HWND_DESKTOP, HWND_DESKTOP, &fd);
    printf("open: %s, %s, \"%s\", %lu\n", h ? "shown" : "not shown", fd.lReturn == DID_OK ? "OK" : "cancelled", fd.szFullFile, fd.ulFQFCount);
    memset(&fd, 0, sizeof fd);
    fd.cbSize = sizeof fd;
    fd.fl = FDS_SAVEAS_DIALOG;
    strcpy(fd.szFullFile, "fdtest\\sub\\");
    h = WinFileDlg(HWND_DESKTOP, HWND_DESKTOP, &fd);
    printf("save: %s, \"%s\"\n", fd.lReturn == DID_OK ? "OK" : "cancelled", fd.szFullFile);
    {
        FONTDLG fn;
        char family[FACESIZE] = "Helvetica";
        memset(&fn, 0, sizeof fn);
        fn.cbSize = sizeof fn;
        fn.pszFamilyname = family; fn.usFamilyBufLen = sizeof family;
        fn.fxPointSize = MAKEFIXED(10, 0);
        fn.pszPreview = "Warp 4";
        fn.fl = FNTS_CENTER;
        WinFontDlg(HWND_DESKTOP, HWND_DESKTOP, &fn);
        printf("font: %s, %s %ld pt, weight %u, face \"%s\", selection %x, use %x\n", fn.lReturn == DID_OK ? "OK" : "cancelled",
               family, (long)(fn.fxPointSize >> 16), fn.usWeight, fn.fAttrs.szFacename, fn.fAttrs.fsSelection, fn.fAttrs.fsFontUse);
    }
    WinDestroyMsgQueue(hmq);
    WinTerminate(hab);
    return 0;
}
