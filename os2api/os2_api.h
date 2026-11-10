/* ============================================================================
 * os2api/os2_api.h  —  OS/2 DOS-call API
 *
 * A DOS-call-shaped bridge onto the kernel's real VFS layer (vfs_open,
 * vfs_read, vfs_opendir, ...). This is the layer dos/ command
 * implementations go through to touch the filesystem, rather than calling
 * vfs_* directly - matching how real OS/2 programs call DOSCALLS.DLL
 * instead of talking to the filesystem driver themselves.
 * ========================================================================== */

#ifndef OS2_API_H
#define OS2_API_H

#include "os2_api_types.h"

/* ── File APIs ───────────────────────────────────────────────────────── */

APIRET DosOpen(const char *filename, HFILE *handle, uint32_t *action,
               uint32_t size, uint32_t attributes, uint32_t open_flags,
               uint32_t open_mode);
APIRET DosClose(HFILE handle);
APIRET DosRead(HFILE handle, void *buffer, uint32_t count, uint32_t *bytes_read);
APIRET DosWrite(HFILE handle, const void *buffer, uint32_t count, uint32_t *bytes_written);
APIRET DosChgFilePtr(HFILE handle, int32_t offset, uint32_t method, uint32_t *newpos);

APIRET DosFindFirst(const char *pattern, HDIR *handle, uint32_t attributes,
                     FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count);
APIRET DosFindNext(HDIR handle, FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count);
APIRET DosFindClose(HDIR handle);

APIRET DosQueryPathInfo(const char *filename, uint32_t info_level, FILESTATUS *info, uint32_t buf_size);
APIRET DosDelete(const char *filename);
APIRET DosMove(const char *old_name, const char *new_name);
APIRET DosCreateDir(const char *dirname);
APIRET DosDeleteDir(const char *dirname);

/* ── Current directory (DOS-style session state; the VFS itself only
 * understands absolute paths) ──────────────────────────────────────── */

APIRET DosSetCurrentDir(const char *dirname);
APIRET DosQueryCurrentDir(char *buffer, uint32_t buffer_len);

/**
 * Resolve a possibly-relative, possibly-backslash path against the
 * current directory into an absolute, forward-slash VFS path.
 */
void DosResolvePath(const char *in, char *out, uint32_t out_size);

/* ── Helper Functions ────────────────────────────────────────────────── */

/** Convert a VFS_ERR_* code to an OS/2 ERROR_* code */
APIRET vfs_to_os2_error(int vfs_error);

/** Convert OS/2 (FILESTATUS-style) attributes to VFS_ATTR_* */
uint32_t os2_to_vfs_attrs(uint16_t os2_attrs);

/** Convert VFS_ATTR_* to OS/2 (FILESTATUS-style) attributes */
uint16_t vfs_to_os2_attrs(uint32_t vfs_attrs);

/** This will convert a 16:32 SS:ESP value to a 16:16 SS:SP value. **/
VOID APIENTRY KernThunkStackTo16(VOID);

/** This will convert a 16:16 SS:SP value back to a 16:32 SS:ESP value. **/
VOID APIENTRY KernThunkStackTo32(VOID);

/** This will convert a 16:32 SS:ESP value to a native 64-bit AArch64 SP
 *  value. There is no such conversion in real OS/2 (32-bit only); this
 *  exists purely so 32-bit-shaped callers on this ARM64 port have a way
 *  to land back on the native flat 64-bit stack. **/
VOID APIENTRY KernThunkStackTo64(VOID);

/** Acquire the global serialization lock real OS/2 required before entering
 *  non-reentrant 16-bit device driver code. **/
VOID APIENTRY KernSerialize16BitDD(VOID);

/** Release the lock acquired by KernSerialize16BitDD(). **/
VOID APIENTRY KernUnSerialize16BitDD(VOID);

/** Copy a buffer of user space data into a kernel buffer. **/

APIRET APIENTRY KernCopyIn(PVOID trgt, PVOID src, ULONG size);
APIRET APIENTRY KernCopyOut(PVOID trgt, PVOID src, ULONG size);

/* ── KernVMAlloc() flags ─────────────────────────────────────────────── */

#define KEE_VMA_16MB        0x0001  /* allocate memory from below 16MB */
#define KEE_VMA_FIXED       0x0002  /* allocate memory that won't be swapped out */
#define KEE_VMA_SWAP        0x0004  /* allocate memory that is swappable */
#define KEE_VMA_CONTIG      0x0008  /* allocate contiguous physical memory pages */
#define KEE_VMA_PHYS        0x0010  /* allocate physical memory (pass address in physaddr argument) */
#define KEE_VMA_PROCESS     0x0020  /* allocate memory in the process address space */
#define KEE_VMA_SGSCONT     0x0040  /* register memory under screen group control */
#define KEE_VMA_GETSEL      0x0080  /* return a GDT selector for the allocated memory (also undocumented in DevHlp_VMAlloc) */
#define KEE_VMA_RESERVE     0x0100  /* don't commit the allocated memory */
#define KEE_VMA_SHARED      0x0200  /* allocate memory that is shareable */
#define KEE_VMA_USEHIGHMEM  0x0400  /* allocate memory from above 16MB */

