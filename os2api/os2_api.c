/* ============================================================================
 * os2api/os2_api.c  —  OS/2 DOS-call API implementation
 *
 * Bridges DOS-call-shaped requests (DosOpen, DosRead, DosFindFirst, ...)
 * onto the kernel's real VFS layer. dos/ command implementations call
 * these, not vfs_* directly - matching how real OS/2 programs go through
 * DOSCALLS.DLL rather than talking to a filesystem driver themselves.
 * ========================================================================== */

#include "os2_api.h"
#include "vfs.h"
#include "string.h"
#include "kprintf.h"

/* This file is compiled two different ways:
 *   - Into DOSCALLS.DLL (add_lx_dll(DOSCALLS ...), top-level CMakeLists.txt)
 *     - a standalone freestanding image with nothing else to initialize
 *     its VFS/blkdev/ext4/btrfs state, so it must do so itself, lazily,
 *     the first time any call touches storage (real OS/2 programs never
 *     had to initialize DOSCALLS.DLL themselves either - it's already
 *     resident and mounted before any app runs).
 *   - Into the `os2api` static library (os2api/CMakeLists.txt), linked
 *     directly into the kernel image alongside rexx - there,
 *     kernel_main() (kernel/src/main.c) already calls vfs_init/blk_init/
 *     ext4_init/btrfs_init once, in this exact address space, before any
 *     shell/REXX code can run, so doing it again here would be redundant
 *     at best (double-mounting the same filesystem). That build defines
 *     OS2_API_SKIP_LAZY_INIT to skip the block below entirely.
 */
#ifndef OS2_API_SKIP_LAZY_INIT
#include "blkdev.h"
#include "fat32.h"
#include "ext4.h"
#include "btrfs.h"

extern void mem_init(uintptr_t start, size_t size);

/* DOSCALLS.DLL's own private heap, placed right after the DLL's own code
 * window (LX_DLL_LOAD_BASE, see kernel/include/lx_dll_loader.h) so it
 * can't overlap the DLL's loaded code, an EXE's code at LX_LOAD_BASE, or
 * an EXE's own private heap. */
#define OS2_API_HEAP_START 0x4A400000UL
#define OS2_API_HEAP_SIZE  (4u * 1024u * 1024u)

static int g_os2_api_initialized = 0;
#endif /* !OS2_API_SKIP_LAZY_INIT */

static void os2_api_ensure_init(void)
{
#ifdef OS2_API_SKIP_LAZY_INIT
    return;
#else
    if (g_os2_api_initialized) return;
    g_os2_api_initialized = 1;

    mem_init(OS2_API_HEAP_START, OS2_API_HEAP_SIZE);
    vfs_init();
    blk_init();
    fat32_init(NULL);
    ext4_init("/CACHE:1024");
    btrfs_init(NULL);
#endif /* !OS2_API_SKIP_LAZY_INIT */
}

/* ── Error / attribute translation ──────────────────────────────────────── */

APIRET vfs_to_os2_error(int vfs_error)
{
    switch (vfs_error) {
        case VFS_OK:           return NO_ERROR;
        case VFS_ERR_NOSPACE:  return ERROR_NOT_ENOUGH_MEMORY;
        case VFS_ERR_NOMOUNT:  return ERROR_INVALID_DRIVE;
        case VFS_ERR_BADFD:    return ERROR_INVALID_HANDLE;
        case VFS_ERR_INVAL:    return ERROR_INVALID_PARAMETER;
        case VFS_ERR_NOTFOUND: return ERROR_FILE_NOT_FOUND;
        case VFS_ERR_NOTDIR:   return ERROR_PATH_NOT_FOUND;
        case VFS_ERR_ISDIR:    return ERROR_ACCESS_DENIED;
        case VFS_ERR_ROFS:     return ERROR_WRITE_PROTECT;
        case VFS_ERR_IO:       return ERROR_NOT_SUPPORTED;
        default:                return ERROR_NOT_SUPPORTED;
    }
}

uint32_t os2_to_vfs_attrs(uint16_t os2_attrs)
{
    return (uint32_t)os2_attrs; /* bit values already match VFS_ATTR_* */
}

uint16_t vfs_to_os2_attrs(uint32_t vfs_attrs)
{
    return (uint16_t)vfs_attrs;
}

