/**
 * @file lx_dll_loader.h
 * @brief Built-in loader for OS/2 LX (.DLL) libraries
 *
 * Companion to lx_loader.h's EXE loader: loads an LX DLL (module_flags &
 * LXMF_LIBRARY) already sitting in memory - normally an .incbin'd copy
 * embedded directly into the calling EXE's own image (see
 * os2api/doscalls_embed.S.in) - and resolves its exports by name.
 *
 * Same constraints as lx_loader.h: this project's flat address space has
 * no fixup/relocation processing, so a DLL is always copied verbatim to
 * the single fixed pair of addresses it was linked at (lx/lx_dll.ld) - it
 * can't be relocated to load at a caller-chosen address, and (today) only
 * one DLL can be resident at a time, since there's only one such fixed
 * pair.
 *
 * Like lx_loader.h's EXE loader, a DLL is two objects - code
 * (.text+.rodata, RX) at LX_DLL_LOAD_BASE and data (.data+.bss, RW+NX) at
 * LX_DLL_DATA_LOAD_BASE, split at a fixed 2MB boundary - so
 * kernel/src/mmu.c can give them real, different W^X permissions instead
 * of one flat RWX region.
 */

#ifndef LX_DLL_LOADER_H
#define LX_DLL_LOADER_H

#include "types.h"

/* Fixed load addresses for LX DLLs built by this project's toolchain (see
 * lx/lx_dll.ld). Deliberately distinct from lx_loader.h's LX_LOAD_BASE
 * (0x48000000, +16MB) and CMD.EXE/KAYTEVM.EXE's own private heap
 * (0x49000000, +4MB) - an EXE loading a DLL is itself already resident
 * at LX_LOAD_BASE, so the DLL needs separate addresses to live at.
 *
 * LX_DLL_CODE_REGION_SIZE is the fixed split point the DLL's linker
 * script pads .text/.rodata up to before .data begins - generous
 * headroom for what this project's own toolchain produces (DOSCALLS.DLL
 * is under 80KB today), not a hard architectural limit; lx_dll.ld ASSERTs
 * at link time if a DLL ever outgrows it. */
#define LX_DLL_LOAD_BASE         0x4A000000UL
#define LX_DLL_CODE_REGION_SIZE  (2u * 1024u * 1024u)
#define LX_DLL_LOAD_MAX_SIZE     (4u * 1024u * 1024u)
#define LX_DLL_DATA_REGION_SIZE  (LX_DLL_LOAD_MAX_SIZE - LX_DLL_CODE_REGION_SIZE)
#define LX_DLL_DATA_LOAD_BASE    (LX_DLL_LOAD_BASE + LX_DLL_CODE_REGION_SIZE)

#define LX_DLL_ERR_BADMAGIC  -1  /* not an LX file, or not a DLL */
#define LX_DLL_ERR_BADCPU    -2  /* not built for ARM64 */
#define LX_DLL_ERR_TOOBIG    -3  /* image too large for the load window */
#define LX_DLL_ERR_NOTFOUND  -4  /* lx_dll_get_proc: no export with that name */

typedef struct {
    void       *code_base;             /* where the DLL's code object now lives (LX_DLL_LOAD_BASE) */
    uint32_t    code_size;             /* size of the code object, in bytes */
    void       *data_base;             /* where the DLL's data object now lives (LX_DLL_DATA_LOAD_BASE) */
    uint32_t    data_size;             /* size of the data object, in bytes */

    /* The resident names / entry tables (needed by lx_dll_get_proc())
     * live in the ORIGINAL file image, not in the copied objects above -
     * a DLL's on-disk layout is [code object][data object][resident
     * names table][entry table][...], and only the code/data objects get
     * copied to LX_DLL_LOAD_BASE/LX_DLL_DATA_LOAD_BASE (see
     * lx_dll_load_image()). Callers must keep `image` alive for as long
     * as they intend to call lx_dll_get_proc() - normally trivial, since
     * it's an .incbin'd blob in .rodata that lives for the whole
     * process. */
    const void *image;
    uint32_t    image_size;
    uint32_t    resident_names_offset;
    uint32_t    entry_table_offset;
} lx_dll_t;

/* Copies `image`'s code object to LX_DLL_LOAD_BASE and its data object to
 * LX_DLL_DATA_LOAD_BASE, and fills in *out. Does not run any code in the
 * DLL (there is no per-DLL init routine in this scheme) - callers resolve
 * and call individual exports themselves via lx_dll_get_proc(). Returns 0
 * on success, a negative LX_DLL_ERR_* on failure. */
int lx_dll_load_image(const void *image, uint32_t image_size, lx_dll_t *out);

/* Looks `name` up in the DLL's resident names table and returns a
 * pointer to that export within the loaded code object (dll->code_base +
 * its entry-table offset - exports are always functions, so always in
 * the code object), or NULL if there's no export by that name. */
void *lx_dll_get_proc(const lx_dll_t *dll, const char *name);

#endif /* LX_DLL_LOADER_H */
