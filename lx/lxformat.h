/* ============================================================================
 * tools/lxformat.h  —  OS/2 LX (Linear eXecutable) Format Specification
 *
 * Defines structures and constants for creating OS/2 LX executables and DLLs.
 * 
 * LX format is used for:
 *   - OS/2 32-bit executables (.EXE)
 *   - OS/2 Dynamic Link Libraries (.DLL)
 *   - OS/2 Physical Device Drivers (.SYS)
 * ========================================================================== */

#ifndef LXFORMAT_H
#define LXFORMAT_H

#include <stdint.h>

/* ── LX Magic Numbers ────────────────────────────────────────────────────── */

#define LX_MAGIC1  'L'
#define LX_MAGIC2  'X'
#define LX_SIGNATURE  0x584C  /* "LX" */

/* ── LX Module Flags ─────────────────────────────────────────────────────── */

#define LXMF_PROGRAM          0x00000000  /* Program module (EXE) */
#define LXMF_LIBRARY          0x00008000  /* Library module (DLL) */
#define LXMF_PROTECTED_DLL    0x00010000  /* Protected memory DLL */
#define LXMF_PHYSICAL_DEVICE  0x00020000  /* Physical device driver */
#define LXMF_VIRTUAL_DEVICE   0x00028000  /* Virtual device driver */
#define LXMF_INIT_INSTANCE    0x00000004  /* Per-process initialization */
#define LXMF_INIT_GLOBAL      0x00000000  /* Global initialization */
#define LXMF_INTERNAL_FIXUPS  0x00000010  /* Internal fixups in load image */
#define LXMF_EXTERNAL_FIXUPS  0x00000020  /* External fixups in load image */
#define LXMF_INCOMPATIBLE_PM  0x00000100  /* Not compatible with PM */
#define LXMF_COMPATIBLE_PM    0x00000200  /* Compatible with PM */
#define LXMF_USES_PM          0x00000300  /* Uses PM API */
#define LXMF_NOT_LOADABLE     0x00002000  /* Not loadable (errors) */

/* ── LX CPU Types ────────────────────────────────────────────────────────── */

#define LXCPU_286    0x01
#define LXCPU_386    0x02
#define LXCPU_486    0x03
#define LXCPU_586    0x04  /* Pentium */
#define LXCPU_ARM64  0x0A  /* ARM64 (custom for OS/2 Warp ARM64) */

/* ── LX OS Types ─────────────────────────────────────────────────────────── */

#define LXOS_OS2     0x01  /* OS/2 */
#define LXOS_WINDOWS 0x02  /* Windows */
#define LXOS_DOS     0x03  /* DOS 4.x */
#define LXOS_WIN386  0x04  /* Windows 386 */

/* ── LX Object Flags ─────────────────────────────────────────────────────── */

#define LXOBJ_READABLE        0x00000001  /* Readable */
#define LXOBJ_WRITABLE        0x00000002  /* Writable */
#define LXOBJ_EXECUTABLE      0x00000004  /* Executable */
#define LXOBJ_RESOURCE        0x00000008  /* Resource */
#define LXOBJ_DISCARDABLE     0x00000010  /* Discardable */
#define LXOBJ_SHARED          0x00000020  /* Shared */
#define LXOBJ_PRELOAD         0x00000040  /* Preload */
#define LXOBJ_INVALID         0x00000080  /* Invalid */
#define LXOBJ_ZEROFILLED      0x00000100  /* Zero filled */
#define LXOBJ_RESIDENT        0x00000200  /* Resident */
#define LXOBJ_CONTIGUOUS      0x00000300  /* Resident & Contiguous */
#define LXOBJ_LONG_LOCKABLE   0x00000400  /* Long lockable */
#define LXOBJ_16_16_ALIAS     0x00001000  /* 16:16 alias required */
#define LXOBJ_BIG_DEFAULT     0x00002000  /* Big/Default bit setting */
#define LXOBJ_CONFORMING      0x00004000  /* Conforming */
#define LXOBJ_IOPL            0x00008000  /* IOPL */

/* ── LX Page Flags ───────────────────────────────────────────────────────── */

#define LXPAGE_VALID          0x00  /* Valid physical page */
#define LXPAGE_ITERDATA       0x01  /* Iterated data */
#define LXPAGE_INVALID        0x02  /* Invalid page */
#define LXPAGE_ZEROED         0x03  /* Zero-filled page */
#define LXPAGE_RANGE          0x04  /* Range of pages */

