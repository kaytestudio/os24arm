/* ============================================================================
 * tools/lxbuild.c  —  LX Executable Builder Command-Line Tool
 *
 * Usage: lxbuild [options] -o output.exe input.bin
 * ========================================================================== */

#include "lxformat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void print_usage(const char *prog)
{
    printf("LX Executable Builder for OS/2 Warp ARM64\n\n");
    printf("Usage: %s [options] -o output input.bin\n\n", prog);
    printf("Options:\n");
    printf("  -o, --output FILE       Output file (required)\n");
    printf("  -t, --type TYPE         Module type: exe, dll, sys (default: exe)\n");
    printf("  -n, --name NAME         Module name (for DLLs)\n");
    printf("  -e, --entry OFFSET      Entry point offset (hex, default: 0)\n");
    printf("  -s, --stack SIZE        Stack size in KB (default: 64)\n");
    printf("  -c, --cpu TYPE          CPU type: arm64, 386, 486 (default: arm64)\n");
    printf("  -x, --export NAME:OFF   Add export (name:offset, can be repeated)\n");
    printf("  -i, --import MOD:FUNC   Add import (module:function)\n");
    printf("  -d, --data FILE         Add data section from file\n");
    printf("  -v, --verbose           Verbose output\n");
    printf("  -h, --help              Show this help\n\n");
    printf("Examples:\n");
    printf("  # Create simple executable\n");
    printf("  %s -o HELLO.EXE --entry 0x1000 hello.bin\n\n", prog);
    printf("  # Create DLL with exports\n");
    printf("  %s -t dll -o MYLIB.DLL -n MYLIB \\\n", prog);
    printf("     --export MyFunc1:0 --export MyFunc2:100 mylib.bin\n\n");
    printf("  # Create device driver\n");
    printf("  %s -t sys -o MYDRV.SYS --entry 0 driver.bin\n\n", prog);
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    const char *input = NULL;
    const char *module_name = "UNNAMED";
    const char *data_file = NULL;
    uint32_t entry_offset = 0;
    uint32_t stack_size = 64 * 1024;  /* 64KB default */
    uint16_t cpu_type = LXCPU_ARM64;
    uint32_t module_flags = LXMF_PROGRAM;
    int verbose = 0;
    
    /* Export and import lists */
    char *exports[256] = {0};
    int export_count = 0;
    char *imports[256] = {0};
    int import_count = 0;
    
    /* Parse command line */
    static struct option long_options[] = {
        {"output",  required_argument, 0, 'o'},
        {"type",    required_argument, 0, 't'},
        {"name",    required_argument, 0, 'n'},
        {"entry",   required_argument, 0, 'e'},
        {"stack",   required_argument, 0, 's'},
        {"cpu",     required_argument, 0, 'c'},
        {"export",  required_argument, 0, 'x'},
        {"import",  required_argument, 0, 'i'},
        {"data",    required_argument, 0, 'd'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "o:t:n:e:s:c:x:i:d:vh", 
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'o':
            output = optarg;
            break;
        case 't':
            if (strcmp(optarg, "exe") == 0) {
                module_flags = LXMF_PROGRAM;
            } else if (strcmp(optarg, "dll") == 0) {
                module_flags = LXMF_LIBRARY | LXMF_INIT_INSTANCE;
            } else if (strcmp(optarg, "sys") == 0) {
                module_flags = LXMF_PHYSICAL_DEVICE;
            } else {
                fprintf(stderr, "Unknown type: %s\n", optarg);
                return 1;
            }
            break;
        case 'n':
            module_name = optarg;
            break;
        case 'e':
            entry_offset = strtoul(optarg, NULL, 0);
            break;
        case 's':
            stack_size = strtoul(optarg, NULL, 0) * 1024;
            break;
        case 'c':
            if (strcmp(optarg, "arm64") == 0) {
                cpu_type = LXCPU_ARM64;
            } else if (strcmp(optarg, "386") == 0) {
                cpu_type = LXCPU_386;
            } else if (strcmp(optarg, "486") == 0) {
                cpu_type = LXCPU_486;
            } else {
                fprintf(stderr, "Unknown CPU type: %s\n", optarg);
                return 1;
            }
            break;
        case 'x':
            if (export_count < 256) {
                exports[export_count++] = strdup(optarg);
            }
            break;
        case 'i':
            if (import_count < 256) {
                imports[import_count++] = strdup(optarg);
            }
            break;
        case 'd':
            data_file = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    /* Get input file */
    if (optind < argc) {
        input = argv[optind];
    }
    
    /* Validate arguments */
    if (!output) {
        fprintf(stderr, "Error: Output file required (-o)\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (!input) {
        fprintf(stderr, "Error: Input file required\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (verbose) {
        printf("Building LX executable...\n");
        printf("  Input:  %s\n", input);
        printf("  Output: %s\n", output);
        printf("  Type:   %s\n", 
               (module_flags & LXMF_LIBRARY) ? "DLL" : 
               (module_flags & LXMF_PHYSICAL_DEVICE) ? "SYS" : "EXE");
        printf("  CPU:    %s\n", 
               cpu_type == LXCPU_ARM64 ? "ARM64" : "x86");
        printf("  Entry:  0x%X\n", entry_offset);
        printf("  Stack:  %u KB\n", stack_size / 1024);
    }
    
    /* Read input file */
    FILE *f = fopen(input, "rb");
    if (!f) {
        perror("Failed to open input file");
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long code_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *code = malloc(code_size);
    if (!code) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        return 1;
    }
    
    fread(code, 1, code_size, f);
    fclose(f);
    
    if (verbose) {
        printf("  Code size: %ld bytes\n", code_size);
    }
    
    /* Read data file if specified */
    uint8_t *data = NULL;
    long data_size = 0;
    
    if (data_file) {
        FILE *df = fopen(data_file, "rb");
        if (!df) {
            perror("Failed to open data file");
            free(code);
            return 1;
        }
        
        fseek(df, 0, SEEK_END);
        data_size = ftell(df);
        fseek(df, 0, SEEK_SET);
        
        data = malloc(data_size);
        if (data) {
            fread(data, 1, data_size, df);
        }
        fclose(df);
        
        if (verbose && data) {
            printf("  Data size: %ld bytes\n", data_size);
        }
    }
    
    /* Create builder */
    LXBuilder *builder = lx_builder_new();
    if (!builder) {
        fprintf(stderr, "Failed to create builder\n");
        free(code);
        if (data) free(data);
        return 1;
    }
    
    /* Configure builder */
    lx_builder_set_type(builder, module_flags);
    lx_builder_set_name(builder, module_name);
    lx_builder_set_cpu(builder, cpu_type);
    lx_builder_set_entry_point(builder, 1, entry_offset);
    lx_builder_set_stack_size(builder, stack_size);
    
    /* Add code section */
    lx_builder_add_object(builder, ".text", code, code_size,
                         LXOBJ_READABLE | LXOBJ_EXECUTABLE);
    
    /* Add data section if present */
    if (data && data_size > 0) {
        lx_builder_add_object(builder, ".data", data, data_size,
                             LXOBJ_READABLE | LXOBJ_WRITABLE);
    } else {
        /* Add empty data section */
        lx_builder_add_object(builder, ".data", NULL, 4096,
                             LXOBJ_READABLE | LXOBJ_WRITABLE);
    }
    
    /* Process exports */
    for (int i = 0; i < export_count; i++) {
        char *exp = exports[i];
        char *colon = strchr(exp, ':');
        
        if (colon) {
            *colon = '\0';
            uint32_t offset = strtoul(colon + 1, NULL, 0);
            
            if (verbose) {
                printf("  Export: %s @ 0x%X\n", exp, offset);
            }
            
            lx_builder_add_export(builder, exp, 1, offset, i + 1);
        }
        
        free(exports[i]);
    }
    
    /* Process imports */
    for (int i = 0; i < import_count; i++) {
        char *imp = imports[i];
        char *colon = strchr(imp, ':');
        
        if (colon) {
            *colon = '\0';
            char *module = imp;
            char *function = colon + 1;
            
            if (verbose) {
                printf("  Import: %s from %s\n", function, module);
            }
            
            lx_builder_add_import(builder, module, function, 0);
        }
        
        free(imports[i]);
    }
    
    /* Write output */
    int result = lx_builder_write(builder, output);
    
    /* Cleanup */
    lx_builder_free(builder);
    free(code);
    if (data) free(data);
    
    if (result == 0) {
        if (verbose) {
            printf("\nSuccessfully created %s\n", output);
        }
    } else {
        fprintf(stderr, "\nFailed to create %s\n", output);
    }
    
    return result;
}
