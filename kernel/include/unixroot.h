/* ============================================================================
 * kernel/include/unixroot.h  —  EMX-style UNIXROOT directory tree
 *
 * Real OS/2's EMX runtime maps Unix-shaped paths ("/bin/sh") onto a real
 * OS/2 drive named by the UNIXROOT environment variable (e.g. "C:", giving
 * "C:/bin/sh"). On this port the VFS root mount "/" already *is* drive C:
 * (see kernel/src/main.c's boot log and fs/btrfs/btrfs.c's btrfs_init,
 * which mounts the boot disk at "/") - so UNIXROOT is tracked here purely
 * as the drive name CONFIG.SYS and callers see, while directory creation
 * below writes straight to the VFS root that drive already refers to.
 * ========================================================================== */

#ifndef UNIXROOT_H
#define UNIXROOT_H

/* Default drive for UNIXROOT when CONFIG.SYS has no "SET UNIXROOT=" line. */
#define UNIXROOT_DEFAULT_DRIVE "C:"

/* Scans CONFIG.SYS text (as loaded into memory, NUL-terminated) for
 * "SET UNIXROOT=<drive>" and "SET USER=<name>" lines (case-insensitive)
 * and remembers them for unixroot_drive() / unixroot_create_tree(). Either
 * or both may be absent - UNIXROOT_DEFAULT_DRIVE and "user" are used in
 * that case. Passing NULL resets both to their defaults. */
void unixroot_configure(const char *config_sys_text);

/* The currently configured UNIXROOT drive, e.g. "C:". Always non-NULL;
 * reflects the last unixroot_configure() call, or the default before the
 * first one. */
const char *unixroot_drive(void);

/* Creates the EMX/Unix directory tree (bin, emx, etc, home/<user>, usr/...,
 * tmp, var - see kernel/src/unixroot.c for the exact layout) on the VFS
 * root that UNIXROOT's drive refers to. Safe to call more than once.
 * Returns the number of directories that could not be created (0 means
 * every directory in the tree now exists) - a non-zero count is expected
 * on a read-only-mounted root (e.g. ext4's current mkdir stub). */
int unixroot_create_tree(void);

#endif /* UNIXROOT_H */
