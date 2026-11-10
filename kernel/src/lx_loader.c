/**
 * @file lx_loader.c
 * @brief Built-in loader for OS/2 LX (.EXE) executables — see lx_loader.h
 *
 * The on-disk structs below mirror lx/lxformat.h's LXHeader/LXObject/LXPage
 * field-for-field. They're duplicated (rather than #include "lxformat.h")
 * because that header pulls in <stdint.h>, which isn't reachable under the
 * kernel's -nostdinc cross-compile; this file uses the kernel's own
 * types.h builtin-based typedefs instead. Keep the two in sync if the LX
 * writer (lx/lxbuilder.c) ever changes its output layout.
 */

#include "lx_loader.h"
#include "vfs.h"
#include "uart.h"
#include "kprintf.h"
#include "os2_api.h"

extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

#pragma pack(push, 1)

typedef struct {
    uint8_t  signature[2];
    uint8_t  byte_order;
    uint8_t  word_order;
    uint32_t format_level;
    uint16_t cpu_type;
    uint16_t os_type;
    uint32_t module_version;
    uint32_t module_flags;
    uint32_t module_pages;
    uint32_t eip_object;
    uint32_t eip;
    uint32_t esp_object;
    uint32_t esp;
    uint32_t page_size;
    uint32_t page_offset_shift;
    uint32_t fixup_section_size;
    uint32_t fixup_section_checksum;
    uint32_t loader_section_size;
    uint32_t loader_section_checksum;
    uint32_t object_table_offset;
    uint32_t object_count;
    uint32_t object_page_table_offset;
    uint32_t object_iter_pages_offset;
    uint32_t resource_table_offset;
    uint32_t resource_count;
    uint32_t resident_names_offset;
    uint32_t entry_table_offset;
    uint32_t module_directives_offset;
    uint32_t module_directives_count;
    uint32_t fixup_page_table_offset;
    uint32_t fixup_record_table_offset;
    uint32_t import_module_table_offset;
    uint32_t import_module_count;
    uint32_t import_proc_table_offset;
    uint32_t per_page_checksum_offset;
    uint32_t data_pages_offset;
    uint32_t preload_page_count;
    uint32_t nonresident_names_offset;
    uint32_t nonresident_names_length;
    uint32_t nonresident_names_checksum;
    uint32_t auto_ds_object;
    uint32_t debug_info_offset;
    uint32_t debug_info_length;
    uint32_t instance_preload_count;
    uint32_t instance_demand_count;
    uint32_t heap_size;
    uint32_t stack_size;
} lx_header_t;

typedef struct {
    uint32_t virtual_size;
    uint32_t reloc_base_addr;
    uint32_t object_flags;
    uint32_t page_table_index;
    uint32_t page_table_entries;
    uint32_t reserved;
} lx_object_t;

typedef struct {
    uint32_t page_data_offset;
    uint16_t data_size;
    uint16_t flags;
} lx_page_t;

#pragma pack(pop)

/* types.h leaves uint64_t undefined under this kernel's -nostdinc build
 * (see fs/btrfs/btrfs_disk.h for the same workaround); needed here for
 * overflow-safe bounds arithmetic on untrusted 32-bit file fields. */
#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define LX_MAGIC1        'L'
#define LX_MAGIC2        'X'
#define LXCPU_ARM64      0x0A
#define LXPAGE_VALID     0x00

/* Sanity caps on untrusted counts from the file, generous enough for
 * anything this project's own toolchain (or any reasonable real LX file)
 * would produce, but small enough that object_table_end/page_table_end
 * below can't overflow even on a corrupt/malicious file. */
#define LX_MAX_OBJECTS   16
#define LX_MAX_PAGES     32768

typedef int (*lx_entry_fn)(void);

/* ARM64 self-modifying-code cache maintenance: pages were just written as
 * data (vfs_read/plain stores) but are about to be executed as
 * instructions, so the I-cache and D-cache must be explicitly
 * synchronized before jumping in - otherwise the core may fetch stale or
 * torn instructions left over from whatever previously used this memory. */
