/**
 * @file lx_dll_loader.c
 * @brief Built-in loader for OS/2 LX (.DLL) libraries — see lx_dll_loader.h
 *
 * The on-disk structs below mirror lx/lxformat.h's LXHeader/LXObject/LXPage
 * field-for-field, duplicated from kernel/src/lx_loader.c rather than
 * shared: that file's copies are static/file-local, and both duplicates
 * exist rather than #include "lxformat.h" because that header pulls in
 * <stdint.h>, which isn't reachable under the kernel's -nostdinc cross-
 * compile. Keep all three (this file, lx_loader.c, lxformat.h) in sync if
 * the LX writer (lx/lxbuilder.c) ever changes its output layout.
 */

#include "lx_dll_loader.h"
#include "uart.h"
#include "kprintf.h"
#include "string.h"

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
#define LXMF_LIBRARY     0x00008000
#define LXPAGE_VALID     0x00

/* Same sanity caps as lx_loader.c - generous for anything this project's
 * own toolchain produces, small enough that the bounds checks below can't
 * overflow even on a corrupt/malicious image. */
#define LX_MAX_OBJECTS   16
#define LX_MAX_PAGES     32768

/* ARM64 self-modifying-code cache maintenance - see lx_loader.c's copy of
 * this for the full explanation (pages were just written as data but are
 * about to be executed as instructions). */
static void lx_dll_flush_icache(void *addr, uint32_t size)
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

/* Copies one LX object's pages to `dest` (an already-mapped, fixed region
 * no bigger than max_size), zero-filling any gaps the file's page table
 * leaves unspecified. Returns 0 on success (including a legitimately
 * empty object, virtual_size == 0 - valid for a DLL with no writable
 * data), a negative LX_DLL_ERR_* on a malformed file. */
static int lx_dll_copy_object(const uint8_t *base, uint32_t image_size,
                              const lx_header_t *hdr, const lx_object_t *obj,
                              uint8_t *dest, uint32_t max_size)
{
    if (obj->virtual_size > max_size) {
        uart_puts("[LX-DLL] Object too large for its region\r\n");
        return LX_DLL_ERR_TOOBIG;
    }
    if (obj->virtual_size == 0)
        return 0;

    uint32_t max_pages_for_size = (obj->virtual_size + hdr->page_size - 1) / hdr->page_size;
    if (obj->page_table_index == 0 ||
        obj->page_table_entries == 0 ||
        obj->page_table_entries > max_pages_for_size ||
        (uint64_t)(obj->page_table_index - 1) + obj->page_table_entries > hdr->module_pages) {
        uart_puts("[LX-DLL] Bad page table range\r\n");
        return LX_DLL_ERR_BADMAGIC;
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
                uart_puts("[LX-DLL] Page data out of bounds\r\n");
                return LX_DLL_ERR_BADMAGIC;
            }
            const uint8_t *src = base + pg->page_data_offset;
            for (uint32_t i = 0; i < pg->data_size; i++)
                dst[i] = src[i];
        }
    }
    return 0;
}

