/* Function-pointer imports for the DOSCALLS API used by DOS commands and
 * Kayte. os2_api_import_init() resolves them from the embedded DLL.
 * Don't include os2_api.h here: it declares the same names as functions. */

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
extern APIRET (*DosQueryFSInfoPath)(const char *path, FSINFOPATH *info);
extern void (*DosDisplayPath)(const char *vfs_path, char *out, uint32_t out_size);
struct lx_services;
extern APIRET (*DosSetKernelServices)(const struct lx_services *svc);

/* Load the embedded DOSCALLS.DLL and resolve the pointers above. Call once
 * before using the API. A missing export means the import list and DLL got
 * out of sync, so initialization fails instead of leaving a null pointer. */
int os2_api_import_init(void);

#endif /* OS2_API_IMPORT_H */