/* ── LX Entry Table Bundle Types ─────────────────────────────────────────── */

#define LXENTRY_TYPE_UNUSED   0x00  /* Empty bundle (skips ordinals) */
#define LXENTRY_TYPE_16BIT    0x01  /* 16-bit offset entry */
#define LXENTRY_TYPE_CALLGATE 0x02  /* 286 call gate entry */
#define LXENTRY_TYPE_32BIT    0x03  /* 32-bit offset entry (what this project's toolchain emits) */
#define LXENTRY_TYPE_FORWARDER 0x04 /* Forwarder to another module's export */

#define LXENTRY_FLAG_EXPORTED 0x01  /* Entry is exported */
#define LXENTRY_FLAG_SHARED   0x02  /* Shared data entry */

/* ══════════════════════════════════════════════════════════════════════════
   LX HEADER STRUCTURE
   ══════════════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)

typedef struct {
    /* ── Signature ────────────────────────────────────────────────────── */
    uint8_t  signature[2];        /* "LX" magic bytes */
    uint8_t  byte_order;          /* Byte ordering (0=little endian) */
    uint8_t  word_order;          /* Word ordering (0=little endian) */
    uint32_t format_level;        /* Executable format level */
    uint16_t cpu_type;            /* CPU type (386, 486, ARM64) */
    uint16_t os_type;             /* Target OS (OS/2, Windows) */
    uint32_t module_version;      /* Module version */
    uint32_t module_flags;        /* Module flags (EXE, DLL, etc.) */
    
    /* ── Module Information ───────────────────────────────────────────── */
    uint32_t module_pages;        /* Number of memory pages */
    uint32_t eip_object;          /* Object number for EIP */
    uint32_t eip;                 /* Starting EIP value */
    uint32_t esp_object;          /* Object number for ESP */
    uint32_t esp;                 /* Starting ESP value */
    uint32_t page_size;           /* Page size (4096 bytes) */
    uint32_t page_offset_shift;   /* Page offset shift */
    uint32_t fixup_section_size;  /* Fixup section size */
    uint32_t fixup_section_checksum; /* Fixup section checksum */
    uint32_t loader_section_size; /* Loader section size */
    uint32_t loader_section_checksum; /* Loader section checksum */
    
    /* ── Table Offsets ────────────────────────────────────────────────── */
    uint32_t object_table_offset; /* Offset to object table */
    uint32_t object_count;        /* Number of objects */
    uint32_t object_page_table_offset; /* Offset to object page table */
    uint32_t object_iter_pages_offset; /* Offset to iterated pages */
    uint32_t resource_table_offset;    /* Offset to resource table */
    uint32_t resource_count;           /* Number of resources */
    uint32_t resident_names_offset;    /* Offset to resident names */
    uint32_t entry_table_offset;       /* Offset to entry table */
    uint32_t module_directives_offset; /* Offset to module directives */
    uint32_t module_directives_count;  /* Number of directives */
    uint32_t fixup_page_table_offset;  /* Offset to fixup page table */
    uint32_t fixup_record_table_offset; /* Offset to fixup records */
    uint32_t import_module_table_offset; /* Offset to import modules */
    uint32_t import_module_count;      /* Number of import modules */
    uint32_t import_proc_table_offset; /* Offset to import procedures */
    uint32_t per_page_checksum_offset; /* Offset to checksums */
    uint32_t data_pages_offset;        /* Offset to data pages */
    uint32_t preload_page_count;       /* Number of preload pages */
    uint32_t nonresident_names_offset; /* Offset to non-resident names */
    uint32_t nonresident_names_length; /* Length of non-resident names */
    uint32_t nonresident_names_checksum; /* Checksum */
    uint32_t auto_ds_object;           /* Automatic data object */
    uint32_t debug_info_offset;        /* Offset to debug info */
    uint32_t debug_info_length;        /* Length of debug info */
    uint32_t instance_preload_count;   /* Instance pages preload */
    uint32_t instance_demand_count;    /* Instance pages demand */
    uint32_t heap_size;                /* Heap size */
    uint32_t stack_size;               /* Stack size */
} LXHeader;

/* ── LX Object Table Entry ───────────────────────────────────────────────── */

typedef struct {
    uint32_t virtual_size;        /* Virtual segment size */
    uint32_t reloc_base_addr;     /* Relocation base address */
    uint32_t object_flags;        /* Object flags (R/W/X/Shared) */
    uint32_t page_table_index;    /* Index into page table */
    uint32_t page_table_entries;  /* Number of page table entries */
    uint32_t reserved;            /* Reserved (must be 0) */
} LXObject;

