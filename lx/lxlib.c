/* ============================================================================
 * lxlib.c - LX Library Utility
 * ============================================================================
 * Command-line utility for working with LX format executables and DLLs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lxlib.h"
#include "lxformat.h"

/* ── Helper Functions ────────────────────────────────────────────────────── */

static void print_usage(const char *progname)
{
    printf("LX Library Utility\n");
    printf("\n");
    printf("Usage: %s <command> <file> [options]\n", progname);
    printf("\n");
    printf("Commands:\n");
    printf("  info      Show LX file information\n");
    printf("  extract   Extract sections from LX file\n");
    printf("  verify    Verify LX file structure\n");
    printf("  symbols   List symbols/exports\n");
    printf("  convert   Convert to another format\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s info FILE.EXE\n", progname);
    printf("  %s extract FILE.EXE output_dir/\n", progname);
    printf("  %s symbols FILE.DLL\n", progname);
    printf("\n");
}

/* ── Command Implementations ─────────────────────────────────────────────── */

int lxlib_info(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file: %s\n", filename);
        return 1;
    }

    /* Read LX header */
    lx_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error: Cannot read LX header\n");
        fclose(f);
        return 1;
    }

    /* Verify magic */
    if (header.magic[0] != 'L' || header.magic[1] != 'X') {
        fprintf(stderr, "Error: Not a valid LX file\n");
        fclose(f);
        return 1;
    }

    /* Display information */
    printf("═══════════════════════════════════════════════════════════\n");
    printf("LX File Information: %s\n", filename);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("Format:         LX (Linear Executable)\n");
    printf("CPU:            0x%04X\n", header.cpu_type);
    printf("OS:             0x%04X\n", header.os_type);
    printf("Version:        %u.%u\n", header.format_level >> 8, header.format_level & 0xFF);
    printf("Flags:          0x%08X\n", header.module_flags);
    printf("\n");
    printf("Entry Point:    0x%08X (Object %u, Offset 0x%08X)\n",
           header.eip, header.eip_object, header.eip);
    printf("Stack:          0x%08X bytes (Object %u)\n",
           header.esp, header.esp_object);
    printf("\n");
    printf("Object Table:   Offset 0x%08X, %u objects\n",
           header.object_table_offset, header.module_num_objects);
    printf("Object Pages:   %u pages (0x%08X per page)\n",
           header.module_num_pages, header.page_size);
    printf("Page Data:      Offset 0x%08X\n", header.data_pages_offset);
    printf("\n");
    printf("Fixup Section:  Offset 0x%08X, Size 0x%08X\n",
           header.fixup_section_offset, header.fixup_section_size);
    printf("Loader Section: Offset 0x%08X, Size 0x%08X\n",
           header.loader_section_offset, header.loader_section_size);
    printf("\n");

    fclose(f);
    return 0;
}

int lxlib_extract(const char *filename, const char *output_dir)
{
    printf("Extracting sections from: %s\n", filename);
    printf("Output directory: %s\n", output_dir);
    printf("(Not implemented yet)\n");
    return 0;
}

int lxlib_verify(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file: %s\n", filename);
        return 1;
    }

    /* Read and verify LX header */
    lx_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fprintf(stderr, "Error: Cannot read LX header\n");
        fclose(f);
        return 1;
    }

    printf("Verifying: %s\n", filename);
    printf("\n");

    /* Check magic */
    if (header.magic[0] == 'L' && header.magic[1] == 'X') {
        printf("Magic signature valid\n");
    } else {
        printf("Invalid magic signature\n");
        fclose(f);
        return 1;
    }

    /* Check version */
    if (header.byte_order == 0) {
        printf("Byte order: Little-endian\n");
    } else {
        printf("Warning: Byte order: Unknown (0x%04X)\n", header.byte_order);
    }

    /* Check object count */
    if (header.module_num_objects > 0 && header.module_num_objects < 256) {
        printf("Object count: %u\n", header.module_num_objects);
    } else {
        printf("Warning: Object count suspicious: %u\n", header.module_num_objects);
    }

    /* Check page size */
    if (header.page_size == 4096 || header.page_size == 8192) {
        printf("Page size: %u bytes\n", header.page_size);
    } else {
        printf("Warning: Non-standard page size: %u bytes\n", header.page_size);
    }

    printf("\nLX file structure appears valid\n");

    fclose(f);
    return 0;
}

int lxlib_symbols(const char *filename)
{
    printf("Listing symbols from: %s\n", filename);
    printf("(Not implemented yet)\n");
    return 0;
}

int lxlib_convert(const char *input, const char *output, const char *format)
{
    printf("Converting: %s -> %s (format: %s)\n", input, output, format);
    printf("(Not implemented yet)\n");
    return 0;
}

/* ── Main Entry Point ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[1];
    const char *filename = argv[2];

    /* Dispatch to command handler */
    if (strcmp(command, "info") == 0) {
        return lxlib_info(filename);
    }
    else if (strcmp(command, "extract") == 0) {
        const char *output_dir = (argc >= 4) ? argv[3] : ".";
        return lxlib_extract(filename, output_dir);
    }
    else if (strcmp(command, "verify") == 0) {
        return lxlib_verify(filename);
    }
    else if (strcmp(command, "symbols") == 0) {
        return lxlib_symbols(filename);
    }
    else if (strcmp(command, "convert") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Error: convert requires input, output, and format\n");
            print_usage(argv[0]);
            return 1;
        }
        const char *output = argv[3];
        const char *format = argv[4];
        return lxlib_convert(filename, output, format);
    }
    else {
        fprintf(stderr, "Error: Unknown command: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}