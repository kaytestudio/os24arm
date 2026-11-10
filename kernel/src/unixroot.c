/* ============================================================================
 * kernel/src/unixroot.c  —  EMX-style UNIXROOT directory tree
 *
 * See kernel/include/unixroot.h for the drive-vs-VFS-root explanation.
 * ========================================================================== */

#include "unixroot.h"
#include "vfs.h"
#include "kprintf.h"
#include "string.h"

#define UNIXROOT_DRIVE_MAX 16
#define UNIXROOT_USER_MAX  32

static char g_drive[UNIXROOT_DRIVE_MAX] = UNIXROOT_DEFAULT_DRIVE;
static char g_home_user[UNIXROOT_USER_MAX] = "user";

/* ── CONFIG.SYS "SET KEY=VALUE" line lookup ──────────────────────────────── */

/* Finds a line of the form "SET <key>=<value>" (case-insensitive on both
 * "SET" and <key>) and copies <value> - up to the next CR/LF or end of
 * string - into out. Returns 0 on success, -1 if not found or out is too
 * small for the value. */
static int config_sys_get_value(const char *cfg, const char *key,
                                 char *out, size_t out_size)
{
    if (!cfg || !key || !out || out_size == 0) return -1;

    size_t key_len = strlen(key);
    const char *line = cfg;

    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        /* Skip leading whitespace on the line. */
        const char *p = line;
        size_t rem = line_len;
        while (rem > 0 && (*p == ' ' || *p == '\t')) { p++; rem--; }

        if (rem > 4 && strncasecmp(p, "SET ", 4) == 0) {
            p += 4; rem -= 4;
            while (rem > 0 && (*p == ' ' || *p == '\t')) { p++; rem--; }

            if (rem > key_len && strncasecmp(p, key, key_len) == 0 &&
                p[key_len] == '=') {
                const char *val = p + key_len + 1;
                const char *val_end = line + line_len;
                /* Trim a trailing '\r' (CONFIG.SYS is CRLF). */
                if (val_end > val && val_end[-1] == '\r') val_end--;

                size_t val_len = (size_t)(val_end - val);
                if (val_len == 0 || val_len >= out_size) return -1;

                memcpy(out, val, val_len);
                out[val_len] = '\0';
                return 0;
            }
        }

        if (!eol) break;
        line = eol + 1;
    }

    return -1;
}

void unixroot_configure(const char *config_sys_text)
{
    if (config_sys_get_value(config_sys_text, "UNIXROOT", g_drive, sizeof(g_drive)) != 0)
        strncpy(g_drive, UNIXROOT_DEFAULT_DRIVE, sizeof(g_drive) - 1);
    g_drive[sizeof(g_drive) - 1] = '\0';

    if (config_sys_get_value(config_sys_text, "USER", g_home_user, sizeof(g_home_user)) != 0)
        strncpy(g_home_user, "user", sizeof(g_home_user) - 1);
    g_home_user[sizeof(g_home_user) - 1] = '\0';

    /* Unix-style login names are conventionally lower-case. */
    for (char *c = g_home_user; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
}

const char *unixroot_drive(void)
{
    return g_drive;
}

/* ── Directory tree ──────────────────────────────────────────────────────── */

/* Fixed part of the tree, in parent-before-child order. Paths are rooted
 * at "/" - the VFS mount that UNIXROOT's drive (see unixroot_drive())
 * refers to. */
static const char *const k_unixroot_dirs[] = {
    "/bin",
    "/emx",
    "/etc",
    "/home",
    "/usr",
    "/usr/bin",
    "/usr/doc",
    "/usr/etc",
    "/usr/include",
    "/usr/info",
    "/usr/lib",
    "/usr/share",
    "/usr/share/locale",
    "/usr/share/man",
    "/tmp",
    "/var",
};

static int mkdir_logged(const char *path)
{
    int r = vfs_mkdir(path);
    if (r == VFS_OK) {
        kprintf("[UNIXROOT] %s\n", path);
        return 0;
    }
    /* Already existing is the common, harmless case on a second boot -
     * still worth a quiet log line, but not a failure. */
    kprintf("[UNIXROOT] %s: %s\n", path,
            (r == VFS_ERR_ROFS) ? "read-only filesystem" : "skipped/exists");
    return (r == VFS_ERR_ROFS) ? -1 : 0;
}

int unixroot_create_tree(void)
{
    int failures = 0;

    kprintf("[UNIXROOT] Creating tree on drive %s (VFS root /)\n", g_drive);

    for (size_t i = 0; i < sizeof(k_unixroot_dirs) / sizeof(k_unixroot_dirs[0]); i++) {
        if (mkdir_logged(k_unixroot_dirs[i]) != 0) failures++;
    }

    /* /home/<user> - the caller's login-ID directory. */
    char home_path[VFS_PATH_MAX];
    strcpy(home_path, "/home/");
    strcat(home_path, g_home_user);
    if (mkdir_logged(home_path) != 0) failures++;

    /* /usr/share/man/{man,cat}1..9 and {man,cat}n. */
    for (int section = 1; section <= 9; section++) {
        char path[VFS_PATH_MAX];
        size_t n;
        char digit = (char)('0' + section);

        strcpy(path, "/usr/share/man/man");
        n = strlen(path);
        path[n] = digit;
        path[n + 1] = '\0';
        if (mkdir_logged(path) != 0) failures++;

        strcpy(path, "/usr/share/man/cat");
        n = strlen(path);
        path[n] = digit;
        path[n + 1] = '\0';
        if (mkdir_logged(path) != 0) failures++;
    }
    if (mkdir_logged("/usr/share/man/mann") != 0) failures++;
    if (mkdir_logged("/usr/share/man/catn") != 0) failures++;

    if (failures == 0) {
        kprintf("[UNIXROOT] Tree complete.\n");
    } else {
        kprintf("[UNIXROOT] %d director%s could not be created (read-only root?)\n",
                failures, (failures == 1) ? "y" : "ies");
    }

    return failures;
}