/** Allocate virtual memory. On success, *linaddr receives the allocated
 *  pointer; *physaddr (if non-NULL) receives its physical address and
 *  *sel (if non-NULL) a GDT selector for it - see KEE_VMA_* above. **/
APIRET APIENTRY KernVMAlloc(ULONG size, ULONG flags, PVOID *linaddr, PULONG physaddr, PULONG sel);


/** Frees dynamic memory allocated with KernVMAlloc. **/
APIRET APIENTRY KernVMFree(PVOID linaddr);

/* ── KernVMLock() flags ──────────────────────────────────────────────── */

#define KEE_VML_NOTBLOCK    0x0001  /* Don't block the call, if the pages can't be locked. */
#define KEE_VML_CONTIG      0x0002  /* Pages locked must be contiguous. */
#define KEE_VML_16M         0x0004  /* Pages must be below 16MB. */
#define KEE_VML_WRITEABLE   0x0008  /* Pages will be writeable. */
#define KEE_VML_LONGLOCK    0x0010  /* The lock is for long time. */
#define KEE_VML_VERIFYONLY  0x0020  /* Verify the accessibility of the region. If KEE_VML_WRITEABLE is set, check for R/W access, else R/O. */

/* This port has no MMU-backed paging (see the comment above KernVMAlloc
 * in os2_api.c) - there's no real page table to describe a lock/pagelist
 * with, so these exist only to give KernVMLock() a well-formed shape to
 * accept/reject against. */
typedef struct {
    ULONG reserved;
} KEEVMLOCK;

typedef struct {
    ULONG page_count;
} KEEVMPageList;

/** Lock a region of memory (linaddr, size) against being paged out, per
 *  flags (KEE_VML_* above); *lock receives the lock handle/data, *pglist
 *  (if non-NULL) the region's page list, and *pgsize (if non-NULL) the
 *  size of that page list. **/
APIRET APIENTRY KernVMLock(ULONG flags, PVOID linaddr, ULONG size,
                            KEEVMLOCK *lock, KEEVMPageList *pglist, PULONG pgsize);

/** Unlock a memory region locked with KernVMLock (releases *lock). **/
APIRET APIENTRY KernVMUnlock(KEEVMLOCK *lock);

/** Build the list of physical page frames backing a linear address range
 *  (linaddr, size); *list receives the page list and *pgsize (if
 *  non-NULL) its size, same shape as KernVMLock's pglist/pgsize. **/
APIRET APIENTRY KernLinToPageList(PVOID linaddr, ULONG size, KEEVMPageList *list, PULONG pgsize);

/* ── KernVMSetMem() flags ────────────────────────────────────────────── */

#define KEE_VMS_UNCOMMIT    0x0001  /* Uncommit the region */
#define KEE_VMS_RESIDENT    0x0002  /* make the region resident */
#define KEE_VMS_SWAPPABLE   0x0004  /* make the region swappable */

/** Change the commit/residency/swap attributes of a region of memory
 *  (linaddr, size), per flags (KEE_VMS_* above). **/
APIRET APIENTRY KernVMSetMem(ULONG flags, PVOID linaddr, ULONG size);

/** Convert a 16:16 selector:offset far pointer to a flat 32-bit linear
 *  address (tiled mapping: selector index * 64K + offset). **/
ULONG APIENTRY KernSelToFlat(ULONG addr16);

/** Create a dynamic API entry point: a call gate letting 16-bit code call
 *  the routine at addr with cnt parameter words; *sel receives its
 *  selector. dummy is reserved (0). This port has no segmentation, so it
 *  always fails with ERROR_NOT_SUPPORTED and sets *sel to 0. **/
APIRET APIENTRY KernDynamicAPI(PVOID addr, ULONG cnt, ULONG dummy, PUSHORT sel);

/** Add a RAS event to the RAS log.
 *    major - major number of the RAS event
 *    minor - minor number of the RAS event
 *    buf   - buffer to be added to the RAS log (may be NULL if size is 0)
 *    size  - size of the buffer, in bytes
 *  This port has no in-memory RAS log, so the event is printed to the
 *  UART console instead. **/
APIRET APIENTRY KernRASSysTrace(ULONG major, ULONG minor, PVOID buf, ULONG size);



#endif /* OS2_API_H */
