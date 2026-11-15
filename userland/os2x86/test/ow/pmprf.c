/* userland/os2x86/test/ow/pmprf.c - profiles (Prf: the user's profile and
 * a private .INI - strings, numbers, binary data, enumeration, deletion)
 * and the help manager's instance. "pmprf write" stores, "pmprf read" (a
 * second run) reads it back from the files. It prints what it gets, for
 * run-tests.sh. */
#define INCL_WIN
#define INCL_WINHELP
#include <os2.h>
#include <stdio.h>
#include <string.h>

static void names(HINI h, PSZ app)
{
    char buf[256], *p;
    ULONG n = PrfQueryProfileString(h, app, NULL, NULL, buf, sizeof buf);
    printf("%s:", app ? (char *)app : "apps");
    for (p = buf; p < buf + n && *p; p += strlen(p) + 1) printf(" %s", p);
    printf("\n");
}

int main(int argc, char **argv)
{
    HAB hab = WinInitialize(0);
    char buf[64];
    ULONG size = 0;
    LONG big = 0;
    if (argc > 1 && !strcmp(argv[1], "write")) {
        LONG v = 123456;
        HINI h;
        PrfWriteProfileString(HINI_USERPROFILE, "OS2X86 Test", "Name", "Warp");
        PrfWriteProfileString(HINI_USERPROFILE, "OS2X86 Test", "Count", "42");
        PrfWriteProfileString(HINI_USERPROFILE, "OS2X86 Test", "Gone", "soon");
        PrfWriteProfileData(HINI_USERPROFILE, "OS2X86 Test", "Big", &v, sizeof v);
        PrfWriteProfileString(HINI_USERPROFILE, "OS2X86 Test", "Gone", NULL);
        PrfWriteProfileString(HINI_USERPROFILE, "Other App", "x", "y");
        PrfWriteProfileString(HINI_USERPROFILE, "Other App", NULL, NULL);
        h = PrfOpenProfile(hab, "PRIVATE.INI");
        printf("private: %s\n", h ? "opened" : "not opened");
        PrfWriteProfileString(h, "Settings", "Colour", "blue");
        PrfCloseProfile(h);
        {
            HELPINIT hi;
            HWND help;
            memset(&hi, 0, sizeof hi);
            hi.cb = sizeof hi; hi.pszHelpWindowTitle = "Test help"; hi.pszHelpLibraryName = "TEST.HLP";
            help = WinCreateHelpInstance(hab, &hi);
            printf("help: %s, rc %lu, associated %d\n", help ? "instance" : "none", hi.ulReturnCode, (int)WinAssociateHelpInstance(help, HWND_DESKTOP));
            WinDestroyHelpInstance(help);
        }
        printf("written\n");
    } else {
        HINI h = PrfOpenProfile(hab, "PRIVATE.INI");
        PrfQueryProfileString(HINI_PROFILE, "OS2X86 Test", "Name", "none", buf, sizeof buf);
        printf("Name %s, Count %ld, Missing %ld\n", buf, PrfQueryProfileInt(HINI_USERPROFILE, "OS2X86 Test", "Count", -1),
               PrfQueryProfileInt(HINI_USERPROFILE, "OS2X86 Test", "Missing", -1));
        size = sizeof big;
        {   /* (Watcom passes printf's arguments right to left: the call first) */
            BOOL got = PrfQueryProfileData(HINI_USERPROFILE, "OS2X86 Test", "Big", &big, &size);
            printf("Big %s %ld\n", got ? "read" : "not read", big);
        }
        PrfQueryProfileSize(HINI_USERPROFILE, "OS2X86 Test", "Name", &size);
        printf("Name's size %lu\n", size);
        PrfQueryProfileString(HINI_USERPROFILE, "OS2X86 Test", "Gone", "deleted", buf, sizeof buf);
        printf("Gone: %s\n", buf);
        names(HINI_USERPROFILE, NULL);
        names(HINI_USERPROFILE, "OS2X86 Test");
        PrfQueryProfileString(h, "Settings", "Colour", "", buf, sizeof buf);
        printf("private Colour %s\n", buf);
        PrfCloseProfile(h);
    }
    WinTerminate(hab);
    return 0;
}