/* ── Current directory ───────────────────────────────────────────────────── */

static char current_dir[VFS_PATH_MAX] = "/";

void DosResolvePath(const char *in, char *out, uint32_t out_size)
{
    char tmp[VFS_PATH_MAX];
    uint32_t n = 0;

    /* DOS-style backslashes are accepted anywhere and normalized to '/'. */
    const char *src = in;
    if (*src == '/' || *src == '\\') {
        /* Already absolute. */
        while (*src && n < sizeof(tmp) - 1) {
            tmp[n++] = (*src == '\\') ? '/' : *src;
            src++;
        }
        tmp[n] = '\0';
    } else {
        /* Relative to current_dir. */
        uint32_t cdlen = (uint32_t)strlen(current_dir);
        for (uint32_t i = 0; i < cdlen && n < sizeof(tmp) - 1; i++) tmp[n++] = current_dir[i];
        if (n == 0 || tmp[n - 1] != '/') {
            if (n < sizeof(tmp) - 1) tmp[n++] = '/';
        }
        while (*src && n < sizeof(tmp) - 1) {
            tmp[n++] = (*src == '\\') ? '/' : *src;
            src++;
        }
        tmp[n] = '\0';
    }

    uint32_t i = 0;
    while (tmp[i] && i < out_size - 1) { out[i] = tmp[i]; i++; }
    out[i] = '\0';
}

APIRET DosSetCurrentDir(const char *dirname)
{
    os2_api_ensure_init();

    char resolved[VFS_PATH_MAX];
    DosResolvePath(dirname, resolved, sizeof(resolved));

    vfs_stat_t st;
    int r = vfs_stat(resolved, &st);
    if (r != VFS_OK) return vfs_to_os2_error(r);
    if (!(st.attrs & VFS_ATTR_DIR)) return ERROR_PATH_NOT_FOUND;

    strncpy(current_dir, resolved, sizeof(current_dir) - 1);
    current_dir[sizeof(current_dir) - 1] = '\0';
    return NO_ERROR;
}

APIRET DosQueryCurrentDir(char *buffer, uint32_t buffer_len)
{
    if (!buffer) return ERROR_INVALID_PARAMETER;
    uint32_t n = (uint32_t)strlen(current_dir);
    if (n >= buffer_len) return ERROR_BUFFER_OVERFLOW;
    strncpy(buffer, current_dir, buffer_len);
    return NO_ERROR;
}

/* ── File APIs ───────────────────────────────────────────────────────────── */

APIRET DosOpen(const char *filename, HFILE *handle, uint32_t *action,
               uint32_t size, uint32_t attributes, uint32_t open_flags,
               uint32_t open_mode)
{
    (void)size; (void)attributes; (void)open_flags;
    os2_api_ensure_init();

    char resolved[VFS_PATH_MAX];
    DosResolvePath(filename, resolved, sizeof(resolved));

    int fd = vfs_open(resolved, (int)open_mode);
    if (fd < 0) return vfs_to_os2_error(fd);

    if (handle) *handle = (HFILE)fd;
    if (action) *action = 1; /* opened existing (we don't distinguish create-vs-open here) */
    return NO_ERROR;
}

APIRET DosClose(HFILE handle)
{
    int r = vfs_close((int)handle);
    return vfs_to_os2_error(r);
}

APIRET DosRead(HFILE handle, void *buffer, uint32_t count, uint32_t *bytes_read)
{
    int r = vfs_read((int)handle, buffer, count);
    if (r < 0) {
        if (bytes_read) *bytes_read = 0;
        return vfs_to_os2_error(r);
    }
    if (bytes_read) *bytes_read = (uint32_t)r;
    return NO_ERROR;
}

APIRET DosWrite(HFILE handle, const void *buffer, uint32_t count, uint32_t *bytes_written)
{
    int r = vfs_write((int)handle, buffer, count);
    if (r < 0) {
        if (bytes_written) *bytes_written = 0;
        return vfs_to_os2_error(r);
    }
    if (bytes_written) *bytes_written = (uint32_t)r;
    return NO_ERROR;
}

