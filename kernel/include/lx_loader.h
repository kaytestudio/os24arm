/**
 * @file lx_loader.h
 * @brief Built-in loader for OS/2 LX (.EXE) executables
 *
 * Loads the LX programs produced by this project's own toolchain (see the
 * top-level CMakeLists.txt's add_lx_executable()/add_lx_library() and the
 * CMD.EXE build target, all of which link via lx/lx_app.ld) and runs them
 * in-place, in the kernel's own address space - there is no process
 * isolation or syscall boundary in this project, so "loading" a program
 * just means copying its pages to the fixed addresses they were linked at
 * (LX_LOAD_BASE/LX_DATA_LOAD_BASE) and calling into it like any other
 * function.
 *
 * This only supports the flat, two-object, no-fixup LX files this
 * project's own build produces (see lx/lxbuilder.c and lx/lx_app.ld): a
 * code object (.text+.rodata, executable, read-only) and a data object
 * (.data+.bss, writable, non-executable) - split at a fixed 2MB boundary
 * so kernel/src/mmu.c can give them genuinely different MMU permissions
 * (W^X), unlike a single flat RWX object. It is NOT a general OS/2 LX
 * loader - no fixup/import resolution, no arbitrary per-object placement,
 * no demand paging.
 */

#ifndef LX_LOADER_H
#define LX_LOADER_H

#include "types.h"

/* Fixed load addresses for LX programs built by this project's toolchain.
 * Must match lx/lx_app.ld (link address equals load address by
 * construction, so pages are copied verbatim with no relocation) and
 * kernel/src/mmu.c's W^X mapping for this window. Chosen well clear of
 * the kernel image/heap and the preloaded test ramdisk (blkdev.c's
 * PRELOADED_DISK_ADDR = 0x44000000, 24MB).
 *
 * LX_CODE_REGION_SIZE is the fixed split point every LX app's linker
 * script pads .text/.rodata up to before .data begins - generous headroom
 * for what this project's own toolchain produces (a few hundred KB), not
 * a hard architectural limit; lx_app.ld ASSERTs at link time if a program
 * ever outgrows it. */
#define LX_LOAD_BASE         0x48000000UL
#define LX_CODE_REGION_SIZE  (2u * 1024u * 1024u)
#define LX_LOAD_MAX_SIZE     (16u * 1024u * 1024u)
#define LX_DATA_REGION_SIZE  (LX_LOAD_MAX_SIZE - LX_CODE_REGION_SIZE)
#define LX_DATA_LOAD_BASE    (LX_LOAD_BASE + LX_CODE_REGION_SIZE)

#define LX_ERR_IO        -1  /* couldn't read/stat the file */
#define LX_ERR_BADMAGIC  -2  /* not an LX file (or corrupt) */
#define LX_ERR_BADCPU    -3  /* not built for ARM64 */
#define LX_ERR_TOOBIG    -4  /* image too large for the load window */
#define LX_ERR_NOMEM     -5  /* mem_alloc failed */

/* Load the LX .EXE at `path` from the VFS and run it. Blocks the caller
 * until the loaded program's entry point returns (it may never return -
 * e.g. CMD.EXE's shell loop takes over the UART forever, exactly like
 * calling into another kernel shell). Returns the entry point's return
 * value, or a negative LX_ERR_* on load failure. */
int lx_exec_file(const char *path);

/* Load and run an LX image already sitting in memory (e.g. embedded in
 * the kernel image via .incbin). Same semantics as lx_exec_file(). */
int lx_exec_image(const void *image, uint32_t image_size);

#endif /* LX_LOADER_H */
