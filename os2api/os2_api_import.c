/* ============================================================================
 * os2api/os2_api_import.c  —  DOSCALLS.DLL import table implementation
 *
 * Defines the function-pointer variables declared in os2_api_import.h and
 * resolves them against an embedded copy of DOSCALLS.DLL - see that
 * header's comment, and os2api/doscalls_embed.S.in for how the DLL bytes
 * get into doscalls_dll_start/doscalls_dll_end below.
 * ========================================================================== */

#include "os2_api_import.h"
#include "lx_dll_loader.h"
#include "kprintf.h"

extern uint8_t doscalls_dll_start[];
extern uint8_t doscalls_dll_end[];

APIRET (*DosOpen)(const char *filename, HFILE *handle, uint32_t *action,
                   uint32_t size, uint32_t attributes, uint32_t open_flags,
                   uint32_t open_mode) = 0;
APIRET (*DosClose)(HFILE handle) = 0;
APIRET (*DosRead)(HFILE handle, void *buffer, uint32_t count, uint32_t *bytes_read) = 0;
APIRET (*DosWrite)(HFILE handle, const void *buffer, uint32_t count, uint32_t *bytes_written) = 0;

APIRET (*DosFindFirst)(const char *pattern, HDIR *handle, uint32_t attributes,
                        FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count) = 0;
APIRET (*DosFindNext)(HDIR handle, FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count) = 0;
APIRET (*DosFindClose)(HDIR handle) = 0;

APIRET (*DosQueryPathInfo)(const char *filename, uint32_t info_level, FILESTATUS *info, uint32_t buf_size) = 0;
APIRET (*DosDelete)(const char *filename) = 0;
APIRET (*DosMove)(const char *old_name, const char *new_name) = 0;
APIRET (*DosCreateDir)(const char *dirname) = 0;
APIRET (*DosDeleteDir)(const char *dirname) = 0;

APIRET (*DosSetCurrentDir)(const char *dirname) = 0;
APIRET (*DosQueryCurrentDir)(char *buffer, uint32_t buffer_len) = 0;

void (*DosResolvePath)(const char *in, char *out, uint32_t out_size) = 0;

static lx_dll_t g_doscalls;

/* Resolves `name` in g_doscalls and stores it into `fnptr` (cast to
 * fnptr's own type via __typeof__, so this works for every signature
 * above without a macro per function). Prints an error and returns 1 if
 * the export is missing - a mismatch between this file's import list and
 * DOSCALLS.DLL's actual exports is a build-time bug, not something to
 * paper over with a NULL pointer a caller could crash into. */
#define RESOLVE(fnptr, name) \
    do { \
        void *proc = lx_dll_get_proc(&g_doscalls, (name)); \
        if (!proc) { \
            kprintf("[DOSCALLS] Missing export: %s\n", (name)); \
            return 1; \
        } \
        (fnptr) = (__typeof__(fnptr))proc; \
    } while (0)

int os2_api_import_init(void)
{
    uint32_t size = (uint32_t)(doscalls_dll_end - doscalls_dll_start);
    int rc = lx_dll_load_image(doscalls_dll_start, size, &g_doscalls);
    if (rc != 0) {
        kprintf("[DOSCALLS] Failed to load embedded DOSCALLS.DLL (%d)\n", rc);
        return rc;
    }

    RESOLVE(DosOpen, "DosOpen");
    RESOLVE(DosClose, "DosClose");
    RESOLVE(DosRead, "DosRead");
    RESOLVE(DosWrite, "DosWrite");
    RESOLVE(DosFindFirst, "DosFindFirst");
    RESOLVE(DosFindNext, "DosFindNext");
    RESOLVE(DosFindClose, "DosFindClose");
    RESOLVE(DosQueryPathInfo, "DosQueryPathInfo");
    RESOLVE(DosDelete, "DosDelete");
    RESOLVE(DosMove, "DosMove");
    RESOLVE(DosCreateDir, "DosCreateDir");
    RESOLVE(DosDeleteDir, "DosDeleteDir");
    RESOLVE(DosSetCurrentDir, "DosSetCurrentDir");
    RESOLVE(DosQueryCurrentDir, "DosQueryCurrentDir");
    RESOLVE(DosResolvePath, "DosResolvePath");

    return 0;
}