APIRET DosChgFilePtr(HFILE handle, int32_t offset, uint32_t method, uint32_t *newpos)
{
    int r = vfs_seek((int)handle, offset, (int)method);
    if (r < 0) {
        if (newpos) *newpos = 0;
        return vfs_to_os2_error(r);
    }
    if (newpos) *newpos = (uint32_t)r;
    return NO_ERROR;
}

/* ── Directory search ───────────────────────────────────────────────────── */

/* Minimal DOS-style wildcard match: '*' = any run of chars, '?' = one char. */
static bool wildcard_match(const char *pattern, const char *name)
{
    while (*pattern && *name) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;
            while (*name) {
                if (wildcard_match(pattern, name)) return true;
                name++;
            }
            return false;
        }
        if (*pattern == '?' || *pattern == *name) {
            pattern++;
            name++;
            continue;
        }
        return false;
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0' && *name == '\0';
}

/* Split "/dir/PATTERN" into dir ("/dir") and the trailing name pattern. */
static void split_pattern(const char *path, char *dir_out, uint32_t dir_size, char *pattern_out, uint32_t pattern_size)
{
    int len = (int)strlen(path);
    int slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') slash = i;
    }

    if (slash < 0) {
        strncpy(dir_out, "/", dir_size);
        strncpy(pattern_out, path, pattern_size);
        return;
    }

    uint32_t dlen = (uint32_t)slash;
    if (dlen == 0) dlen = 1; /* root */
    if (dlen >= dir_size) dlen = dir_size - 1;
    memcpy(dir_out, path, dlen);
    dir_out[dlen] = '\0';

    const char *name = path + slash + 1;
    strncpy(pattern_out, name, pattern_size);
}

static void fill_findbuf(FILEFINDBUF *buf, const vfs_stat_t *st, const char *name)
{
    memset(buf, 0, sizeof(*buf));
    buf->file_size = st->size;
    buf->alloc_size = st->size;
    buf->attributes = vfs_to_os2_attrs(st->attrs);

    uint32_t n = (uint32_t)strlen(name);
    if (n > sizeof(buf->name) - 1) n = sizeof(buf->name) - 1;
    memcpy(buf->name, name, n);
    buf->name[n] = '\0';
    buf->name_len = (uint8_t)n;
}

/* Directory search state: HDIR is a VFS dfd plus the remembered pattern. */
typedef struct {
    int dfd;
    char dir[VFS_PATH_MAX];
    char pattern[VFS_NAME_MAX + 1];
} find_state_t;

#define MAX_FIND_HANDLES 16
static find_state_t find_states[MAX_FIND_HANDLES];
static bool find_used[MAX_FIND_HANDLES];

static int alloc_find_handle(void)
{
    for (int i = 0; i < MAX_FIND_HANDLES; i++) {
        if (!find_used[i]) return i;
    }
    return -1;
}

static APIRET find_next_locked(find_state_t *fs, FILEFINDBUF *buffer, uint32_t *count)
{
    vfs_dirent_t ent;
    while (vfs_readdir(fs->dfd, &ent) == VFS_OK) {
        if (!wildcard_match(fs->pattern, ent.name)) continue;

        char full[VFS_PATH_MAX];
        uint32_t dlen = (uint32_t)strlen(fs->dir);
        uint32_t i = 0;
        while (i < dlen && i < sizeof(full) - 1) { full[i] = fs->dir[i]; i++; }
        if (i == 0 || full[i - 1] != '/') { if (i < sizeof(full) - 1) full[i++] = '/'; }
        uint32_t j = 0;
        while (ent.name[j] && i < sizeof(full) - 1) full[i++] = ent.name[j++];
        full[i] = '\0';

        vfs_stat_t st;
        if (vfs_stat(full, &st) != VFS_OK) continue;

        fill_findbuf(buffer, &st, ent.name);
        if (count) *count = 1;
        return NO_ERROR;
    }
    if (count) *count = 0;
    return ERROR_NO_MORE_FILES;
}

