/**
 * @file ifs_loader.c
 * @brief IFS (Installable File System) Loader Implementation
 */

#include "ifs_loader.h"
#include "kio.h"
#include "vfs.h"
#include "uart.h"
#include <stddef.h>

/* ── Private Data ────────────────────────────────────────────────────────── */

/** @brief Registry of loaded IFS drivers */
static ifs_driver_t ifs_drivers[IFS_MAX_DRIVERS];

/** @brief Number of registered drivers */
static int ifs_count = 0;

/* ── String Utilities ────────────────────────────────────────────────────── */

/**
 * @brief Compare strings case-insensitively
 */
static int stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

/**
 * @brief Skip whitespace
 */
static const char* skip_whitespace(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/**
 * @brief Extract parameter from IFS line
 *
 * @param[in]  line   IFS line (e.g., "IFS=EXT4.IFS /CACHE:2048")
 * @param[out] name   Buffer for driver name (e.g., "EXT4")
 * @param[out] params Buffer for parameters (e.g., "/CACHE:2048")
 *
 * @return 0 on success, -1 on error
 */
static int parse_ifs_line(const char *line, char *name, char *params)
{
    // Skip "IFS="
    const char *p = line;
    if (p[0] == 'I' && p[1] == 'F' && p[2] == 'S' && p[3] == '=') {
        p += 4;
    } else {
        return -1; // Not an IFS line
    }

    p = skip_whitespace(p);

    // Extract driver name (up to .IFS)
    int i = 0;
    while (*p && *p != '.' && *p != ' ' && *p != '\t' && i < 31) {
        name[i++] = *p++;
    }
    name[i] = '\0';

    if (i == 0) return -1; // No name

    // Skip .IFS extension
    if (*p == '.') {
        p++;
        if (*p == 'I' && p[1] == 'F' && p[2] == 'S') {
            p += 3;
        }
    }

    // Extract parameters
    p = skip_whitespace(p);
    i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < 255) {
        params[i++] = *p++;
    }
    params[i] = '\0';

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ifs_loader_init(void)
{
    uart_puts("DEBUG: Entering ifs_loader_init\r\n");

    // ifs_drivers array is already zeroed by boot.S BSS clearing
    // Just initialize the counter
    ifs_count = 0;

    uart_puts("DEBUG: ifs_loader_init complete\r\n");
}

int ifs_register_builtin(const char *name, ifs_init_fn init_fn, vfs_fs_t *fs_ops)
{
    uart_puts("DEBUG: Entering ifs_register_builtin\r\n");

    if (ifs_count >= IFS_MAX_DRIVERS) {
        uart_puts("DEBUG: Too many IFS drivers\r\n");
        return -1;
    }

    if (!name || !init_fn || !fs_ops) {
        uart_puts("DEBUG: Invalid driver parameters\r\n");
        return -1;
    }

    // Register driver - simple direct assignment
    ifs_drivers[ifs_count].name = name;
    ifs_drivers[ifs_count].init = init_fn;
    ifs_drivers[ifs_count].fs_ops = fs_ops;
    ifs_drivers[ifs_count].module_base = NULL;
    ifs_drivers[ifs_count].module_size = 0;

    ifs_count++;

    uart_puts("DEBUG: ifs_register_builtin complete\r\n");
    return 0;
}


int ifs_load_from_line(const char *line)
{
    char name[32];
    char params[256];

    // Parse IFS line
    if (parse_ifs_line(line, name, params) != 0) {
        return -1; // Not an IFS line or parse error
    }

    kprintf("[IFS] Loading driver: %s", name);
    if (params[0]) {
        kprintf(" (params: %s)", params);
    }
    kprintf("\n");

    // Find driver in registry
    ifs_driver_t *drv = NULL;
    for (int i = 0; i < ifs_count; i++) {
        if (stricmp(ifs_drivers[i].name, name) == 0) {
            drv = &ifs_drivers[i];
            break;
        }
    }

    if (!drv) {
        kprintf("[IFS] ERROR: Driver '%s' not found\n", name);
        kprintf("[IFS] Available drivers:\n");
        for (int i = 0; i < ifs_count; i++) {
            kprintf("[IFS]   - %s\n", ifs_drivers[i].name);
        }
        return -1;
    }

    // Initialize driver
    int ret = drv->init(params);
    if (ret != 0) {
        kprintf("[IFS] ERROR: Driver '%s' initialization failed: %d\n", name, ret);
        return -1;
    }

    kprintf("[IFS] Driver '%s' loaded successfully\n", name);
    return 0;
}

int ifs_load_from_config(const char *config_path)
{
    kprintf("[IFS] Reading CONFIG.SYS: %s\n", config_path);

    // Open CONFIG.SYS
    int fd = vfs_open(config_path, O_RDONLY);
    if (fd < 0) {
        kprintf("[IFS] WARNING: Could not open %s\n", config_path);
        return 0; // Not a fatal error
    }

    // Read CONFIG.SYS line by line
    char line[512];
    int line_pos = 0;
    int drivers_loaded = 0;

    while (1) {
        char c;
        int bytes = vfs_read(fd, &c, 1);

        if (bytes <= 0) break; // EOF or error

        if (c == '\n' || c == '\r') {
            // End of line
            if (line_pos > 0) {
                line[line_pos] = '\0';

                // Check if IFS line
                if (line[0] == 'I' && line[1] == 'F' && line[2] == 'S' && line[3] == '=') {
                    if (ifs_load_from_line(line) == 0) {
                        drivers_loaded++;
                    }
                }

                line_pos = 0;
            }
        } else {
            // Add to line buffer
            if (line_pos < sizeof(line) - 1) {
                line[line_pos++] = c;
            }
        }
    }

    // Process last line if no newline at EOF
    if (line_pos > 0) {
        line[line_pos] = '\0';
        if (line[0] == 'I' && line[1] == 'F' && line[2] == 'S' && line[3] == '=') {
            if (ifs_load_from_line(line) == 0) {
                drivers_loaded++;
            }
        }
    }

    vfs_close(fd);

    kprintf("[IFS] Loaded %d driver(s) from CONFIG.SYS\n", drivers_loaded);
    return drivers_loaded;
}

ifs_driver_t* ifs_get_driver(const char *name)
{
    for (int i = 0; i < ifs_count; i++) {
        if (stricmp(ifs_drivers[i].name, name) == 0) {
            return &ifs_drivers[i];
        }
    }
    return NULL;
}

void ifs_list_drivers(void)
{
    kprintf("\n");
    kprintf("════════════════════════════════════════════════════════════\n");
    kprintf("Installed File Systems (IFS)\n");
    kprintf("════════════════════════════════════════════════════════════\n");

    if (ifs_count == 0) {
        kprintf("  No IFS drivers loaded\n");
    } else {
        for (int i = 0; i < ifs_count; i++) {
            kprintf("  [%d] %-12s", i + 1, ifs_drivers[i].name);
            if (ifs_drivers[i].module_base) {
                kprintf(" (dynamic @ %p)", ifs_drivers[i].module_base);
            } else {
                kprintf(" (built-in)");
            }
            kprintf("\n");
        }
    }

    kprintf("════════════════════════════════════════════════════════════\n");
    kprintf("\n");
}