/* ============================================================================
 * tools/makedll.c  —  Simple DLL Builder
 *
 * Usage: makedll code.bin data.bin output.dll dllname [name:offset ...]
 * ========================================================================== */

#include "lxformat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(size > 0 ? size : 1);
    if (!buf) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        return NULL;
    }
    if (size > 0)
        fread(buf, 1, size, f);
    fclose(f);

    *out_size = size;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        printf("Usage: %s code.bin data.bin output.dll dllname [name:offset ...]\n", argv[0]);
        printf("\n");
        printf("Creates an OS/2 LX DLL from two separately-extracted binaries -\n");
        printf("code (.text+.rodata) and data (.data+.bss) - so the kernel's\n");
        printf("loader/MMU can map them RX/RW+NX respectively (W^X). Extract each\n");
        printf("with e.g.:\n");
        printf("  objcopy -O binary --only-section=.text --only-section=.rodata a.elf code.bin\n");
        printf("  objcopy -O binary --only-section=.data --only-section=.bss    a.elf data.bin\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  code.bin     - Extracted .text+.rodata\n");
        printf("  data.bin     - Extracted .data+.bss\n");
        printf("  output.dll   - Output LX DLL\n");
        printf("  dllname      - DLL module name (uppercase, no extension)\n");
        printf("  name:offset  - Exported function name and its real byte offset\n");
        printf("                 within code.bin (e.g. from lx_export_offsets.py)\n");
        printf("\n");
        printf("Example:\n");
        printf("  %s code.bin data.bin MYLIB.DLL MYLIB MyFunc1:0 MyFunc2:128\n", argv[0]);
        return 1;
    }

    const char *code_path = argv[1];
    const char *data_path = argv[2];
    const char *output = argv[3];
    const char *dllname = argv[4];

    /* Collect exports: each argv[i] is "name:offset" */
    static char names_storage[256][128];
    const char *exports[256];
    uint32_t offsets[256];
    int export_count = 0;

    for (int i = 5; i < argc && export_count < 256; i++) {
        const char *colon = strrchr(argv[i], ':');
        if (!colon) {
            fprintf(stderr, "Bad export argument '%s' (expected name:offset)\n", argv[i]);
            return 1;
        }
        size_t name_len = (size_t)(colon - argv[i]);
        if (name_len >= sizeof(names_storage[0])) {
            fprintf(stderr, "Export name too long in '%s'\n", argv[i]);
            return 1;
        }
        memcpy(names_storage[export_count], argv[i], name_len);
        names_storage[export_count][name_len] = '\0';
        exports[export_count] = names_storage[export_count];
        offsets[export_count] = (uint32_t)strtoul(colon + 1, NULL, 10);
        export_count++;
    }

    long code_size, data_size;
    uint8_t *code = read_file(code_path, &code_size);
    if (!code) return 1;
    uint8_t *data = read_file(data_path, &data_size);
    if (!data) { free(code); return 1; }

    printf("Creating DLL...\n");
    printf("  Code:    %s (%ld bytes)\n", code_path, code_size);
    printf("  Data:    %s (%ld bytes)\n", data_path, data_size);
    printf("  Output:  %s\n", output);
    printf("  Name:    %s\n", dllname);
    printf("  Exports: %d\n", export_count);

    for (int i = 0; i < export_count; i++) {
        printf("    [%d] %s @ offset %u\n", i + 1, exports[i], offsets[i]);
    }

    /* Create DLL */
    int result = lx_create_dll(output, dllname, code, (uint32_t)code_size, data, (uint32_t)data_size,
                               exports, offsets, export_count);

    free(code);
    free(data);

    if (result == 0) {
        printf("✓ Successfully created %s\n", output);
    } else {
        printf("✗ Failed to create DLL\n");
    }

    return result;
}