APIRET DosFindFirst(const char *pattern, HDIR *handle, uint32_t attributes,
                     FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count)
{
    (void)attributes; (void)buffer_size;
    os2_api_ensure_init();
    if (!handle || !buffer) return ERROR_INVALID_PARAMETER;

    char resolved[VFS_PATH_MAX];
    DosResolvePath(pattern, resolved, sizeof(resolved));

    int slot = alloc_find_handle();
    if (slot < 0) return ERROR_NOT_ENOUGH_MEMORY;

    find_state_t *fs = &find_states[slot];
    split_pattern(resolved, fs->dir, sizeof(fs->dir), fs->pattern, sizeof(fs->pattern));

    int dfd = vfs_opendir(fs->dir);
    if (dfd < 0) return vfs_to_os2_error(dfd);
    fs->dfd = dfd;
    find_used[slot] = true;

    APIRET r = find_next_locked(fs, buffer, count);
    if (r != NO_ERROR) {
        vfs_closedir(fs->dfd);
        find_used[slot] = false;
        return r;
    }

    *handle = (HDIR)slot;
    return NO_ERROR;
}

APIRET DosFindNext(HDIR handle, FILEFINDBUF *buffer, uint32_t buffer_size, uint32_t *count)
{
    (void)buffer_size;
    if (handle >= MAX_FIND_HANDLES || !find_used[handle] || !buffer) return ERROR_INVALID_HANDLE;
    return find_next_locked(&find_states[handle], buffer, count);
}

APIRET DosFindClose(HDIR handle)
{
    if (handle >= MAX_FIND_HANDLES || !find_used[handle]) return ERROR_INVALID_HANDLE;
    vfs_closedir(find_states[handle].dfd);
    find_used[handle] = false;
    return NO_ERROR;
}

/* ── Metadata / directory tree ──────────────────────────────────────────── */

APIRET DosQueryPathInfo(const char *filename, uint32_t info_level, FILESTATUS *info, uint32_t buf_size)
{
    (void)info_level; (void)buf_size;
    os2_api_ensure_init();
    if (!info) return ERROR_INVALID_PARAMETER;

    char resolved[VFS_PATH_MAX];
    DosResolvePath(filename, resolved, sizeof(resolved));

    vfs_stat_t st;
    int r = vfs_stat(resolved, &st);
    if (r != VFS_OK) return vfs_to_os2_error(r);

    memset(info, 0, sizeof(*info));
    info->file_size = st.size;
    info->alloc_size = st.size;
    info->attributes = vfs_to_os2_attrs(st.attrs);
    return NO_ERROR;
}

APIRET DosDelete(const char *filename)
{
    os2_api_ensure_init();
    char resolved[VFS_PATH_MAX];
    DosResolvePath(filename, resolved, sizeof(resolved));
    int r = vfs_unlink(resolved);
    return vfs_to_os2_error(r);
}

APIRET DosMove(const char *old_name, const char *new_name)
{
    os2_api_ensure_init();
    char old_resolved[VFS_PATH_MAX];
    char new_resolved[VFS_PATH_MAX];
    DosResolvePath(old_name, old_resolved, sizeof(old_resolved));
    DosResolvePath(new_name, new_resolved, sizeof(new_resolved));
    int r = vfs_rename(old_resolved, new_resolved);
    return vfs_to_os2_error(r);
}

APIRET DosCreateDir(const char *dirname)
{
    os2_api_ensure_init();
    char resolved[VFS_PATH_MAX];
    DosResolvePath(dirname, resolved, sizeof(resolved));
    int r = vfs_mkdir(resolved);
    return vfs_to_os2_error(r);
}

APIRET DosDeleteDir(const char *dirname)
{
    /* The VFS has no dedicated rmdir op; unlink is the closest available
     * primitive, and every current filesystem driver is read-only anyway. */
    os2_api_ensure_init();
    char resolved[VFS_PATH_MAX];
    DosResolvePath(dirname, resolved, sizeof(resolved));
    int r = vfs_unlink(resolved);
    return vfs_to_os2_error(r);
}

/* ── Stack-pointer thunking (legacy 16/32-bit no-ops; real on ARM64) ─────── */

VOID APIENTRY KernThunkStackTo16(VOID)
{
    /* Real OS/2 used this to switch a 16:32 SS:ESP value to a 16:16 SS:SP
     * value when calling into 16-bit segmented code. There is no 16-bit
     * mode, no segmentation, and no mixed-mode calling on this flat
     * 64-bit AArch64 port - nothing to thunk, so this is a deliberate no-op
     * kept only so existing OS/2 callers of it still link. */
}