int lx_dll_load_image(const void *image, uint32_t image_size, lx_dll_t *out)
{
    if (!image || !out || image_size < sizeof(lx_header_t)) {
        uart_puts("[LX-DLL] Image too small\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }

    const uint8_t *base = (const uint8_t *)image;
    const lx_header_t *hdr = (const lx_header_t *)base;

    if (hdr->signature[0] != LX_MAGIC1 || hdr->signature[1] != LX_MAGIC2) {
        uart_puts("[LX-DLL] Bad magic - not an LX file\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }
    if (hdr->cpu_type != LXCPU_ARM64) {
        uart_puts("[LX-DLL] Not built for ARM64\r\n");
        return LX_DLL_ERR_BADCPU;
    }
    if (!(hdr->module_flags & LXMF_LIBRARY)) {
        uart_puts("[LX-DLL] Not a DLL (LXMF_LIBRARY not set)\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }
    /* This project's own toolchain (lx/lxbuilder.c, lx/lx_dll.ld) always
     * emits exactly two objects per DLL - code (.text+.rodata) then data
     * (.data+.bss) - so kernel/src/mmu.c can map them RX/RW+NX (W^X)
     * instead of one flat RWX region. */
    if (hdr->object_count != 2) {
        uart_puts("[LX-DLL] Expected exactly 2 objects (code, data)\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }
    if (hdr->page_size < 512 || hdr->page_size > 65536) {
        uart_puts("[LX-DLL] Bad page size\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }
    if (hdr->module_pages == 0 || hdr->module_pages > LX_MAX_PAGES) {
        uart_puts("[LX-DLL] Bad page count\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }

    uint64_t object_table_end = (uint64_t)hdr->object_table_offset +
                                 (uint64_t)hdr->object_count * sizeof(lx_object_t);
    if (hdr->object_table_offset < sizeof(lx_header_t) || object_table_end > image_size) {
        uart_puts("[LX-DLL] Object table out of bounds\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }

    uint64_t page_table_end = (uint64_t)hdr->object_page_table_offset +
                               (uint64_t)hdr->module_pages * sizeof(lx_page_t);
    if (hdr->object_page_table_offset < sizeof(lx_header_t) || page_table_end > image_size) {
        uart_puts("[LX-DLL] Page table out of bounds\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }

    const lx_object_t *objects = (const lx_object_t *)(base + hdr->object_table_offset);
    const lx_object_t *code_obj = &objects[0];
    const lx_object_t *data_obj = &objects[1];

    if (code_obj->virtual_size == 0) {
        uart_puts("[LX-DLL] Empty code object\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }

    /* Resident names / entry tables must fall inside the file too -
     * bounds-checked once here so lx_dll_get_proc() can trust the
     * offsets it's handed without re-validating on every lookup. */
    if (hdr->resident_names_offset >= image_size || hdr->entry_table_offset >= image_size) {
        uart_puts("[LX-DLL] Resident names/entry table out of bounds\r\n");
        return LX_DLL_ERR_BADMAGIC;
    }

    /* Every DLL this project builds is linked (lx/lx_dll.ld) at exactly
     * LX_DLL_LOAD_BASE (code) / LX_DLL_DATA_LOAD_BASE (data), so pages
     * are copied verbatim - no relocation or fixup processing needed,
     * same as the EXE loader. Kept in two separate fixed regions (rather
     * than one flat RWX blob) so kernel/src/mmu.c can give them real,
     * different W^X permissions. */
    int rc;
    rc = lx_dll_copy_object(base, image_size, hdr, code_obj,
                            (uint8_t *)LX_DLL_LOAD_BASE, LX_DLL_CODE_REGION_SIZE);
    if (rc != 0) return rc;
    rc = lx_dll_copy_object(base, image_size, hdr, data_obj,
                            (uint8_t *)LX_DLL_DATA_LOAD_BASE, LX_DLL_DATA_REGION_SIZE);
    if (rc != 0) return rc;

    lx_dll_flush_icache((void *)LX_DLL_LOAD_BASE, code_obj->virtual_size);

    out->code_base = (void *)LX_DLL_LOAD_BASE;
    out->code_size = code_obj->virtual_size;
    out->data_base = (void *)LX_DLL_DATA_LOAD_BASE;
    out->data_size = data_obj->virtual_size;
    out->image = image;
    out->image_size = image_size;
    out->resident_names_offset = hdr->resident_names_offset;
    out->entry_table_offset = hdr->entry_table_offset;

    kprintf("[LX-DLL] Loaded code at %p (%u bytes), data at %p (%u bytes)\r\n",
            out->code_base, out->code_size, out->data_base, out->data_size);
    return 0;
}

void *lx_dll_get_proc(const lx_dll_t *dll, const char *name)
{
    if (!dll || !dll->image || !name)
        return NULL;

    const uint8_t *base = (const uint8_t *)dll->image;
    size_t name_len = strlen(name);

    /* Walk the resident names table (module name at ordinal 0, then each
     * export at its ordinal - see lx_builder_write()'s comment in
     * lx/lxbuilder.c) to find `name`'s ordinal. */
    uint32_t pos = dll->resident_names_offset;
    uint16_t wanted_ordinal = 0;
    int found = 0;

    while (pos < dll->image_size) {
        uint8_t entry_len = base[pos++];
        if (entry_len == 0)
            break; /* terminator */
        if ((uint64_t)pos + entry_len + 2 > dll->image_size)
            break; /* corrupt table - treat as not found */

        const uint8_t *entry_name = base + pos;
        pos += entry_len;
        uint16_t ordinal = (uint16_t)(base[pos] | (base[pos + 1] << 8));
        pos += 2;

        if (entry_len == name_len && memcmp(entry_name, name, name_len) == 0) {
            wanted_ordinal = ordinal;
            found = 1;
            break;
        }
    }
    /* Ordinal 0 is always the module's own name (see the resident names
     * table layout above), never a real export. */
    if (!found || wanted_ordinal == 0)
        return NULL;

    /* Walk the entry table's bundles, counting entries in order (ordinal
     * N is the Nth entry overall, 1-based - see build_entry_table() in
     * lx/lxbuilder.c), until reaching wanted_ordinal. */
    pos = dll->entry_table_offset;
    uint32_t running_ordinal = 0;

    while (pos < dll->image_size) {
        uint8_t count = base[pos++];
        if (count == 0)
            break; /* terminator */
        if ((uint64_t)pos + 3 > dll->image_size)
            break;

        pos++; /* bundle type - this loader only understands plain
                 * 32-bit-offset entries, which is all this project's
                 * own toolchain ever emits */
        uint16_t object = (uint16_t)(base[pos] | (base[pos + 1] << 8));
        pos += 2;

        for (uint8_t i = 0; i < count; i++) {
            running_ordinal++;
            if ((uint64_t)pos + 5 > dll->image_size)
                return NULL;

            uint32_t off = (uint32_t)base[pos + 1] |
                           ((uint32_t)base[pos + 2] << 8) |
                           ((uint32_t)base[pos + 3] << 16) |
                           ((uint32_t)base[pos + 4] << 24);
            pos += 5;

            if (running_ordinal == wanted_ordinal) {
                if (object != 1 || off >= dll->code_size)
                    return NULL;
                return (void *)((uint8_t *)dll->code_base + off);
            }
        }
    }
    return NULL;
}