/* ── LX Object Page Table Entry ──────────────────────────────────────────── */

typedef struct {
    uint32_t page_data_offset;    /* Offset to page data (high 24 bits) */
    uint16_t data_size;           /* Size of page data */
    uint16_t flags;               /* Page flags */
} LXPage;

/* ── LX Fixup Record ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  source_type;         /* Source type */
    uint8_t  flags;               /* Flags */
    uint16_t source_offset;       /* Source offset in page */
    uint16_t object;              /* Target object number */
    uint32_t target_offset;       /* Target offset */
} LXFixup;

/* ── LX Import Module Entry ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  name_length;         /* Length of module name */
    char     name[255];           /* Module name */
} LXImportModule;

/* ── LX Resident/Non-Resident Name Entry ─────────────────────────────────── */

typedef struct {
    uint8_t  name_length;         /* Length of name */
    char     name[255];           /* Name string */
    uint16_t ordinal;             /* Ordinal number */
} LXName;

/* ── LX Entry Table Bundle ───────────────────────────────────────────────── */

typedef struct {
    uint8_t  entry_count;         /* Number of entries */
    uint8_t  bundle_type;         /* Bundle type (16-bit, 32-bit, etc) */
    uint16_t object_number;       /* Object number */
} LXEntryBundle;

typedef struct {
    uint8_t  flags;               /* Entry flags */
    uint32_t offset;              /* Offset in object */
} LXEntry;

#pragma pack(pop)

/* ══════════════════════════════════════════════════════════════════════════
   LX BUILDER API
   ══════════════════════════════════════════════════════════════════════════ */

typedef struct LXBuilder LXBuilder;

/* Create new LX builder */
LXBuilder* lx_builder_new(void);
void lx_builder_free(LXBuilder *builder);

/* Set module properties */
void lx_builder_set_type(LXBuilder *builder, uint32_t flags); /* EXE or DLL */
void lx_builder_set_name(LXBuilder *builder, const char *name);
void lx_builder_set_description(LXBuilder *builder, const char *desc);
void lx_builder_set_version(LXBuilder *builder, uint32_t version);
void lx_builder_set_cpu(LXBuilder *builder, uint16_t cpu_type);

/* Add code/data sections */
int lx_builder_add_object(LXBuilder *builder, const char *name, 
                          const void *data, uint32_t size, uint32_t flags);

/* Set entry point */
void lx_builder_set_entry_point(LXBuilder *builder, uint32_t object, 
                                uint32_t offset);

/* Set stack size */
void lx_builder_set_stack_size(LXBuilder *builder, uint32_t size);

/* Export functions/variables */
int lx_builder_add_export(LXBuilder *builder, const char *name, 
                          uint32_t object, uint32_t offset, uint16_t ordinal);

/* Import functions from other modules */
int lx_builder_add_import(LXBuilder *builder, const char *module, 
                          const char *function, uint16_t ordinal);

/* Add fixup/relocation */
int lx_builder_add_fixup(LXBuilder *builder, uint32_t source_object, 
                         uint32_t source_offset, uint32_t target_object, 
                         uint32_t target_offset);

/* Write LX file */
int lx_builder_write(LXBuilder *builder, const char *filename);

/* Utility: Create an executable from two separate objects - code
 * (.text+.rodata, mapped RX) and data (.data+.bss, mapped RW+NX) - rather
 * than one flat RWX blob, so the kernel's loader (kernel/src/lx_loader.c)
 * and MMU setup (kernel/src/mmu.c) can enforce W^X between them. */
int lx_create_exe(const char *filename,
                  const void *code, uint32_t code_size,
                  const void *data, uint32_t data_size,
                  uint32_t entry_offset, uint32_t stack_size);

/* Utility: Create a DLL from two separate objects, same reasoning as
 * lx_create_exe above. offsets[i] is the real byte offset of exports[i]
 * within `code` (e.g. from an nm lookup against the linked ELF) - callers
 * must supply real addresses, not placeholders, since these become the
 * entry table's actual on-disk offsets. */
int lx_create_dll(const char *filename, const char *dll_name,
                  const void *code, uint32_t code_size,
                  const void *data, uint32_t data_size,
                  const char **exports, const uint32_t *offsets,
                  uint32_t export_count);

#endif /* LXFORMAT_H */