VOID APIENTRY KernThunkStackTo32(VOID)
{
    /* Real OS/2 used this to switch a 16:16 SS:SP value back to a 16:32
     * SS:ESP value when returning from 16-bit segmented code to 32-bit
     * code. Same reasoning as KernThunkStackTo16(): no segmentation on
     * this flat 64-bit AArch64 port, so nothing to thunk - kept only so
     * existing OS/2 callers of it still link. */
}

VOID APIENTRY KernThunkStackTo64(VOID)
{
    /* Not a real OS/2 API (OS/2 never had a 64-bit mode); added for this
     * ARM64 port so a 32-bit-shaped caller can land back on the native
     * flat 64-bit stack. Unlike the two thunks above, SP here genuinely
     * *is* the real, single, 64-bit AArch64 stack pointer already - so
     * this just reads it back, both to document that fact and to give
     * callers a real (if trivial) value instead of a pure no-op. */
    uintptr_t sp;
#if defined(__riscv)
    __asm__ volatile("mv %0, sp" : "=r"(sp));   /* BeagleV-Ahead (RV64) */
#else
    __asm__ volatile("mov %0, sp" : "=r"(sp));  /* AArch64, ARMv7 (BBB) */
#endif
    (void)sp;
}

/* ── 16-bit device driver serialization (legacy no-ops) ───────────────────
 * Real OS/2 required this pair around any call into a 16-bit device
 * driver: 16-bit DDs weren't reentrant, so a caller had to serialize
 * (acquire a global lock) before entering one and unserialize (release it)
 * after returning, guarding against another CPU/thread entering 16-bit
 * driver code concurrently. This port has no 16-bit device drivers at all
 * - every driver here (fs/, drivers/video/) is native AArch64 C - and per
 * scheduler.c, this kernel is single-threaded with no preemption yet, so
 * there is nothing to serialize against either way. Both kept as no-ops
 * only so existing OS/2 callers of them still link. */

VOID APIENTRY KernSerialize16BitDD(VOID)
{
}

VOID APIENTRY KernUnSerialize16BitDD(VOID)
{
}

/** Copy a buffer of user space data into a kernel buffer. **/

APIRET APIENTRY KernCopyIn(PVOID trgt, PVOID src, ULONG size)
{
    /* Real OS/2 validated src against the calling ring-3 process's address
     * space (its LDT/segment limits) before copying, since kernel and user
     * code lived in separate address spaces. This port has no MMU-backed
     * process isolation - kernel, CMD.EXE, and everything else share one
     * flat address space (see README.md) - so there is no separate process
     * boundary to check src against. What *can* still be validated here,
     * architecture-independent of any MMU: that trgt/src aren't NULL, that
     * trgt+size/src+size don't overflow and wrap the address space (which
     * would otherwise silently copy to/from the wrong place), and that the
     * two regions don't overlap (memcpy() is undefined behavior on
     * overlapping buffers; memmove() below is the safe alternative). */
    if (!trgt || !src) return ERROR_INVALID_PARAMETER;
    if (size == 0) return NO_ERROR;

    uintptr_t trgt_addr = (uintptr_t)trgt;
    uintptr_t src_addr = (uintptr_t)src;
    if (trgt_addr + size < trgt_addr) return ERROR_INVALID_PARAMETER;
    if (src_addr + size < src_addr) return ERROR_INVALID_PARAMETER;

    memmove(trgt, src, size);
    return NO_ERROR;
}

APIRET APIENTRY KernCopyOut(PVOID trgt, PVOID src, ULONG size)
{
    /* Real OS/2 validated src against the calling ring-3 process's address
     * space (its LDT/segment limits) before copying, since kernel and user
     * code lived in separate address spaces. This port has no MMU-backed
     * process isolation - kernel, CMD.EXE, and everything else share one
     * flat address space (see README.md) - so there is no separate process
     * boundary to check src against. What *can* still be validated here,
     * architecture-independent of any MMU: that trgt/src aren't NULL, that
     * trgt+size/src+size don't overflow and wrap the address space (which
     * would otherwise silently copy to/from the wrong place), and that the
     * two regions don't overlap (memcpy() is undefined behavior on
     * overlapping buffers; memmove() below is the safe alternative). */
    if (!trgt || !src) return ERROR_INVALID_PARAMETER;
    if (size == 0) return NO_ERROR;

    uintptr_t trgt_addr = (uintptr_t)trgt;
    uintptr_t src_addr = (uintptr_t)src;
    if (trgt_addr + size < trgt_addr) return ERROR_INVALID_PARAMETER;
    if (src_addr + size < src_addr) return ERROR_INVALID_PARAMETER;

    memmove(trgt, src, size);
    return NO_ERROR;
}

extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

APIRET APIENTRY KernVMAlloc(ULONG size, ULONG flags, PVOID *linaddr, PULONG physaddr, PULONG sel)
{
    if (!linaddr || size == 0) return ERROR_INVALID_PARAMETER;

    /* This port has one flat address space with no MMU and a simple bump
     * allocator (mem_alloc(), kernel/src/mem.c) - no physical-vs-linear
     * address distinction, no swapping, no GDT selectors, and no way to
     * place an allocation at a caller-chosen or range-constrained address.
     * Flags that are just hints in that world - FIXED/SWAP (nothing is
     * ever swapped either way), SHARED/PROCESS (everything already shares
     * the one address space), SGSCONT (no screen-group bookkeeping to
     * register with), RESERVE (mem_alloc always commits immediately - a
     * safe superset of "don't commit yet"), CONTIG (a bump allocator's
     * output is always contiguous), and GETSEL (no GDT to return a
     * selector from) - are accepted and effectively no-ops. Flags that
     * promise a specific placement this allocator cannot deliver - PHYS
     * (a caller-chosen physical address), 16MB/USEHIGHMEM (an address
     * range guarantee) - are honestly rejected rather than silently
     * ignored: a caller relying on e.g. sub-16MB memory for legacy DMA
     * must not be told it succeeded when it didn't. */
    if (flags & (KEE_VMA_PHYS | KEE_VMA_16MB | KEE_VMA_USEHIGHMEM)) {
        return ERROR_NOT_SUPPORTED;
    }

    void *ptr = mem_alloc((size_t)size);
    if (!ptr) return ERROR_NOT_ENOUGH_MEMORY;

    *linaddr = ptr;
    if (physaddr) *physaddr = (ULONG)(uintptr_t)ptr; /* flat: physical == linear */
    if (sel) *sel = 0; /* no segmentation/GDT on this ARM64 port */
    return NO_ERROR;
}

APIRET APIENTRY KernVMFree(PVOID linaddr)
{
    if (!linaddr) return ERROR_INVALID_PARAMETER;

    /* mem_alloc() (kernel/src/mem.c) is a simple bump allocator with no
     * free-list, so mem_free() is intentionally a no-op - this can't
     * actually reclaim the memory. Still validates the pointer and
     * returns success, same as KernVMAlloc's flag handling above: a
     * caller checking the return code gets an honest NO_ERROR for a
     * well-formed free, it just can't be told the memory is reusable. */
    mem_free(linaddr);
    return NO_ERROR;
}

APIRET APIENTRY KernVMLock(ULONG flags, PVOID linaddr, ULONG size,
                            KEEVMLOCK *lock, KEEVMPageList *pglist, PULONG pgsize)
{
    (void)flags;

    if (!linaddr || size == 0) return ERROR_INVALID_PARAMETER;

    /* This port has one flat address space with no MMU and nothing is
     * ever paged out (see the comment above KernVMAlloc) - so every
     * region is already permanently resident, and locking it against
     * swap-out (or just verifying it's accessible, KEE_VML_VERIFYONLY)
     * trivially succeeds for any non-NULL pointer/size. There's no real
     * lock handle or page list to hand back either; zero them out so a
     * caller that inspects lock/pglist/pgsize doesn't mistake
     * uninitialized memory for real entries. */
    if (lock) memset(lock, 0, sizeof(*lock));
    if (pglist) memset(pglist, 0, sizeof(*pglist));
    if (pgsize) *pgsize = 0;
    return NO_ERROR;
}

APIRET APIENTRY KernVMUnlock(KEEVMLOCK *lock)
{
    if (!lock) return ERROR_INVALID_PARAMETER;

    /* Nothing to actually release - KernVMLock() above never took a real
     * lock (this port has no MMU/paging, so nothing needed protecting
     * against swap-out in the first place; see its comment). Just
     * validate the handle and clear it so a stale lock can't be reused
     * as if it were still valid. */
    memset(lock, 0, sizeof(*lock));
    return NO_ERROR;
}

