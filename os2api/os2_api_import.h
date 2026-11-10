/* ============================================================================
 * os2api/os2_api_import.h  —  DOSCALLS.DLL import table
 *
 * Declares the subset of os2_api.h's Dos* API that dos/dos_commands.c,
 * dos/cmd_main.c, and kayte/kaytevm_main.c actually call, as function
 * *pointer* variables instead of plain function prototypes - each one
 * resolved at startup from an embedded copy of DOSCALLS.DLL (built by
 * add_lx_dll() in the top-level CMakeLists.txt) via os2_api_import_init()
 * below, rather than linked in directly as compiled-in object code the
 * way os2_api.c used to be.
 *
 * Because a pointer-to-function variable is called with exactly the same
 * `Name(args...)` syntax as a real function, callers need no source
 * changes beyond including this header instead of os2_api.h - see
 * dos/dos_commands.c, dos/cmd_main.c, kayte/kaytevm_main.c.
 *
 * A plain function and a function-pointer variable of the same name
 * can't both be declared (the compiler sees conflicting declarations),
 * so this deliberately does NOT include os2_api.h - only the shared
 * types/constants (os2_api_types.h) it needs, which os2_api.h also
 * includes.
 * ========================================================================== */

#ifndef OS2_API_IMPORT_H
#define OS2_API_IMPORT_H

#include "os2_api_types.h"

extern APIRET (*DosOpen)(const char *filename, HFILE *handle, uint32_t *action,
                          uint32_t size, uint32_t attributes, uint32_t open_flags,
                          uint32_t open_mode);
extern APIRET (*DosClose)(HFILE handle);
extern APIRET (*DosRead)(HFILE handle, void *buffer, uint32_t count, uint32_t *bytes_read);
extern APIRET (*DosWrite)(HFILE handle, const void *buffer, uint32_t count, uint32_t *bytes_written);

extern APIRET (*DosFindFirst)(const char *pattern, HDIR *handle, uint32_t attributes,
                               FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count);
extern APIRET (*DosFindNext)(HDIR handle, FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count);
extern APIRET (*DosFindClose)(HDIR handle);

extern APIRET (*DosQueryPathInfo)(const char *filename, uint32_t info_level, FILESTATUS *info, uint32_t buf_size);
extern APIRET (*DosDelete)(const char *filename);
extern APIRET (*DosMove)(const char *old_name, const char *new_name);
extern APIRET (*DosCreateDir)(const char *dirname);
extern APIRET (*DosDeleteDir)(const char *dirname);

extern APIRET (*DosSetCurrentDir)(const char *dirname);
extern APIRET (*DosQueryCurrentDir)(char *buffer, uint32_t buffer_len);

extern void (*DosResolvePath)(const char *in, char *out, uint32_t out_size);

/* Loads the embedded DOSCALLS.DLL (see os2api/doscalls_embed.S.in) and
 * resolves every pointer above against its exports. Callers (cmd_main.c,
 * kaytevm_main.c) must call this once, before using any Dos* call above -
 * returns 0 on success, negative on failure (a missing/unresolvable
 * export is a build-time mismatch between this file's list and
 * DOSCALLS.DLL's actual exports, not something to recover from at
 * runtime, so this prints an error and returns non-zero rather than
 * leaving a NULL pointer for a caller to crash into later). */
int os2_api_import_init(void);

#endif /* OS2_API_IMPORT_H */
