/* ============================================================================
 * tools/makeexe.c  —  Simple EXE Builder
 *
 * Usage: makeexe code.bin data.bin output.exe entry_offset
 * ========================================================================== */

#include "lxformat.h"
#include <stdio.h>
#include <stdlib.h>

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
        printf("Usage: %s code.bin data.bin output.exe entry_offset\n", argv[0]);
        printf("\n");
        printf("Creates an OS/2 LX executable from two separately-extracted\n");
        printf("binaries - code (.text+.rodata) and data (.data+.bss) - so the\n");
        printf("kernel's loader/MMU can map them RX/RW+NX respectively (W^X).\n");
        printf("Extract each with e.g.:\n");
        printf("  objcopy -O binary --only-section=.text --only-section=.rodata a.elf code.bin\n");
        printf("  objcopy -O binary --only-section=.data --only-section=.bss    a.elf data.bin\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  code.bin       - Extracted .text+.rodata\n");
        printf("  data.bin       - Extracted .data+.bss\n");
        printf("  output.exe     - Output LX executable\n");
        printf("  entry_offset   - Entry point offset within code.bin (hex)\n");
        printf("\n");
        printf("Example:\n");
        printf("  %s code.bin data.bin HELLO.EXE 0x1000\n", argv[0]);
        return 1;
    }

    const char *code_path = argv[1];
    const char *data_path = argv[2];
    const char *output = argv[3];
    uint32_t entry_offset = strtoul(argv[4], NULL, 0);

    long code_size, data_size;
    uint8_t *code = read_file(code_path, &code_size);
    if (!code) return 1;
    uint8_t *data = read_file(data_path, &data_size);
    if (!data) { free(code); return 1; }

    printf("Creating executable...\n");
    printf("  Code:   %s (%ld bytes)\n", code_path, code_size);
    printf("  Data:   %s (%ld bytes)\n", data_path, data_size);
    printf("  Output: %s\n", output);
    printf("  Entry:  0x%X\n", entry_offset);

    /* Create EXE */
    int result = lx_create_exe(output, code, (uint32_t)code_size, data, (uint32_t)data_size,
                                entry_offset, 0x10000);

    free(code);
    free(data);

    if (result == 0) {
        printf("✓ Successfully created %s\n", output);
    } else {
        printf("✗ Failed to create executable\n");
    }

    return result;
}