APIRET APIENTRY KernLinToPageList(PVOID linaddr, ULONG size, KEEVMPageList *list, PULONG pgsize)
{
    if (!linaddr || size == 0) return ERROR_INVALID_PARAMETER;

    /* This port has no MMU-backed paging (see the comment above
     * KernVMAlloc) - physical address always equals linear address, so
     * there's no real per-page frame table to build the way a real
     * VMLinToPageList would. Same honest-stub treatment as KernVMLock/
     * KernVMUnlock above: validate the range, report zero pages rather
     * than fabricate frame numbers a caller might mistake for real ones. */
    if (list) memset(list, 0, sizeof(*list));
    if (pgsize) *pgsize = 0;
    return NO_ERROR;
}

APIRET APIENTRY KernVMSetMem(ULONG flags, PVOID linaddr, ULONG size)
{
    (void)flags;

    if (!linaddr || size == 0) return ERROR_INVALID_PARAMETER;

    /* This port has one flat address space with no MMU and a bump
     * allocator that always commits immediately (see the KEE_VMA_RESERVE
     * comment above KernVMAlloc) - there's no committed-vs-reserved state
     * to change. Nothing is ever paged out (see KernVMLock above) and
     * nothing is ever swapped (see KEE_VMA_SWAP above either), so
     * KEE_VMS_UNCOMMIT, KEE_VMS_RESIDENT, and KEE_VMS_SWAPPABLE are all
     * no-ops here: the region is already resident and already committed,
     * and there's no swap subsystem to opt in or out of. Just validate
     * the range and succeed. */
    return NO_ERROR;
}

ULONG APIENTRY KernSelToFlat(ULONG addr16)
{
    /* Real OS/2 resolved a 16:16 selector:offset through the LDT; with
     * its tiled memory layout every LDT selector maps a 64K tile, so the
     * flat address is (selector index * 64K) + offset. This port has no
     * segmentation (and KernVMAlloc never hands out real selectors - see
     * KEE_VMA_GETSEL above), so that fixed tiling formula is the whole
     * conversion: drop the selector's TI/RPL bits and splice in the
     * offset. */
    ULONG sel = addr16 >> 16;
    ULONG off = addr16 & 0xFFFFu;
    return ((sel >> 3) << 16) | off;
}


APIRET APIENTRY KernDynamicAPI(PVOID addr, ULONG cnt, ULONG dummy, PUSHORT sel)
{
    (void)cnt; (void)dummy;

    if (!addr || !sel) return ERROR_INVALID_PARAMETER;

    /* Real OS/2 built a GDT call gate here, so 16-bit code could call the
     * 32-bit routine at addr (with cnt words of parameters copied across
     * the gate), and returned that gate's selector in *sel. This port has
     * no segmentation at all - no GDT, no call gates, no 16-bit code to
     * call through one (see KernThunkStackTo16 and KEE_VMA_GETSEL above) -
     * so there is nothing to create. Refuse honestly rather than hand out
     * a selector that doesn't lead anywhere. */
    *sel = 0;
    return ERROR_NOT_SUPPORTED;
}


APIRET APIENTRY KernRASSysTrace(ULONG major, ULONG minor, PVOID buf, ULONG size)
{
    if (!buf && size != 0) return ERROR_INVALID_PARAMETER;
    if (buf && (uintptr_t)buf + size < (uintptr_t)buf) return ERROR_INVALID_PARAMETER;

    /* Real OS/2 added the RAS event (major/minor number plus buf) to the
     * in-memory RAS log, read back later with TRACEFMT. This port has no
     * RAS log or TRACE= support in CONFIG.SYS - the UART console
     * (kprintf) is its only diagnostic sink - so the event is written
     * there instead: a header line, then buf as a hex dump. */
    kprintf("[RAS] major=0x%04x minor=0x%04x size=%u\n",
            (unsigned)major, (unsigned)minor, (unsigned)size);

    const unsigned char *p = (const unsigned char *)buf;
    for (ULONG i = 0; i < size; i++) {
        kprintf((i % 16 == 0) ? "  %02x" : " %02x", (unsigned)p[i]);
        if (i % 16 == 15 || i == size - 1) kprintf("\n");
    }
    return NO_ERROR;
}