static void lx_flush_icache(void *addr, uint32_t size)
{
    uintptr_t start = (uintptr_t)addr & ~63UL;
    uintptr_t end = (uintptr_t)addr + size;

    for (uintptr_t a = start; a < end; a += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");

    for (uintptr_t a = start; a < end; a += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* Copies one LX object's pages to `dest` (an already-mapped, fixed
 * region no bigger than max_size), zero-filling any gaps the file's page
 * table leaves unspecified. Returns 0 on success (including a
 * legitimately empty object, virtual_size == 0 - valid for a program
 * with no writable data), a negative LX_ERR_* on a malformed file. */
static int lx_copy_object(const uint8_t *base, uint32_t image_size,
                          const lx_header_t *hdr, const lx_object_t *obj,
                          uint8_t *dest, uint32_t max_size)
{
    if (obj->virtual_size > max_size) {
        uart_puts("[LX] Object too large for its region\r\n");
        return LX_ERR_TOOBIG;
    }
    if (obj->virtual_size == 0)
        return 0;

    /* page_table_entries must actually fit within the object's own
     * (already bounds-checked) virtual_size, and the [page_table_index-1,
     * +page_table_entries) slice must fall inside the file's real page
     * table (bounded above by module_pages). */
    uint32_t max_pages_for_size = (obj->virtual_size + hdr->page_size - 1) / hdr->page_size;
    if (obj->page_table_index == 0 ||
        obj->page_table_entries == 0 ||
        obj->page_table_entries > max_pages_for_size ||
        (uint64_t)(obj->page_table_index - 1) + obj->page_table_entries > hdr->module_pages) {
        uart_puts("[LX] Bad page table range\r\n");
        return LX_ERR_BADMAGIC;
    }

    const lx_page_t *pages = (const lx_page_t *)(base + hdr->object_page_table_offset);
    const lx_page_t *obj_pages = &pages[obj->page_table_index - 1];

    for (uint32_t p = 0; p < obj->page_table_entries; p++) {
        const lx_page_t *pg = &obj_pages[p];
        uint8_t *dst = dest + (uint32_t)p * hdr->page_size;

        for (uint32_t i = 0; i < hdr->page_size; i++)
            dst[i] = 0;

        if (pg->flags == LXPAGE_VALID && pg->data_size > 0) {
            if (pg->data_size > hdr->page_size ||
                (uint64_t)pg->page_data_offset + pg->data_size > image_size) {
                uart_puts("[LX] Page data out of bounds\r\n");
                return LX_ERR_BADMAGIC;
            }
            const uint8_t *src = base + pg->page_data_offset;
            for (uint32_t i = 0; i < pg->data_size; i++)
                dst[i] = src[i];
        }
    }
    return 0;
}

int lx_exec_image(const void *image, uint32_t image_size)
{
    if (!image || image_size < sizeof(lx_header_t)) {
        uart_puts("[LX] Image too small\r\n");
        return LX_ERR_BADMAGIC;
    }

    const uint8_t *base = (const uint8_t *)image;
    const lx_header_t *hdr = (const lx_header_t *)base;

    if (hdr->signature[0] != LX_MAGIC1 || hdr->signature[1] != LX_MAGIC2) {
        uart_puts("[LX] Bad magic - not an LX file\r\n");
        return LX_ERR_BADMAGIC;
    }
    if (hdr->cpu_type != LXCPU_ARM64) {
        uart_puts("[LX] Not built for ARM64\r\n");
        return LX_ERR_BADCPU;
    }
    /* This project's own toolchain (lx/lxbuilder.c, lx/lx_app.ld) always
     * emits exactly two objects - code (.text+.rodata) then data
     * (.data+.bss) - so kernel/src/mmu.c can map them RX/RW+NX (W^X)
     * instead of one flat RWX region. eip_object must be 1 (code); real
     * OS/2 LX allows the entry point in any object, but there's nothing
     * else this loader is prepared to place it in. */
    if (hdr->object_count != 2 || hdr->eip_object != 1) {
        uart_puts("[LX] Expected exactly 2 objects (code, data) with entry in object 1\r\n");
        return LX_ERR_BADMAGIC;
    }
    if (hdr->page_size < 512 || hdr->page_size > 65536) {
        uart_puts("[LX] Bad page size\r\n");
        return LX_ERR_BADMAGIC;
    }
    if (hdr->module_pages == 0 || hdr->module_pages > LX_MAX_PAGES) {
        uart_puts("[LX] Bad page count\r\n");
        return LX_ERR_BADMAGIC;
    }

    /* Everything below is untrusted, possibly-corrupt file data driving
     * pointer arithmetic - validate every offset/count against the actual
     * buffer before using it. A file that failed one of these checks used
     * to be read (or written!) out of bounds instead of rejected: e.g.
     * page_table_entries drove the copy loop below with no relation to
     * the already-validated virtual_size, so a small virtual_size plus a
     * huge page_table_entries was an unbounded out-of-bounds write. */
    uint64_t object_table_end = (uint64_t)hdr->object_table_offset +
                                 (uint64_t)hdr->object_count * sizeof(lx_object_t);
    if (hdr->object_table_offset < sizeof(lx_header_t) || object_table_end > image_size) {
        uart_puts("[LX] Object table out of bounds\r\n");
        return LX_ERR_BADMAGIC;
    }

    uint64_t page_table_end = (uint64_t)hdr->object_page_table_offset +
                               (uint64_t)hdr->module_pages * sizeof(lx_page_t);
    if (hdr->object_page_table_offset < sizeof(lx_header_t) || page_table_end > image_size) {
        uart_puts("[LX] Page table out of bounds\r\n");
        return LX_ERR_BADMAGIC;
    }

    const lx_object_t *objects = (const lx_object_t *)(base + hdr->object_table_offset);
    const lx_object_t *code_obj = &objects[0];
    const lx_object_t *data_obj = &objects[1];

    if (code_obj->virtual_size == 0 || hdr->eip >= code_obj->virtual_size) {
        uart_puts("[LX] Entry point outside code object\r\n");
        return LX_ERR_BADMAGIC;
    }

    /* Every LX program this project builds is linked (lx/lx_app.ld) at
     * exactly LX_LOAD_BASE (code) / LX_DATA_LOAD_BASE (data), so pages
     * are copied verbatim - no relocation or fixup processing needed
     * (and this project's own toolchain never emits any; see
     * lx/lxbuilder.c). Kept in two separate fixed regions (rather than
     * one flat RWX blob) so kernel/src/mmu.c can give them real, different
     * W^X permissions. */
    int rc;
    rc = lx_copy_object(base, image_size, hdr, code_obj,
                        (uint8_t *)LX_LOAD_BASE, LX_CODE_REGION_SIZE);
    if (rc != 0) return rc;
    rc = lx_copy_object(base, image_size, hdr, data_obj,
                        (uint8_t *)LX_DATA_LOAD_BASE, LX_DATA_REGION_SIZE);
    if (rc != 0) return rc;

    lx_flush_icache((void *)LX_LOAD_BASE, code_obj->virtual_size);

    kprintf("[LX] Loaded code at %p, data at %p, entry +0x%x\r\n",
            (void *)LX_LOAD_BASE, (void *)LX_DATA_LOAD_BASE, hdr->eip);

    lx_entry_fn entry = (lx_entry_fn)((uint8_t *)LX_LOAD_BASE + hdr->eip);
    return entry();
}

int lx_exec_file(const char *path)
{
    /* vfs_* wants an absolute path (find_mount() only matches paths
     * starting with "/"); resolve relative names (e.g. "CMD.EXE") against
     * the current directory the same way DosOpen/DIR/TYPE/etc. do. */
    char resolved[VFS_PATH_MAX];
    DosResolvePath(path, resolved, sizeof(resolved));

    vfs_stat_t st;
    if (vfs_stat(resolved, &st) != VFS_OK) {
        uart_puts("[LX] Cannot stat ");
        uart_puts(path);
        uart_puts("\r\n");
        return LX_ERR_IO;
    }
    if (st.size == 0 || st.size > LX_LOAD_MAX_SIZE) {
        uart_puts("[LX] Bad file size\r\n");
        return LX_ERR_TOOBIG;
    }

    int fd = vfs_open(resolved, O_RDONLY);
    if (fd < 0) {
        uart_puts("[LX] Cannot open ");
        uart_puts(path);
        uart_puts("\r\n");
        return LX_ERR_IO;
    }

    uint8_t *buf = (uint8_t *)mem_alloc(st.size);
    if (!buf) {
        vfs_close(fd);
        return LX_ERR_NOMEM;
    }

    uint32_t total = 0;
    while (total < st.size) {
        int n = vfs_read(fd, buf + total, st.size - total);
        if (n <= 0)
            break;
        total += (uint32_t)n;
    }
    vfs_close(fd);

    if (total != st.size) {
        mem_free(buf);
        uart_puts("[LX] Short read\r\n");
        return LX_ERR_IO;
    }

    int rc = lx_exec_image(buf, total);
    mem_free(buf);
    return rc;
}
