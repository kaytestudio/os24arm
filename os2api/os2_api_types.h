/* ============================================================================
 * os2api/os2_api_types.h  —  OS/2 DOS-call API: shared types and constants
 *
 * Split out of os2_api.h so a caller that wants os2_api_import.h's
 * function-*pointer* declarations (for DosOpen et al, resolved at runtime
 * from an embedded DOSCALLS.DLL - see os2api/os2_api_import.h) can get the
 * types/constants those pointers are declared in terms of without also
 * pulling in os2_api.h's plain-function prototypes of the same names,
 * which would conflict.
 * ========================================================================== */

#ifndef OS2_API_TYPES_H
#define OS2_API_TYPES_H

#include "types.h"

/* ── VFS Constants ────────────────────────────────────────────────────── */

#ifndef VFS_PATH_MAX
#define VFS_PATH_MAX        260     /* Maximum path length (DOS/OS2 compat) */
#endif

#ifndef VFS_NAME_MAX
#define VFS_NAME_MAX        255     /* Maximum filename length */
#endif

#ifndef SECTOR_SIZE
#define SECTOR_SIZE         512     /* Standard sector size */
#endif

/* File attributes (DOS/OS2 compatible) */
#ifndef VFS_ATTR_RDONLY
#define VFS_ATTR_RDONLY     0x01    /* Read-only */
#define VFS_ATTR_HIDDEN     0x02    /* Hidden */
#define VFS_ATTR_SYSTEM     0x04    /* System */
#define VFS_ATTR_VOLUME     0x08    /* Volume label */
#define VFS_ATTR_DIR        0x10    /* Directory */
#define VFS_ATTR_ARCHIVE    0x20    /* Archive */
#endif

/* File open modes (DOS compatible; matches vfs.h's O_* exactly) */
#ifndef O_RDONLY
#define O_RDONLY            0x0000
#define O_WRONLY            0x0001
#define O_RDWR              0x0002
#define O_CREAT             0x0040
#define O_TRUNC             0x0200
#define O_APPEND            0x0400
#endif

/* Seek whence values */
#ifndef SEEK_SET
#define SEEK_SET            0
#define SEEK_CUR            1
#define SEEK_END            2
#endif

/* ── DOS-Style Command Codes ─────────────────────────────────────────── */

#define DOS_CMD_DIR         0x01    /* Directory listing */
#define DOS_CMD_COPY        0x02    /* Copy file */
#define DOS_CMD_DEL         0x03    /* Delete file */
#define DOS_CMD_REN         0x04    /* Rename file */
#define DOS_CMD_TYPE        0x05    /* Display file contents */
#define DOS_CMD_MD          0x06    /* Make directory */
#define DOS_CMD_RD          0x07    /* Remove directory */
#define DOS_CMD_CD          0x08    /* Change directory */
#define DOS_CMD_VOL         0x09    /* Volume label */
#define DOS_CMD_CHKDSK      0x0A    /* Check disk */
#define DOS_CMD_FORMAT      0x0B    /* Format disk */
#define DOS_CMD_ATTRIB      0x0C    /* File attributes */

/* ── OS/2 API Types ──────────────────────────────────────────────────── */

typedef uint32_t HFILE;             /* File handle */
typedef uint32_t HDIR;              /* Directory search handle */

/* Standard OS/2 API return codes */
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_PATH_NOT_FOUND        3
#define ERROR_ACCESS_DENIED         5
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_INVALID_DRIVE         15
#define ERROR_NO_MORE_FILES         18
#define ERROR_WRITE_PROTECT         19
#define ERROR_NOT_DOS_DISK          26
#define ERROR_SHARING_VIOLATION     32
#define ERROR_HANDLE_EOF            38
#define ERROR_HANDLE_DISK_FULL      39
#define ERROR_NOT_SUPPORTED         50
#define ERROR_INVALID_PARAMETER     87
#define ERROR_BUFFER_OVERFLOW       111
#define ERROR_FILENAME_EXCED_RANGE  206
#define ERROR_DIRECTORY             267

/* ── OS/2 File Information Structures ───────────────────────────────── */

/* File information level 1 (FIL_STANDARD) */
typedef struct {
    uint16_t  creation_date;
    uint16_t  creation_time;
    uint16_t  access_date;
    uint16_t  access_time;
    uint16_t  write_date;
    uint16_t  write_time;
    uint32_t  file_size;
    uint32_t  alloc_size;
    uint16_t  attributes;
} FILESTATUS;

/* File find buffer (for DosFindFirst/DosFindNext) */
typedef struct {
    uint16_t  creation_date;
    uint16_t  creation_time;
    uint16_t  access_date;
    uint16_t  access_time;
    uint16_t  write_date;
    uint16_t  write_time;
    uint32_t  file_size;
    uint32_t  alloc_size;
    uint16_t  attributes;
    uint8_t   name_len;
    char      name[VFS_NAME_MAX];
} FILEFINDBUF;

#endif /* OS2_API_TYPES_H */
